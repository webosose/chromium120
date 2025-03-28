import { GPUTest } from '../../../gpu_test';
import { checkElementsPassPredicate } from '../../../util/check_contents.js';

/* All buffer sizes are counted in units of 4-byte words. */

/**
 * The value type loaded and stored from memory.
 * This is what the WGSL spec calls 'store type' for the locations being accessed.
 * The GPU buffers are sized assuming this type is at most 4 bytes.
 *
 * 'u32' is the default case; it can be atomically loaded and stored.
 * 'f16' is interesting because it is not 32-bits, and can't be the store type
 * for atomic accesses.
 */
export type AccessValueType = 'f16' | 'u32';
export const kAccessValueTypes = ['f16', 'u32'] as const;

/* Parameter values are set heuristically, typically by a time-intensive search. */
export type MemoryModelTestParams = {
  /* Number of invocations per workgroup. The workgroups are 1-dimensional. */
  workgroupSize: number;
  /** The number of workgroups to assign to running the test. */
  testingWorkgroups: number;
  /**
   * Run no more than this many workgroups. Must be >= the number of testing workgroups. Non-testing workgroups are used
   * to stress other memory locations.
   */
  maxWorkgroups: number;
  /** The percentage of iterations to shuffle the workgroup ids. */
  shufflePct: number;
  /** The percentage of iterations to run the bounded spin-loop barrier. */
  barrierPct: number;
  /** The percentage of iterations to run memory stress using non-testing workgroups. */
  memStressPct: number;
  /** The number of iterations to run the memory stress pattern. */
  memStressIterations: number;
  /** The percentage of iterations the first instruction in the stress pattern should be a store. */
  memStressStoreFirstPct: number;
  /** The percentage of iterations the second instruction in the stress pattern should be a store. */
  memStressStoreSecondPct: number;
  /** The percentage of iterations for testing threads to run stress before running the test. */
  preStressPct: number;
  /** Same as for memStressIterations. */
  preStressIterations: number;
  /** The percentage of iterations the first instruction in the pre-stress pattern should be a store. */
  preStressStoreFirstPct: number;
  /** The percentage of iterations the second instruction in the pre-stress pattern should be a store. */
  preStressStoreSecondPct: number;
  /** The size of the scratch memory region, used for stressing threads. */
  scratchMemorySize: number;
  /** The size of each block of memory stressing threads access. */
  stressLineSize: number;
  /** The number of blocks of memory to assign stressing threads to. */
  stressTargetLines: number;
  /** How non-testing threads are assigned to stressing locations. 100 means all iterations use a round robin approach, 0 means all use a chunking approach. */
  stressStrategyBalancePct: number;
  /** Used to permute thread ids within a workgroup, so more random pairings are created between threads coordinating on a test. */
  permuteFirst: number;
  /** Used to create distance between memory locations used in a test. Set this to 1 for memory that should be aliased. */
  permuteSecond: number;
  /** The distance (in number of 4 byte intervals) between any two memory locations used for testing. */
  memStride: number;
  /** For tests that access one memory location, but use dynamic addresses to avoid compiler optimization, aliased memory should be set to true. */
  aliasedMemory: boolean;
  /** The number of possible behaviors that a test can have. */
  numBehaviors: number;
};

/** The number of memory locations accessed by a test. Currently, only tests with up to 2 memory locations are supported. */
const numMemLocations = 2;

/** The number of read outputs per test that need to be analyzed in the result aggregation shader. Currently, only tests with up to 2 read outputs are supported. */
const numReadOutputs = 2;

/** Represents a device buffer and a utility buffer for resetting memory and copying parameters. */
type BufferWithSource = {
  /** Buffer used by shader code. */
  deviceBuf: GPUBuffer;
  /** Buffer populated from the host size, data is copied to device buffer for use by shader. */
  srcBuf: GPUBuffer;
  /** Size in bytes of the buffer. */
  size: number;
};

/** Specifies the buffers used during a memory model test. */
type MemoryModelBuffers = {
  /** This is the memory region that testing threads read from and write to. */
  testLocations: BufferWithSource;
  /** This buffer collects the results of reads for analysis in the result aggregation shader. */
  readResults: BufferWithSource;
  /** This buffer is the aggregated results of every testing thread, and is used to check for test success/failure. */
  testResults: BufferWithSource;
  /** This buffer stores the shuffled workgroup ids for use during testing. Read-only in the shader. */
  shuffledWorkgroups: BufferWithSource;
  /** This is the bounded spin-loop barrier, used to temporally align testing threads. */
  barrier: BufferWithSource;
  /** Memory region for stressing threads to read to and write from. */
  scratchpad: BufferWithSource;
  /** The memory locations in the scratch region that stressing threads access. */
  scratchMemoryLocations: BufferWithSource;
  /** Parameters that are used by the shader to calculate memory locations and perform stress. */
  stressParams: BufferWithSource;
};

/** The number of stress params to add to the stress params buffer. */
const numStressParams = 12;
const barrierParamIndex = 0;
const memStressIndex = 1;
const memStressIterationsIndex = 2;
const memStressPatternIndex = 3;
const preStressIndex = 4;
const preStressIterationsIndex = 5;
const preStressPatternIndex = 6;
const permuteFirstIndex = 7;
const permuteSecondIndex = 8;
const testingWorkgroupsIndex = 9;
const memStrideIndex = 10;
const memLocationOffsetIndex = 11;

/**
 * All memory used in these consists of a four byte word, so this value is used to correctly set the byte size of buffers that
 * are read to/written from during tests and for storing test results.
 */
const bytesPerWord = 4;

