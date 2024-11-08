/**
 * Reifies a `GPUOrigin3D` into a `Required<GPUOrigin3DDict>`.
 */
export function reifyOrigin3D(
  val: Readonly<GPUOrigin3DDict> | Iterable<number>
): Required<GPUOrigin3DDict> {
  if (Symbol.iterator in val) {
    const v = Array.from(val as Iterable<number>);
    return {
      x: (v[0] ?? 0) | 0,
      y: (v[1] ?? 0) | 0,
      z: (v[2] ?? 0) | 0,
    };
  } else {
    const v = val as Readonly<GPUOrigin3DDict>;
    return {
      x: (v.x ?? 0) | 0,
      y: (v.y ?? 0) | 0,
      z: (v.z ?? 0) | 0,
    };
  }
}

/**
 * Reifies a `GPUExtent3D` into a `Required<GPUExtent3DDict>`.
 */
export function reifyExtent3D(
  val: Readonly<GPUExtent3DDict> | Iterable<number>
): Required<GPUExtent3DDict> {
  if (Symbol.iterator in val) {
    const v = Array.from(val as Iterable<number>);
    return {
      width: (v[0] ?? 1) | 0,
      height: (v[1] ?? 1) | 0,
      depthOrArrayLayers: (v[2] ?? 1) | 0,
    };
  } else {
    const v = val as Readonly<GPUExtent3DDict>;
    return {
      width: (v.width ?? 1) | 0,
      height: (v.height ?? 1) | 0,
      depthOrArrayLayers: (v.depthOrArrayLayers ?? 1) | 0,
    };
  }
}
