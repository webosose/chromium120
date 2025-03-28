// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_RENDERER_V8_UNWINDER_H_
#define CHROME_RENDERER_V8_UNWINDER_H_

#include <memory>
#include <set>

#include "base/memory/raw_ptr.h"
#include "base/profiler/unwinder.h"
#include "v8/include/v8-unwinder.h"

namespace v8 {
class Isolate;
}  // namespace v8

// Implements stack frame unwinding for V8 generated code frames, for use with
// the StackSamplingProfiler.
class V8Unwinder : public base::Unwinder {
 public:
  explicit V8Unwinder(v8::Isolate* isolate);
  ~V8Unwinder() override;

  V8Unwinder(const V8Unwinder&) = delete;
  V8Unwinder& operator=(const V8Unwinder&) = delete;

  // Unwinder:
  void InitializeModules() override;
  void OnStackCapture() override;
  void UpdateModules() override;
  bool CanUnwindFrom(const base::Frame& current_frame) const override;
  base::UnwindResult TryUnwind(base::RegisterContext* thread_context,
                               uintptr_t stack_top,
                               std::vector<base::Frame>* stack) override;

  // Build ids generated by the unwinder. Exposed for test use.
  static const char kV8EmbeddedCodeRangeBuildId[];
  static const char kV8CodeRangeBuildId[];

 protected:
  // Invokes CopyCodePages on the Isolate. Virtual to provide a seam for testing
  // of module processing behavior.
  virtual size_t CopyCodePages(size_t capacity, v8::MemoryRange* code_pages);

 private:
  // Custom container for storing V8 code memory ranges. We use this type rather
  // than std::vector to guarantee that no heap allocation occurs during the
  // operations used in OnStackCapture().
  class MemoryRanges {
   public:
    MemoryRanges();
    ~MemoryRanges();

    // Functions that must not heap allocate:
    // Returns a pointer to the start of the internal buffer.
    v8::MemoryRange* buffer() { return ranges_.get(); }
    const v8::MemoryRange* buffer() const { return ranges_.get(); }

    // The capacity of the internal buffer.
    size_t capacity() const { return capacity_; }

    // Sets the number of elements stored.
    void SetSize(size_t size);

    // Functions that may heap allocate:
    // Ensures that the object can store |required_capacity| elements,
    // allocating more space if necessary.
    void ExpandCapacityIfNecessary(size_t required_capacity);

    // The number of elements stored.
    size_t size() const {
      DCHECK_LE(size_, capacity_);
      return size_;
    }

   private:
    // Capacity and size are denominated in number of elements.
    size_t capacity_;
    size_t size_;
    std::unique_ptr<v8::MemoryRange[]> ranges_;
  };

  // Compares modules by their extents in memory.
  struct ModuleCompare {
    bool operator()(const base::ModuleCache::Module* a,
                    const base::ModuleCache::Module* b) const;
  };

  const raw_ptr<v8::Isolate, ExperimentalRenderer> isolate_;
  const v8::JSEntryStubs js_entry_stubs_;
  const v8::MemoryRange embedded_code_range_;

  // Code ranges recorded for the current sample.
  MemoryRanges code_ranges_;

  // The number of code ranges required to represent all of ranges supplied by
  // V8 on the last call to CopyCodePages().
  size_t required_code_ranges_capacity_ = 0;

  // Records the currently active V8 modules, ordered by their extents in
  // memory.
  std::set<const base::ModuleCache::Module*, ModuleCompare> modules_;
};

#endif  // CHROME_RENDERER_V8_UNWINDER_H_