/**
 * Returns the shader preamble based on the access value type:
 *  - enable directives, if necessary
 *  - the type alias for AccessValueType
 */
function shaderPreamble(accessValueType: AccessValueType): string {
  if (accessValueType === 'f16') {
    return 'enable f16;\nalias AccessValueTy = f16;\n';
  }
  return `alias AccessValueTy = ${accessValueType};\n`;
}

/**
 * Implements setup code necessary to run a memory model test. A test consists of two parts:
 *  1.) A test shader that runs a specified memory model litmus test and attempts to reveal a weak (disallowed) behavior.
 *      At a high level, a test shader consists of a set of testing workgroups where every invocation executes the litmus test
 *      on a set of test locations, and a set of stressing workgroups where every invocation accesses a specified memory location
 *      in a random pattern.
 *
 *      The main buffer variables are:
 *
 *        `test_locations`: invocations access entries in this array, trying to
 *          evoke weak behaviours.
 *
 *          This is array<AccessValueTy> or array<atomic<u32>>.
 *          AccessValueTy is either f16 or u32.
 *          Note that atomic<u32> is only used when AccessValueTy is u32.
 *
 *        `results`: holds the observed values, which is where we can see
 *          whether a weak behaviour was observed.
 *
 *          This is an array<atomic<u32>>.
 *
 *      The others are used to parameterize and stress the main activity.
 *
 *  2.) A result shader that takes the output of the test shader, which consists of the memory locations accessed during the test
 *      and the results of any reads made during the test, and aggregate the results based on the possible behaviors of the test.
 *
 *      The first two buffer variables are the same buffers as for the test shader:
 *
 *        `test_locations` is the same as `test_locations` from the test shader,
 *        but is mapped as array<AccessValueTy>.
 *
 *        `read_results` is the same buffer as `results` from the test shader.
 *
 *      The other variables are used to accumulate a summary that counts the weak behaviours stimulated and recorded by the
 *      test shader.
 */
export class MemoryModelTester {
  protected test: GPUTest;
  protected params: MemoryModelTestParams;
  protected buffers: MemoryModelBuffers;
  protected testPipeline: GPUComputePipeline;
  protected testBindGroup: GPUBindGroup;
  protected resultPipeline: GPUComputePipeline;
  protected resultBindGroup: GPUBindGroup;

  /** Sets up a memory model test by initializing buffers and pipeline layouts. */
  constructor(
    t: GPUTest,
    params: MemoryModelTestParams,
    testShader: string,
    resultShader: string,
    accessValueType: AccessValueType = 'u32'
  ) {
    this.test = t;
    this.params = params;

    testShader = shaderPreamble(accessValueType) + testShader;
    resultShader = shaderPreamble(accessValueType) + resultShader;

    // set up buffers
    const testingThreads = this.params.workgroupSize * this.params.testingWorkgroups;
    const testLocationsSize =
      testingThreads * numMemLocations * this.params.memStride * bytesPerWord;
    const testLocationsBuffer: BufferWithSource = {
      deviceBuf: this.test.device.createBuffer({
        size: testLocationsSize,
        usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.STORAGE,
      }),
      srcBuf: this.test.makeBufferWithContents(
        new Uint32Array(testLocationsSize).fill(0),
        GPUBufferUsage.COPY_SRC
      ),
      size: testLocationsSize,
    };

    const readResultsSize = testingThreads * numReadOutputs * bytesPerWord;
    const readResultsBuffer: BufferWithSource = {
      deviceBuf: this.test.device.createBuffer({
        size: readResultsSize,
        usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.STORAGE,
      }),
      srcBuf: this.test.makeBufferWithContents(
        new Uint32Array(readResultsSize).fill(0),
        GPUBufferUsage.COPY_SRC
      ),
      size: readResultsSize,
    };

    const testResultsSize = this.params.numBehaviors * bytesPerWord;
    const testResultsBuffer: BufferWithSource = {
      deviceBuf: this.test.device.createBuffer({
        size: testResultsSize,
        usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC,
      }),
      srcBuf: this.test.makeBufferWithContents(
        new Uint32Array(testResultsSize).fill(0),
        GPUBufferUsage.COPY_SRC
      ),
      size: testResultsSize,
    };

    const shuffledWorkgroupsSize = this.params.maxWorkgroups * bytesPerWord;
    const shuffledWorkgroupsBuffer: BufferWithSource = {
      deviceBuf: this.test.device.createBuffer({
        size: shuffledWorkgroupsSize,
        usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.STORAGE,
      }),
      srcBuf: this.test.device.createBuffer({
        size: shuffledWorkgroupsSize,
        usage: GPUBufferUsage.COPY_SRC | GPUBufferUsage.MAP_WRITE,
      }),
      size: shuffledWorkgroupsSize,
    };

    const barrierSize = bytesPerWord;
    const barrierBuffer: BufferWithSource = {
      deviceBuf: this.test.device.createBuffer({
        size: barrierSize,
        usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.STORAGE,
      }),
      srcBuf: this.test.makeBufferWithContents(
        new Uint32Array(barrierSize).fill(0),
        GPUBufferUsage.COPY_SRC
      ),
      size: barrierSize,
    };

    const scratchpadSize = this.params.scratchMemorySize * bytesPerWord;
    const scratchpadBuffer: BufferWithSource = {
      deviceBuf: this.test.device.createBuffer({
        size: scratchpadSize,
        usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.STORAGE,
      }),
      srcBuf: this.test.makeBufferWithContents(
        new Uint32Array(scratchpadSize).fill(0),
        GPUBufferUsage.COPY_SRC
      ),
      size: scratchpadSize,
    };

    const scratchMemoryLocationsSize = this.params.maxWorkgroups * bytesPerWord;
    const scratchMemoryLocationsBuffer: BufferWithSource = {
      deviceBuf: this.test.device.createBuffer({
        size: scratchMemoryLocationsSize,
        usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.STORAGE,
      }),
      srcBuf: this.test.device.createBuffer({
        size: scratchMemoryLocationsSize,
        usage: GPUBufferUsage.COPY_SRC | GPUBufferUsage.MAP_WRITE,
      }),
      size: scratchMemoryLocationsSize,
    };

    const stressParamsSize = numStressParams * bytesPerWord;
    const stressParamsBuffer: BufferWithSource = {
      deviceBuf: this.test.device.createBuffer({
        size: stressParamsSize,
        usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.UNIFORM,
      }),
      srcBuf: this.test.device.createBuffer({
        size: stressParamsSize,
        usage: GPUBufferUsage.COPY_SRC | GPUBufferUsage.MAP_WRITE,
      }),
      size: stressParamsSize,
    };

    this.buffers = {
      testLocations: testLocationsBuffer,
      readResults: readResultsBuffer,
      testResults: testResultsBuffer,
      shuffledWorkgroups: shuffledWorkgroupsBuffer,
      barrier: barrierBuffer,
      scratchpad: scratchpadBuffer,
      scratchMemoryLocations: scratchMemoryLocationsBuffer,
      stressParams: stressParamsBuffer,
    };

    // set up pipeline layouts
    const testLayout = this.test.device.createBindGroupLayout({
      entries: [
        { binding: 0, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'storage' } },
        { binding: 1, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'storage' } },
        { binding: 2, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'read-only-storage' } },
        { binding: 3, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'storage' } },
        { binding: 4, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'storage' } },
        { binding: 5, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'storage' } },
        { binding: 6, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'uniform' } },
      ],
    });
    this.testPipeline = this.test.device.createComputePipeline({
      layout: this.test.device.createPipelineLayout({
        bindGroupLayouts: [testLayout],
      }),
      compute: {
        module: this.test.device.createShaderModule({
          code: testShader,
        }),
        entryPoint: 'main',
      },
    });
    this.testBindGroup = this.test.device.createBindGroup({
      entries: [
        { binding: 0, resource: { buffer: this.buffers.testLocations.deviceBuf } },
        { binding: 1, resource: { buffer: this.buffers.readResults.deviceBuf } },
        { binding: 2, resource: { buffer: this.buffers.shuffledWorkgroups.deviceBuf } },
        { binding: 3, resource: { buffer: this.buffers.barrier.deviceBuf } },
        { binding: 4, resource: { buffer: this.buffers.scratchpad.deviceBuf } },
        { binding: 5, resource: { buffer: this.buffers.scratchMemoryLocations.deviceBuf } },
        { binding: 6, resource: { buffer: this.buffers.stressParams.deviceBuf } },
      ],
      layout: testLayout,
    });

    const resultLayout = this.test.device.createBindGroupLayout({
      entries: [
        { binding: 0, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'storage' } },
        { binding: 1, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'storage' } },
        { binding: 2, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'storage' } },
        { binding: 3, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'uniform' } },
      ],
    });
    this.resultPipeline = this.test.device.createComputePipeline({
      layout: this.test.device.createPipelineLayout({
        bindGroupLayouts: [resultLayout],
      }),
      compute: {
        module: this.test.device.createShaderModule({
          code: resultShader,
        }),
        entryPoint: 'main',
      },
    });
    this.resultBindGroup = this.test.device.createBindGroup({
      entries: [
        { binding: 0, resource: { buffer: this.buffers.testLocations.deviceBuf } },
        { binding: 1, resource: { buffer: this.buffers.readResults.deviceBuf } },
        { binding: 2, resource: { buffer: this.buffers.testResults.deviceBuf } },
        { binding: 3, resource: { buffer: this.buffers.stressParams.deviceBuf } },
      ],
      layout: resultLayout,
    });
  }

  /**
   * Run the test for the specified number of iterations. Checks the testResults buffer on the weakIndex; if
   * this value is not 0 then the test has failed. The number of iterations is chosen per test so that the
   * full set of tests meets some time budget while still being reasonably effective at uncovering issues.
   * Currently, we aim for each test to complete in under one second.
   */
  async run(iterations: number, weakIndex: number): Promise<void> {
    for (let i = 0; i < iterations; i++) {
      const numWorkgroups = this.getRandomInRange(
        this.params.testingWorkgroups,
        this.params.maxWorkgroups
      );
      await this.setShuffledWorkgroups(numWorkgroups);
      await this.setScratchLocations(numWorkgroups);
      await this.setStressParams();
      const encoder = this.test.device.createCommandEncoder();
      this.copyBufferToBuffer(encoder, this.buffers.testLocations);
      this.copyBufferToBuffer(encoder, this.buffers.readResults);
      this.copyBufferToBuffer(encoder, this.buffers.testResults);
      this.copyBufferToBuffer(encoder, this.buffers.barrier);
      this.copyBufferToBuffer(encoder, this.buffers.shuffledWorkgroups);
      this.copyBufferToBuffer(encoder, this.buffers.scratchpad);
      this.copyBufferToBuffer(encoder, this.buffers.scratchMemoryLocations);
      this.copyBufferToBuffer(encoder, this.buffers.stressParams);

      const testPass = encoder.beginComputePass();
      testPass.setPipeline(this.testPipeline);
      testPass.setBindGroup(0, this.testBindGroup);
      testPass.dispatchWorkgroups(numWorkgroups);
      testPass.end();

      const resultPass = encoder.beginComputePass();
      resultPass.setPipeline(this.resultPipeline);
      resultPass.setBindGroup(0, this.resultBindGroup);
      resultPass.dispatchWorkgroups(this.params.testingWorkgroups);
      resultPass.end();

      this.test.device.queue.submit([encoder.finish()]);
      this.test.expectGPUBufferValuesPassCheck(
        this.buffers.testResults.deviceBuf,
        this.checkWeakIndex(weakIndex),
        {
          type: Uint32Array,
          typedLength: this.params.numBehaviors,
        }
      );
    }
  }

  /** Returns a function that checks whether the test passes, given a weak index and the test results buffer. */
  protected checkWeakIndex(weakIndex: number): (a: Uint32Array) => Error | undefined {
    const checkResult = this.checkResult(weakIndex);
    const resultPrinter = this.resultPrinter(weakIndex);
    return function (a: Uint32Array): Error | undefined {
      return checkElementsPassPredicate(a, checkResult, {
        predicatePrinter: [{ leftHeader: 'expected ==', getValueForCell: resultPrinter }],
      });
    };
  }

  /**
   * Returns a function that checks whether the specified weak index's value is not equal to 0.
   * If the weak index's value is not 0, it means the test has observed a behavior disallowed by the memory model and
   * is considered a test failure.
   */
  protected checkResult(weakIndex: number): (i: number, v: number) => boolean {
    return function (i: number, v: number): boolean {
      if (i === weakIndex && v > 0) {
        return false;
      }
      return true;
    };
  }

  /** Returns a printer function that visualizes the results of checking the test results. */
  protected resultPrinter(weakIndex: number): (i: number) => string | number {
    return function (i: number): string | number {
      if (i === weakIndex) {
        return 0;
      } else {
        return 'any value';
      }
    };
  }

  /** Utility method that simplifies copying source buffers to device buffers. */
  protected copyBufferToBuffer(encoder: GPUCommandEncoder, buffer: BufferWithSource): void {
    encoder.copyBufferToBuffer(buffer.srcBuf, 0, buffer.deviceBuf, 0, buffer.size);
  }

  /** Returns a random integer between 0 and the max. */
  protected getRandomInt(max: number): number {
    return Math.floor(Math.random() * max);
  }

  /** Returns a random number in between the min and max values. */
  protected getRandomInRange(min: number, max: number): number {
    if (min === max) {
      return min;
    } else {
      const offset = this.getRandomInt(max - min);
      return min + offset;
    }
  }

  /** Returns a permuted array using a simple Fisher-Yates shuffle algorithm. */
  protected shuffleArray(a: number[]): void {
    for (let i = a.length - 1; i >= 0; i--) {
      const toSwap = this.getRandomInt(i + 1);
      const temp = a[toSwap];
      a[toSwap] = a[i];
      a[i] = temp;
    }
  }

  /**
   * Shuffles the order of workgroup ids, so that threads operating on the same memory location are not always in
   * consecutive workgroups.
   */
  protected async setShuffledWorkgroups(numWorkgroups: number): Promise<void> {
    await this.buffers.shuffledWorkgroups.srcBuf.mapAsync(GPUMapMode.WRITE);
    const shuffledWorkgroupsBuffer = this.buffers.shuffledWorkgroups.srcBuf.getMappedRange();
    const shuffledWorkgroupsArray = new Uint32Array(shuffledWorkgroupsBuffer);
    for (let i = 0; i < numWorkgroups; i++) {
      shuffledWorkgroupsArray[i] = i;
    }
    if (this.getRandomInt(100) < this.params.shufflePct) {
      for (let i = numWorkgroups - 1; i > 0; i--) {
        const x = this.getRandomInt(i + 1);
        const temp = shuffledWorkgroupsArray[i];
        shuffledWorkgroupsArray[i] = shuffledWorkgroupsArray[x];
        shuffledWorkgroupsArray[x] = temp;
      }
    }
    this.buffers.shuffledWorkgroups.srcBuf.unmap();
  }

  /** Sets the memory locations that stressing workgroups will access. Uses either a chunking or round robin assignment strategy. */
  protected async setScratchLocations(numWorkgroups: number): Promise<void> {
    await this.buffers.scratchMemoryLocations.srcBuf.mapAsync(GPUMapMode.WRITE);
    const scratchLocationsArrayBuffer = this.buffers.scratchMemoryLocations.srcBuf.getMappedRange();
    const scratchLocationsArray = new Uint32Array(scratchLocationsArrayBuffer);
    const scratchNumRegions = this.params.scratchMemorySize / this.params.stressLineSize;
    const scratchRegions = [...Array(scratchNumRegions).keys()];
    this.shuffleArray(scratchRegions);
    for (let i = 0; i < this.params.stressTargetLines; i++) {
      const region = scratchRegions[i];
      const locInRegion = this.getRandomInt(this.params.stressLineSize);
      if (this.getRandomInt(100) < this.params.stressStrategyBalancePct) {
        // In the round-robin case, the current scratch location is striped across all workgroups.
        for (let j = i; j < numWorkgroups; j += this.params.stressTargetLines) {
          scratchLocationsArray[j] = region * this.params.stressLineSize + locInRegion;
        }
      } else {
        // In the chunking case, the current scratch location is assigned to a block of workgroups. The final scratch
        // location may be assigned to more workgroups, if the number of scratch locations does not cleanly divide the
        // number of workgroups.
        const workgroupsPerLocation = numWorkgroups / this.params.stressTargetLines;
        for (let j = 0; j < workgroupsPerLocation; j++) {
          scratchLocationsArray[i * workgroupsPerLocation + j] =
            region * this.params.stressLineSize + locInRegion;
        }
        if (
          i === this.params.stressTargetLines - 1 &&
          numWorkgroups % this.params.stressTargetLines !== 0
        ) {
          for (let j = 0; j < numWorkgroups % this.params.stressTargetLines; j++) {
            scratchLocationsArray[numWorkgroups - j - 1] =
              region * this.params.stressLineSize + locInRegion;
          }
        }
      }
    }
    this.buffers.scratchMemoryLocations.srcBuf.unmap();
  }

  /** Sets the parameters that are used by the shader to calculate memory locations and perform stress. */
  protected async setStressParams(): Promise<void> {
    await this.buffers.stressParams.srcBuf.mapAsync(GPUMapMode.WRITE);
    const stressParamsArrayBuffer = this.buffers.stressParams.srcBuf.getMappedRange();
    const stressParamsArray = new Uint32Array(stressParamsArrayBuffer);
    if (this.getRandomInt(100) < this.params.barrierPct) {
      stressParamsArray[barrierParamIndex] = 1;
    } else {
      stressParamsArray[barrierParamIndex] = 0;
    }
    if (this.getRandomInt(100) < this.params.memStressPct) {
      stressParamsArray[memStressIndex] = 1;
    } else {
      stressParamsArray[memStressIndex] = 0;
    }
    stressParamsArray[memStressIterationsIndex] = this.params.memStressIterations;
    const memStressStoreFirst = this.getRandomInt(100) < this.params.memStressStoreFirstPct;
    const memStressStoreSecond = this.getRandomInt(100) < this.params.memStressStoreSecondPct;
    let memStressPattern;
    if (memStressStoreFirst && memStressStoreSecond) {
      memStressPattern = 0;
    } else if (memStressStoreFirst && !memStressStoreSecond) {
      memStressPattern = 1;
    } else if (!memStressStoreFirst && memStressStoreSecond) {
      memStressPattern = 2;
    } else {
      memStressPattern = 3;
    }
    stressParamsArray[memStressPatternIndex] = memStressPattern;
    if (this.getRandomInt(100) < this.params.preStressPct) {
      stressParamsArray[preStressIndex] = 1;
    } else {
      stressParamsArray[preStressIndex] = 0;
    }
    stressParamsArray[preStressIterationsIndex] = this.params.preStressIterations;
    const preStressStoreFirst = this.getRandomInt(100) < this.params.preStressStoreFirstPct;
    const preStressStoreSecond = this.getRandomInt(100) < this.params.preStressStoreSecondPct;
    let preStressPattern;
    if (preStressStoreFirst && preStressStoreSecond) {
      preStressPattern = 0;
    } else if (preStressStoreFirst && !preStressStoreSecond) {
      preStressPattern = 1;
    } else if (!preStressStoreFirst && preStressStoreSecond) {
      preStressPattern = 2;
    } else {
      preStressPattern = 3;
    }
    stressParamsArray[preStressPatternIndex] = preStressPattern;
    stressParamsArray[permuteFirstIndex] = this.params.permuteFirst;
    stressParamsArray[permuteSecondIndex] = this.params.permuteSecond;
    stressParamsArray[testingWorkgroupsIndex] = this.params.testingWorkgroups;
    stressParamsArray[memStrideIndex] = this.params.memStride;
    if (this.params.aliasedMemory) {
      stressParamsArray[memLocationOffsetIndex] = 0;
    } else {
      stressParamsArray[memLocationOffsetIndex] = this.params.memStride;
    }
    this.buffers.stressParams.srcBuf.unmap();
  }
}

/** Defines common data structures used in memory model test shaders. */
const shaderMemStructures = `
  struct Memory {
    value: array<AccessValueTy>
  };

  struct AtomicMemory {
    value: array<atomic<u32>>
  };

  struct IndexMemory {
    value: array<u32>
  };

  struct ReadResult {
    r0: atomic<u32>,
    r1: atomic<u32>,
  };

  struct ReadResults {
    value: array<ReadResult>
  };

  struct StressParamsMemory {
    do_barrier: u32,
    mem_stress: u32,
    mem_stress_iterations: u32,
    mem_stress_pattern: u32,
    pre_stress: u32,
    pre_stress_iterations: u32,
    pre_stress_pattern: u32,
    permute_first: u32,
    permute_second: u32,
    testing_workgroups: u32,
    mem_stride: u32,
    location_offset: u32,
  };
`;

/**
 * Structure to hold the counts of occurrences of the possible behaviors of a two-thread, four-instruction test.
 * "seq0" means the first invocation's instructions are observed to have occurred before the second invocation's instructions.
 * "seq1" means the second invocation's instructions are observed to have occurred before the first invocation's instructions.
 * "interleaved" means there was an observation of some interleaving of instructions between the two invocations.
 * "weak" means there was an observation of some ordering of instructions that is inconsistent with the WebGPU memory model.
 */
const fourBehaviorTestResultStructure = `
  struct TestResults {
    seq0: atomic<u32>,
    seq1: atomic<u32>,
    interleaved: atomic<u32>,
    weak: atomic<u32>,
  };
`;

/**
 * Defines the possible behaviors of a two instruction test. Used to test the behavior of non-atomic memory with barriers and
 * one-thread coherence tests.
 * "seq" means that the expected, sequential behavior occurred.
 * "weak" means that an unexpected, inconsistent behavior occurred.
 */
const twoBehaviorTestResultStructure = `
  struct TestResults {
    seq: atomic<u32>,
    weak: atomic<u32>,
  };
`;

/** Common bindings used in the test shader phase of a test. */
const commonTestShaderBindings = `
  @group(0) @binding(1) var<storage, read_write> results : ReadResults;
  @group(0) @binding(2) var<storage, read> shuffled_workgroups : IndexMemory;
  @group(0) @binding(3) var<storage, read_write> barrier : AtomicMemory;
  @group(0) @binding(4) var<storage, read_write> scratchpad : IndexMemory;
  @group(0) @binding(5) var<storage, read_write> scratch_locations : IndexMemory;
  @group(0) @binding(6) var<uniform> stress_params : StressParamsMemory;
`;

/** The combined bindings for a test on atomic memory. */
const atomicTestShaderBindings = [
  `
  @group(0) @binding(0) var<storage, read_write> test_locations : AtomicMemory;
`,
  commonTestShaderBindings,
].join('\n');

/** The combined bindings for a test on non-atomic memory. */
const nonAtomicTestShaderBindings = [
  `
  @group(0) @binding(0) var<storage, read_write> test_locations : Memory;
`,
  commonTestShaderBindings,
].join('\n');

/** Bindings used in the result aggregation phase of the test. */
const resultShaderBindings = `
  @group(0) @binding(0) var<storage, read_write> test_locations : Memory;
  @group(0) @binding(1) var<storage, read_write> read_results : ReadResults;
  @group(0) @binding(2) var<storage, read_write> test_results : TestResults;
  @group(0) @binding(3) var<uniform> stress_params : StressParamsMemory;
`;

/**
 * For tests that operate on workgroup memory, include this definition. 3584 memory locations is
 * large enough to accommodate the maximum memory size needed per workgroup for testing, which is
 * 256 invocations per workgroup x 2 memory locations x 7 (memStride, or max stride between successive memory locations).
 * Should change to a pipeline overridable constant when possible.
 */
const atomicWorkgroupMemory = `
  var<workgroup> wg_test_locations: array<atomic<u32>, 3584>;
`;

/**
 * For tests that operate on non-atomic workgroup memory, include this definition. 3584 memory locations
 * is large enough to accommodate the maximum memory size needed per workgroup for testing.
 */
const nonAtomicWorkgroupMemory = `
  var<workgroup> wg_test_locations: array<AccessValueTy, 3584>;
`;

/**
 * Functions used to calculate memory locations for each invocation, for both testing and result aggregation.
 * The permute function ensures a random permutation based on multiplying and modding by coprime numbers. The stripe
 * workgroup function ensures that invocations coordinating on a test are spread out across different workgroups.
 */
const memoryLocationFunctions = `
  fn permute_id(id: u32, factor: u32, mask: u32) -> u32 {
    return (id * factor) % mask;
  }

  fn stripe_workgroup(workgroup_id: u32, local_id: u32) -> u32 {
    return (workgroup_id + 1u + local_id % (stress_params.testing_workgroups - 1u)) % stress_params.testing_workgroups;
  }
`;

/** Functions that help add stress to the test. */
const testShaderFunctions = `
  //Force the invocations in the workgroup to wait for each other, but without the general memory ordering
  // effects of a control barrier. The barrier spins until either all invocations have incremented the atomic
  // variable or 1024 loops have occurred. 1024 was chosen because it gives more time for invocations to enter
  // the barrier but does not overly reduce testing throughput.
  fn spin(limit: u32) {
    var i : u32 = 0u;
    var bar_val : u32 = atomicAdd(&barrier.value[0], 1u);
    loop {
      if (i == 1024u || bar_val >= limit) {
        break;
      }
      bar_val = atomicAdd(&barrier.value[0], 0u);
      i = i + 1u;
    }
  }

  // Perform iterations of stress, depending on the specified pattern. Pattern 0 is store-store, pattern 1 is store-load,
  // pattern 2 is load-store, and pattern 3 is load-load. The extra if condition (if tmpX > 100000u), is used to avoid
  // the compiler optimizing out unused loads, where 100,000 is larger than the maximum number of stress iterations used
  // in any test.
  fn do_stress(iterations: u32, pattern: u32, workgroup_id: u32) {
    let addr = scratch_locations.value[workgroup_id];
    switch(pattern) {
      case 0u: {
        for(var i: u32 = 0u; i < iterations; i = i + 1u) {
          scratchpad.value[addr] = i;
          scratchpad.value[addr] = i + 1u;
        }
      }
      case 1u: {
        for(var i: u32 = 0u; i < iterations; i = i + 1u) {
          scratchpad.value[addr] = i;
          let tmp1: u32 = scratchpad.value[addr];
          if (tmp1 > 100000u) {
            scratchpad.value[addr] = i;
            break;
          }
        }
      }
      case 2u: {
        for(var i: u32 = 0u; i < iterations; i = i + 1u) {
          let tmp1: u32 = scratchpad.value[addr];
          if (tmp1 > 100000u) {
            scratchpad.value[addr] = i;
            break;
          }
          scratchpad.value[addr] = i;
        }
      }
      case 3u: {
        for(var i: u32 = 0u; i < iterations; i = i + 1u) {
          let tmp1: u32 = scratchpad.value[addr];
          if (tmp1 > 100000u) {
            scratchpad.value[addr] = i;
            break;
          }
          let tmp2: u32 = scratchpad.value[addr];
          if (tmp2 > 100000u) {
            scratchpad.value[addr] = i;
            break;
          }
        }
      }
      default: {
      }
    }
  }
`;

/**
 * Entry point to both test and result shaders. One-dimensional workgroup size is hardcoded to 256, until
 * pipeline overridable constants are supported.
 */
const shaderEntryPoint = `
  // Change to pipeline overridable constant when possible.
  const workgroupXSize = 256u;
  @compute @workgroup_size(workgroupXSize) fn main(
    @builtin(local_invocation_id) local_invocation_id : vec3<u32>,
    @builtin(workgroup_id) workgroup_id : vec3<u32>) {
`;

/** All test shaders first calculate the shuffled workgroup. */
const testShaderCommonHeader = `
    let shuffled_workgroup = shuffled_workgroups.value[workgroup_id[0]];
    if (shuffled_workgroup < stress_params.testing_workgroups) {
`;

/**
 * All test shaders must calculate addresses for memory locations used in the test. Not all these addresses are
 * used in every test, but no test uses more than these addresses.
 */
const testShaderCommonCalculations = `
  let x_0 = id_0 * stress_params.mem_stride * 2u;
  let y_0 = permute_id(id_0, stress_params.permute_second, total_ids) * stress_params.mem_stride * 2u + stress_params.location_offset;
  let x_1 = id_1 * stress_params.mem_stride * 2u;
  let y_1 = permute_id(id_1, stress_params.permute_second, total_ids) * stress_params.mem_stride * 2u + stress_params.location_offset;
  if (stress_params.pre_stress == 1u) {
    do_stress(stress_params.pre_stress_iterations, stress_params.pre_stress_pattern, shuffled_workgroup);
  }
`;

/**
 * An inter-workgroup test calculates two sets of memory locations that are guaranteed to be in separate workgroups.
 * If the bounded spin-loop barrier is called, it attempts to wait for all invocations in all workgroups.
 */
const interWorkgroupTestShaderCode = [
  `
  let total_ids = workgroupXSize * stress_params.testing_workgroups;
  let id_0 = shuffled_workgroup * workgroupXSize + local_invocation_id[0];
  let new_workgroup = stripe_workgroup(shuffled_workgroup, local_invocation_id[0]);
  let id_1 = new_workgroup * workgroupXSize + permute_id(local_invocation_id[0], stress_params.permute_first, workgroupXSize);
`,
  testShaderCommonCalculations,
  `
  if (stress_params.do_barrier == 1u) {
    spin(workgroupXSize * stress_params.testing_workgroups);
  }
`,
].join('\n');

/**
 * An intra-workgroup test calculates two set of memory locations that are guaranteed to be in the same workgroup.
 * If the bounded spin-loop barrier is called, it attempts to wait for all invocations in the same workgroup.
 */
const intraWorkgroupTestShaderCode = [
  `
  let total_ids = workgroupXSize;
  let id_0 = local_invocation_id[0];
  let id_1 = permute_id(local_invocation_id[0], stress_params.permute_first, workgroupXSize);
`,
  testShaderCommonCalculations,
  `
  if (stress_params.do_barrier == 1u) {
    spin(workgroupXSize);
  }
`,
].join('\n');

/**
 * Tests that operate on storage memory and communicate with invocations in the same workgroup must offset their locations
 * relative to global memory.
 */
const storageIntraWorkgroupTestShaderCode = `
  let total_ids = workgroupXSize;
  let id_0 = local_invocation_id[0];
  let id_1 = permute_id(local_invocation_id[0], stress_params.permute_first, workgroupXSize);
  let x_0 = (shuffled_workgroup * workgroupXSize + id_0) * stress_params.mem_stride * 2u;
  let y_0 = (shuffled_workgroup * workgroupXSize + permute_id(id_0, stress_params.permute_second, total_ids)) * stress_params.mem_stride * 2u + stress_params.location_offset;
  let x_1 = (shuffled_workgroup * workgroupXSize + id_1) * stress_params.mem_stride * 2u;
  let y_1 = (shuffled_workgroup * workgroupXSize + permute_id(id_1, stress_params.permute_second, total_ids)) * stress_params.mem_stride * 2u + stress_params.location_offset;
  if (stress_params.pre_stress == 1u) {
    do_stress(stress_params.pre_stress_iterations, stress_params.pre_stress_pattern, shuffled_workgroup);
  }
  if (stress_params.do_barrier == 1u) {
    spin(workgroupXSize);
  }
`;

/** All test shaders may perform stress with non-testing threads. */
const testShaderCommonFooter = `
    } else if (stress_params.mem_stress == 1u) {
      do_stress(stress_params.mem_stress_iterations, stress_params.mem_stress_pattern, shuffled_workgroup);
    }
  }
`;

/**
 * All result shaders must calculate memory locations used in the test. Not all these locations are
 * used in every result shader, but no result shader uses more than these locations.
 *
 * Each value read from test_locations is converted from AccessValueTy to u32
 * before storing it in the read result.  This assumes u32(AccessValueTy)
 * is either an identity function u32(u32) or a value-converting overload such
 * as u32(f16).
 */
const resultShaderCommonCalculations = `
  let id_0 = workgroup_id[0] * workgroupXSize + local_invocation_id[0];
  let x_0 = id_0 * stress_params.mem_stride * 2u;
  let mem_x_0 = u32(test_locations.value[x_0]);
  let r0 = atomicLoad(&read_results.value[id_0].r0);
  let r1 = atomicLoad(&read_results.value[id_0].r1);
`;

/** Common result shader code for an inter-workgroup test. */
const interWorkgroupResultShaderCode = [
  resultShaderCommonCalculations,
  `
  let total_ids = workgroupXSize * stress_params.testing_workgroups;
  let y_0 = permute_id(id_0, stress_params.permute_second, total_ids) * stress_params.mem_stride * 2u + stress_params.location_offset;
  let mem_y_0 = u32(test_locations.value[y_0]);
`,
].join('\n');

/** Common result shader code for an intra-workgroup test. */
const intraWorkgroupResultShaderCode = [
  resultShaderCommonCalculations,
  `
  let total_ids = workgroupXSize;
  let y_0 = (workgroup_id[0] * workgroupXSize + permute_id(local_invocation_id[0], stress_params.permute_second, total_ids)) * stress_params.mem_stride * 2u + stress_params.location_offset;
  let mem_y_0 = u32(test_locations.value[y_0]);
`,
].join('\n');

/** Ending bracket for result shaders. */
const resultShaderCommonFooter = `
}
`;

/** The common shader code for test shaders that perform atomic storage class memory litmus tests. */
const storageMemoryAtomicTestShaderCode = [
  shaderMemStructures,
  atomicTestShaderBindings,
  memoryLocationFunctions,
  testShaderFunctions,
  shaderEntryPoint,
  testShaderCommonHeader,
].join('\n');

/** The common shader code for test shaders that perform non-atomic storage class memory litmus tests. */
const storageMemoryNonAtomicTestShaderCode = [
  shaderMemStructures,
  nonAtomicTestShaderBindings,
  memoryLocationFunctions,
  testShaderFunctions,
  shaderEntryPoint,
  testShaderCommonHeader,
].join('\n');

/** The common shader code for test shaders that perform atomic workgroup class memory litmus tests. */
const workgroupMemoryAtomicTestShaderCode = [
  shaderMemStructures,
  atomicTestShaderBindings,
  atomicWorkgroupMemory,
  memoryLocationFunctions,
  testShaderFunctions,
  shaderEntryPoint,
  testShaderCommonHeader,
].join('\n');

/** The common shader code for test shaders that perform non-atomic workgroup class memory litmus tests. */
const workgroupMemoryNonAtomicTestShaderCode = [
  shaderMemStructures,
  nonAtomicTestShaderBindings,
  nonAtomicWorkgroupMemory,
  memoryLocationFunctions,
  testShaderFunctions,
  shaderEntryPoint,
  testShaderCommonHeader,
].join('\n');

/** The common shader code for all result shaders. */
const resultShaderCommonCode = [
  shaderMemStructures,
  resultShaderBindings,
  memoryLocationFunctions,
  shaderEntryPoint,
].join('\n');

/**
 * Defines the types of possible memory a test is operating on. Used as part of the process of building shader code from
 * its composite parts.
 */
export enum MemoryType {
  /** Atomic memory in the storage address space. */
  AtomicStorageClass = 'atomic_storage',
  /** Non-atomic memory in the storage address space. */
  NonAtomicStorageClass = 'non_atomic_storage',
  /** Atomic memory in the workgroup address space. */
  AtomicWorkgroupClass = 'atomic_workgroup',
  /** Non-atomic memory in the workgroup address space. */
  NonAtomicWorkgroupClass = 'non_atomic_workgroup',
}

/**
 * Defines the relative positions of two invocations coordinating on a test. Used as part of the process of building shader
 * code from its composite parts.
 */
export enum TestType {
  /** A test consists of two invocations in different workgroups. */
  InterWorkgroup = 'inter_workgroup',
  /** A test consists of two invocations in the same workgroup. */
  IntraWorkgroup = 'intra_workgroup',
}

/** Defines the number of behaviors a test may have. */
export enum ResultType {
  TwoBehavior,
  FourBehavior,
}

/**
 * Given test code that performs the actual sequence of loads and stores, as well as a memory type and test type, returns
 * a complete test shader.
 */
export function buildTestShader(
  testCode: string,
  memoryType: MemoryType,
  testType: TestType
): string {
  let memoryTypeCode;
  let isStorageAS = false;
  switch (memoryType) {
    case MemoryType.AtomicStorageClass:
      memoryTypeCode = storageMemoryAtomicTestShaderCode;
      isStorageAS = true;
      break;
    case MemoryType.NonAtomicStorageClass:
      memoryTypeCode = storageMemoryNonAtomicTestShaderCode;
      isStorageAS = true;
      break;
    case MemoryType.AtomicWorkgroupClass:
      memoryTypeCode = workgroupMemoryAtomicTestShaderCode;
      break;
    case MemoryType.NonAtomicWorkgroupClass:
      memoryTypeCode = workgroupMemoryNonAtomicTestShaderCode;
  }
  let testTypeCode;
  switch (testType) {
    case TestType.InterWorkgroup:
      testTypeCode = interWorkgroupTestShaderCode;
      break;
    case TestType.IntraWorkgroup:
      if (isStorageAS) {
        testTypeCode = storageIntraWorkgroupTestShaderCode;
      } else {
        testTypeCode = intraWorkgroupTestShaderCode;
      }
  }
  return [memoryTypeCode, testTypeCode, testCode, testShaderCommonFooter].join('\n');
}

/**
 * Given result code that aggregates the possible behaviors of a test across all instances, as well as a test type and
 * number of behaviors, returns a complete result shader.
 */
export function buildResultShader(
  resultCode: string,
  testType: TestType,
  resultType: ResultType
): string {
  let resultStructure;
  switch (resultType) {
    case ResultType.TwoBehavior:
      resultStructure = twoBehaviorTestResultStructure;
      break;
    case ResultType.FourBehavior:
      resultStructure = fourBehaviorTestResultStructure;
  }
  let testTypeCode;
  switch (testType) {
    case TestType.InterWorkgroup:
      testTypeCode = interWorkgroupResultShaderCode;
      break;
    case TestType.IntraWorkgroup:
      testTypeCode = intraWorkgroupResultShaderCode;
  }
  return [
    resultStructure,
    resultShaderCommonCode,
    testTypeCode,
    resultCode,
    resultShaderCommonFooter,
  ].join('\n');
}
