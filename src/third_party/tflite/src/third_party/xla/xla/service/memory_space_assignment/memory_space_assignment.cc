/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/memory_space_assignment/memory_space_assignment.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/memory/memory.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "xla/debug_options_flags.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/hlo/utils/hlo_live_range.h"
#include "xla/service/heap_simulator.h"
#include "xla/service/hlo_value.h"
#include "xla/service/memory_space_assignment/repacking.h"
#include "xla/service/memory_space_assignment/tuning_utils.h"
#include "xla/service/memory_space_assignment/utils.h"
#include "xla/service/time_utils.h"
#include "xla/service/tuple_util.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/status.h"
#include "xla/util.h"
#include "tsl/platform/casts.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/logging.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace memory_space_assignment {
namespace {

// Define a dummy chunk for chunks that will be allocated in the default memory
// space and for keeping track of number of asynchronous copies.
const HeapSimulator::Chunk kDummyChunk =
    HeapSimulator::Chunk::FromOffsetSize(-1, -1);
// For cross-program prefetched buffer, we only perform the freeing optimization
// if the buffer occupies less of the execution time ratio than this value.
const float kCrossProgramPrefetchOccupyFreeingLimit = 0.6;
// Each time we retry compilation, increase the preferred eviction end time by
// this amount multiplied by preferred overlap to async copy ratio.
const float kEvictionRetryMultiplier = 2.0;
// The number of decreasing intervals for CostAnalysisPrefetchIntervalPicker to
// return when it runs out of increasing intervals. Increasing this number may
// hurt compilation time.
const int kNumExploredDecreasingIntervals = 100;

template <typename T>
std::string VectorToString(const std::vector<T>& v,
                           bool include_indices = false, int start = 0,
                           int end = std::numeric_limits<int>::max()) {
  std::vector<std::string> elements;

  for (int i = start; i < end && i < v.size(); ++i) {
    std::string prefix;
    if (include_indices) {
      prefix = absl::StrCat(i, ": ");
    }
    elements.push_back(absl::StrCat(prefix, v[i]));
  }

  return absl::StrCat("[ ", absl::StrJoin(elements, ", "), " ]");
}

bool LooksLikeAnActivation(const HloInstruction* inst) {
  for (HloInstruction* user : inst->users()) {
    switch (user->opcode()) {
      case HloOpcode::kConvolution:
      case HloOpcode::kDot:
        if (user->operand(0) == inst) {
          return true;
        }
        break;
      case HloOpcode::kGather:
        if (user->operand(1) == inst) {
          return true;
        }
        break;
      case HloOpcode::kFusion:
        for (int i = 0; i < user->operand_count(); ++i) {
          if (user->operand(i) == inst &&
              LooksLikeAnActivation(user->fused_parameter(i))) {
            return true;
          }
        }
        break;
      case HloOpcode::kBitcast:
      case HloOpcode::kBroadcast:
      case HloOpcode::kTranspose:
        if (LooksLikeAnActivation(user)) {
          return true;
        }
        break;
      case HloOpcode::kCopy:
        if (user->IsFused() && (user == user->parent()->root_instruction())) {
          user = user->parent()->FusionInstruction();
          if (LooksLikeAnActivation(user)) {
            return true;
          } else {
            break;
          }
        }
        return true;
      case HloOpcode::kDynamicUpdateSlice:
      case HloOpcode::kDynamicSlice:
        if (std::find(user->operands().begin() + 1, user->operands().end(),
                      inst) != user->operands().end()) {
          return true;
        }
        if (LooksLikeAnActivation(user)) {
          return true;
        }
        break;
      case HloOpcode::kReduce:
        // Check init operands.
        if (std::find(user->operands().begin() + user->operand_count() / 2,
                      user->operands().end(), inst) != user->operands().end()) {
          return true;
        }
        if (LooksLikeAnActivation(user)) {
          return true;
        }
        break;
      default:
        return true;
    }
  }
  return false;
}

// Filters out buffer uses that cannot use the cross-program prefetch due to
// aliasing with program output.
std::vector<HloUse> FindCrossProgramPrefetchUses(
    absl::Span<const HloUse> buffer_uses,
    const HloAliasAnalysis& alias_analysis) {
  std::vector<HloUse> uses;
  if (buffer_uses.empty()) {
    return uses;
  }
  const HloInstruction* root_instruction = buffer_uses.at(0)
                                               .instruction->GetModule()
                                               ->entry_computation()
                                               ->root_instruction();
  // This function returns true if the use value does not live out of the
  // module. The value lives out if it is the root or it aliases with another
  // value that lives out. We recurse to detect the latter case.
  std::function<bool(const HloUse&)> use_does_not_live_out =
      [&](const HloUse& use) {
        if (use.instruction == root_instruction &&
            (use.instruction->opcode() == HloOpcode::kTuple ||
             use.instruction->opcode() == HloOpcode::kBitcast)) {
          return false;
        }
        auto in_place_pairs =
            HloDataflowAnalysis::GetInPlaceInputOutputPairs(use.instruction);
        return absl::c_all_of(
            in_place_pairs,
            [&](const std::pair<HloOperandIndex, ShapeIndex>& in_place_pair) {
              if (in_place_pair.first.operand_number == use.operand_number &&
                  in_place_pair.first.operand_index == use.operand_index) {
                return use.instruction != root_instruction &&
                       absl::c_all_of(
                           alias_analysis.dataflow_analysis()
                               .GetUniqueValueAt(use.instruction,
                                                 in_place_pair.second)
                               .GetUses(),
                           use_does_not_live_out);
              }
              return true;
            });
      };

  absl::c_copy_if(buffer_uses, std::back_inserter(uses), use_does_not_live_out);
  return uses;
}

bool IsCrossProgramPrefetchCandidate(const HloValue& value,
                                     const HloAliasAnalysis& alias_analysis,
                                     const Options& options) {
  std::vector<HloUse> uses =
      FindCrossProgramPrefetchUses(value.GetUses(), alias_analysis);
  return value.defining_instruction()->parent() ==
             value.defining_instruction()->GetModule()->entry_computation() &&
         value.defining_instruction()->opcode() == HloOpcode::kParameter &&
         (!value.shape().has_layout() ||
          value.shape().layout().memory_space() !=
              options.alternate_memory_space) &&
         value.index().size() <= 1 && value.shape().IsArray() &&
         !uses.empty() && options.size_fn(value) <= options.max_size_in_bytes &&
         absl::c_all_of(uses, [&](const HloUse& use) {
           const HloInstruction* inst =
               use.instruction->operand(use.operand_number);

           // Skip the LooksLikeAnActivation test since we're testing the
           // parent GTE/parameter and its children below.
           if (inst->opcode() == HloOpcode::kBitcast &&
               ((inst->operand(0)->opcode() == HloOpcode::kGetTupleElement &&
                 inst->operand(0)->operand(0)->opcode() ==
                     HloOpcode::kParameter) ||
                inst->operand(0)->opcode() == HloOpcode::kParameter)) {
             return true;
           }

           return (inst->opcode() == HloOpcode::kGetTupleElement ||
                   inst->opcode() == HloOpcode::kParameter) &&
                  !LooksLikeAnActivation(inst);
         });
}

std::vector<MemorySpaceAssignment::BufferInterval>
FindCrossProgramPrefetchCandidates(const HloAliasAnalysis& alias_analysis,
                                   const HloLiveRange& hlo_live_range,
                                   const Options& options) {
  std::vector<MemorySpaceAssignment::BufferInterval> candidates;
  for (const HloBuffer& buffer : alias_analysis.buffers()) {
    CHECK_GE(buffer.values().size(), 1);
    const HloValue* value = buffer.values().at(0);
    if (IsCrossProgramPrefetchCandidate(*value, alias_analysis, options)) {
      MemorySpaceAssignment::BufferInterval interval;
      interval.buffer = value;
      interval.size = options.size_fn(*value);
      interval.start = 0;
      interval.end = hlo_live_range.schedule_end_time();
      interval.need_allocation = true;
      interval.colocations = {++buffer.values().begin(), buffer.values().end()};
      candidates.emplace_back(interval);
    }
  }

  // The BufferIntervalCompare function used to sort buffers implements the
  // greater-than operator so that the most beneficial buffers are allocated
  // first. The size_compare function below hence uses the greater-than operator
  // to pick the largest buffer.
  auto size_compare = [](const auto& x, const auto& y) {
    if (x.size == y.size) {
      // When both buffers are of same size, we prefer the one that is used to
      // produce larger tensors in its consumer instructions.
      auto get_use_size =
          [](const MemorySpaceAssignment::BufferInterval& bi) -> int64_t {
        int64_t use_size = 0;
        for (const auto& use : bi.buffer->GetUses()) {
          use_size += ShapeUtil::ElementsInRecursive(use.instruction->shape());
        }
        return use_size;
      };
      return get_use_size(x) > get_use_size(y);
    }
    return x.size > y.size;
  };
  auto& compare = options.default_cross_program_prefetch_heuristic &&
                          options.buffer_interval_compare
                      ? *options.buffer_interval_compare
                      : size_compare;

  absl::c_sort(candidates, compare);

  VLOG(3) << "Cross-program prefetch candidates: " << candidates.size();
  for (auto& candidate : candidates) {
    VLOG(3) << "Cross-program prefetch candidate picked: "
            << candidate.buffer->ToString();
  }
  return candidates;
}

Status InsertInstructionAndEnsureOperandsInserted(
    HloInstruction* new_instruction, HloInstructionSequence* new_sequence,
    absl::flat_hash_set<HloInstruction*>* inserted_instructions);

// Insert an instruction to the schedule, and make sure its dependencies
// (operands) are already in the schedule. If not, insert these operands
// before the instruction.
Status EnsureInstructionAndOperandsInserted(
    HloInstruction* new_instruction, HloInstructionSequence* new_sequence,
    absl::flat_hash_set<HloInstruction*>* inserted_instructions) {
  if (inserted_instructions->contains(new_instruction)) {
    return OkStatus();
  }
  return InsertInstructionAndEnsureOperandsInserted(
      new_instruction, new_sequence, inserted_instructions);
}

// Same as above, but does not check if instruction is already inserted. This is
// used when the caller already knows the instruction isn't inserted yet, to
// speed up compilation.
Status InsertInstructionAndEnsureOperandsInserted(
    HloInstruction* new_instruction, HloInstructionSequence* new_sequence,
    absl::flat_hash_set<HloInstruction*>* inserted_instructions) {
  for (HloInstruction* operand : new_instruction->operands()) {
    TF_RETURN_IF_ERROR(EnsureInstructionAndOperandsInserted(
        operand, new_sequence, inserted_instructions));
  }
  VLOG(4) << "inserting: " << new_instruction->ToShortString();
  new_sequence->push_back(new_instruction);
  TF_RET_CHECK(inserted_instructions->insert(new_instruction).second);
  return OkStatus();
}

std::string UsesToString(const std::vector<HloUse>& uses) {
  if (uses.empty()) {
    return "none";
  }
  std::vector<std::string> uses_str;
  uses_str.reserve(uses.size());
  for (const auto& use : uses) {
    uses_str.push_back(use.ToString());
  }
  return absl::StrJoin(uses_str, ",");
}

StatusOr<xla::HloLiveRange::LogicalTime> GetScheduleTimeFromInstructionName(
    absl::string_view name,
    const absl::flat_hash_map<const xla::HloInstruction*,
                              xla::HloLiveRange::LogicalTime>& schedule) {
  for (auto schedule_entry : schedule) {
    if (schedule_entry.first->name() == name) {
      return schedule_entry.second;
    }
  }
  return NotFound("Reference instruction %s was not found in the schedule.",
                  name);
}

StatusOr<bool> GetFilterResult(
    const std::pair<FilterUpdatePreferredPrefetch::FilterType, std::string>&
        filter,
    int64_t operand_size, const HloUse& hlo_use) {
  switch (filter.first) {
    case FilterUpdatePreferredPrefetch::FilterType::OP_SIZE_GTE:
      return FilterUpdatePreferredPrefetch::IsOpSizeGte(operand_size,
                                                        filter.second);
    case FilterUpdatePreferredPrefetch::FilterType::OP_SIZE_LTE:
      return FilterUpdatePreferredPrefetch::IsOpSizeLte(operand_size,
                                                        filter.second);
    case FilterUpdatePreferredPrefetch::FilterType::INSTRUCTION_NAME_EXACT:
      return FilterUpdatePreferredPrefetch::IsInstructionNameExact(
          hlo_use.instruction->name(), filter.second);
    case FilterUpdatePreferredPrefetch::FilterType::OP_NUMBER_EXACT:
      return FilterUpdatePreferredPrefetch::IsOpNumberExact(
          hlo_use.operand_number, filter.second);
    case FilterUpdatePreferredPrefetch::FilterType::OP_INDEX_EXACT:
      return FilterUpdatePreferredPrefetch::IsOpIndexExact(
          hlo_use.operand_index, filter.second);
    default:
      return InvalidArgument("Unknown filter type.");
  }
}

StatusOr<std::optional<int64_t>> GetOverriddenPreferredPrefetchTime(
    const std::vector<FilterUpdatePreferredPrefetch>&
        filter_update_preferred_prefetches,
    int64_t operand_size, const HloUse& hlo_use,
    const absl::flat_hash_map<const HloInstruction*, HloLiveRange::LogicalTime>&
        instruction_schedule,
    int64_t earliest_prefetch_time, int64_t latest_prefetch_time) {
  for (const auto& filter_update_preferred_prefetch :
       filter_update_preferred_prefetches) {
    bool match = true;
    for (const auto& filter : filter_update_preferred_prefetch.filter_list_) {
      TF_ASSIGN_OR_RETURN(auto filter_result,
                          GetFilterResult(filter, operand_size, hlo_use));
      match &= filter_result;
    }
    if (match) {
      LOG(INFO) << "Config " << filter_update_preferred_prefetch.ToString()
                << " match for instruction " << hlo_use.instruction->name()
                << " operand number " << hlo_use.operand_number
                << " operand index " << hlo_use.operand_index.ToString()
                << " size " << operand_size << " live range ("
                << earliest_prefetch_time << ", " << latest_prefetch_time
                << ")";
      switch (filter_update_preferred_prefetch.override_type_) {
        case FilterUpdatePreferredPrefetch::OverrideType::PREFETCH_EAGERNESS:
          return filter_update_preferred_prefetch.GetPrefetchByEagerness(
              earliest_prefetch_time, latest_prefetch_time);
        case FilterUpdatePreferredPrefetch::OverrideType::PUT_AFTER_INSTRUCTION:
          return filter_update_preferred_prefetch
              .GetPrefetchTimeAfterInstruction(instruction_schedule);
        case FilterUpdatePreferredPrefetch::OverrideType::
            PUT_BEFORE_INSTRUCTION:
          return filter_update_preferred_prefetch
              .GetPrefetchTimeBeforeInstruction(instruction_schedule);
        default:
          return InvalidArgument("Unknown override type.");
      }
    }
  }
  return static_cast<StatusOr<std::optional<int64_t>>>(std::nullopt);
}

std::tuple<int64_t, bool, int64_t> GetAllocationSortTuple(
    const std::unique_ptr<MemorySpaceAssignment::Allocation>& allocation) {
  int64_t scheduled_on_or_before = allocation->start_time();
  int64_t scheduled_on_or_after = allocation->start_time();
  if (allocation->is_copy_allocation()) {
    auto copy_allocation =
        tensorflow::down_cast<MemorySpaceAssignment::CopyAllocation*>(
            allocation.get());
    scheduled_on_or_before = copy_allocation->copy_done_schedule_before();
    scheduled_on_or_after = copy_allocation->copy_start_schedule_after();
  }
  return std::forward_as_tuple(scheduled_on_or_before,
                               !allocation->is_copy_allocation(),
                               scheduled_on_or_after);
}

void SortAllocationSequence(
    MemorySpaceAssignment::AllocationSequence& allocations) {
  absl::c_sort(
      allocations,
      [](const std::unique_ptr<MemorySpaceAssignment::Allocation>& lhs,
         const std::unique_ptr<MemorySpaceAssignment::Allocation>& rhs) {
        return GetAllocationSortTuple(lhs) < GetAllocationSortTuple(rhs);
      });
}

std::string AllocationSequenceToString(
    MemorySpaceAssignment::AllocationSequence& allocations,
    bool sort_allocations = false) {
  if (sort_allocations) {
    SortAllocationSequence(allocations);
  }
  std::string allocations_str = "\n";
  for (const std::unique_ptr<MemorySpaceAssignment::Allocation>& allocation :
       allocations) {
    absl::StrAppend(&allocations_str, allocation->ToString(), "\n");
  }
  return allocations_str;
}

std::string InstructionScheduleToString(const HloLiveRange& hlo_live_range) {
  const absl::flat_hash_map<const HloInstruction*, HloLiveRange::LogicalTime>&
      instruction_schedule = hlo_live_range.instruction_schedule();
  std::vector<std::pair<int64_t, const HloInstruction*>> instructions;
  instructions.reserve(instruction_schedule.size());
  for (const auto& instruction : instruction_schedule) {
    instructions.push_back({instruction.second, instruction.first});
  }
  std::string instruction_schedule_str = "\n";
  absl::c_sort(instructions);
  for (auto& instruction : instructions) {
    absl::StrAppend(&instruction_schedule_str,
                    "LogicalTime: ", instruction.first, " ",
                    instruction.second->ToString(), "\n");
  }
  return instruction_schedule_str;
}

void EnsureParentAllocationIsAvailableForCopy(
    MemorySpaceAssignment::CopyAllocation* copy_allocation) {
  MemorySpaceAssignment::Allocation& parent_allocation =
      copy_allocation->mutable_prev_allocation();
  parent_allocation.Extend(copy_allocation->copy_done_schedule_before());
  if (parent_allocation.is_copy_allocation()) {
    auto parent_copy_allocation =
        tensorflow::down_cast<MemorySpaceAssignment::CopyAllocation*>(
            &parent_allocation);
    parent_copy_allocation->set_copy_done_schedule_before(
        std::min(parent_copy_allocation->copy_done_schedule_before(),
                 copy_allocation->start_time()));
    parent_copy_allocation->set_copy_start_schedule_after(
        std::min(parent_copy_allocation->copy_start_schedule_after(),
                 parent_copy_allocation->copy_done_schedule_before() - 1));
  }
}

void MakeCopyAllocationJitForSingleUse(
    MemorySpaceAssignment::CopyAllocation* copy_allocation, int64_t use_time) {
  copy_allocation->set_start_time(use_time - 1);
  copy_allocation->set_copy_start_schedule_after(use_time - 1);
  copy_allocation->set_end_time(use_time);
  copy_allocation->set_copy_done_schedule_before(use_time);
  EnsureParentAllocationIsAvailableForCopy(copy_allocation);
}

int64_t GetUseTime(const HloUse& use, const HloLiveRange& hlo_live_range) {
  return hlo_live_range.instruction_schedule().at(use.instruction);
}

std::vector<MemorySpaceAssignment::Allocation*>
GetAllocationSequenceInRawPointers(
    MemorySpaceAssignment::AllocationSequence& allocations) {
  std::vector<MemorySpaceAssignment::Allocation*> allocations_in_raw_pointers;
  for (const std::unique_ptr<MemorySpaceAssignment::Allocation>& allocation :
       allocations) {
    allocations_in_raw_pointers.push_back(allocation.get());
  }
  return allocations_in_raw_pointers;
}

void ProcessPrefetchesToAlternateMemory(
    MemorySpaceAssignment::AllocationSequence& allocations,
    const HloLiveRange& hlo_live_range) {
  std::vector<MemorySpaceAssignment::Allocation*> allocations_in_raw_pointers =
      GetAllocationSequenceInRawPointers(allocations);
  for (auto allocation : allocations_in_raw_pointers) {
    if (allocation->is_copy_allocation() && allocation->is_in_alternate_mem() &&
        !allocation->uses().empty()) {
      MemorySpaceAssignment::CopyAllocation* prefetch =
          tensorflow::down_cast<MemorySpaceAssignment::CopyAllocation*>(
              allocation);
      std::vector<HloUse> uses = prefetch->uses();  // Create a copy of uses.
      prefetch->clear_uses();                       // Clear old uses.
      // For every prefetch, update prefetch to serve earliest use just in time.
      prefetch->AddUse(uses[0]);
      MakeCopyAllocationJitForSingleUse(prefetch,
                                        GetUseTime(uses[0], hlo_live_range));
      // For every use after the first use, create a new prefetch from the same
      // parent allocation.
      for (size_t use_index = 1; use_index < uses.size(); ++use_index) {
        const HloUse& use = uses[use_index];
        int64_t use_time = GetUseTime(use, hlo_live_range);
        auto jit_single_use_prefetch =
            std::make_unique<MemorySpaceAssignment::CopyAllocation>(
                prefetch->mutable_prev_allocation(),
                MemorySpaceAssignment::MemorySpace::kAlternate,
                prefetch->chunk(), use_time - 1, use_time, use_time);
        jit_single_use_prefetch->set_copy_start_schedule_after(use_time - 1);
        jit_single_use_prefetch->AddUse(use);
        EnsureParentAllocationIsAvailableForCopy(jit_single_use_prefetch.get());
        allocations.push_back(std::move(jit_single_use_prefetch));
      }
    }
  }
}

void MakeEvictionImmediate(MemorySpaceAssignment::CopyAllocation* eviction) {
  const MemorySpaceAssignment::Allocation& parent_allocation =
      eviction->prev_allocation();
  eviction->set_start_time(parent_allocation.start_time());
  eviction->set_copy_start_schedule_after(parent_allocation.start_time());
  eviction->set_copy_done_schedule_before(parent_allocation.start_time() + 1);
  eviction->Extend(parent_allocation.start_time() + 1);
}

absl::flat_hash_map<MemorySpaceAssignment::Allocation*,
                    MemorySpaceAssignment::CopyAllocation*>
GetEvictionsMap(std::vector<MemorySpaceAssignment::Allocation*>& allocations) {
  absl::flat_hash_map<MemorySpaceAssignment::Allocation*,
                      MemorySpaceAssignment::CopyAllocation*>
      evictions_map;
  for (auto& allocation : allocations) {
    if (allocation->is_copy_allocation() && allocation->is_in_default_mem()) {
      auto eviction =
          tensorflow::down_cast<MemorySpaceAssignment::CopyAllocation*>(
              allocation);
      MemorySpaceAssignment::Allocation& parent_allocation =
          eviction->mutable_prev_allocation();
      if (!parent_allocation.is_copy_allocation()) {
        evictions_map[&parent_allocation] = eviction;
      }
    }
  }
  return evictions_map;
}

void ProcessBuffersProducedInAlternateMemory(
    MemorySpaceAssignment::AllocationSequence& allocations,
    const HloLiveRange& hlo_live_range) {
  std::vector<MemorySpaceAssignment::Allocation*> allocations_in_raw_pointers =
      GetAllocationSequenceInRawPointers(allocations);
  // For all parent allocations produced in alternate memory, create a map from
  // parent allocation -> eviction.
  absl::flat_hash_map<MemorySpaceAssignment::Allocation*,
                      MemorySpaceAssignment::CopyAllocation*>
      evictions_map = GetEvictionsMap(allocations_in_raw_pointers);
  // Make all such evictions immediate.
  for (auto& [_, eviction] : evictions_map) {
    MakeEvictionImmediate(eviction);
  }
  VLOG(2) << "AllocationSequence after making spills immediate spills\n";
  XLA_LOG_LINES(2, AllocationSequenceToString(allocations, true));
  // Process all buffers produced in the alternate memory:
  // 1. Make the buffer short lived.
  // 2. Service immediate use if any.
  // 3. If buffer is also used later get or create an immediate eviction.
  // 4. For every later use prefetch just in time from the eviction.
  for (auto allocation : allocations_in_raw_pointers) {
    if (!allocation->is_copy_allocation() &&
        allocation->is_in_alternate_mem()) {
      std::vector<HloUse> uses = allocation->uses();  // Create a copy of uses.
      allocation->clear_uses();                       // Clear old uses.
      // Make buffer short lived.
      allocation->set_end_time(allocation->start_time() + 1);
      for (const HloUse& use : uses) {
        int64_t use_time = GetUseTime(use, hlo_live_range);
        if (allocation->start_time() + 1 == use_time) {
          allocation->AddUse(use);
          continue;
        }
        if (!evictions_map.contains(allocation)) {
          auto eviction_unique_ptr =
              std::make_unique<MemorySpaceAssignment::CopyAllocation>(
                  *allocation, MemorySpaceAssignment::MemorySpace::kDefault,
                  std::nullopt, allocation->start_time(),
                  allocation->start_time() + 1, allocation->start_time() + 1);
          eviction_unique_ptr->set_copy_start_schedule_after(
              allocation->start_time());
          evictions_map[allocation] = eviction_unique_ptr.get();
          allocations.push_back(std::move(eviction_unique_ptr));
        }
        MemorySpaceAssignment::CopyAllocation* eviction =
            evictions_map[allocation];
        auto jit_single_use_prefetch =
            std::make_unique<MemorySpaceAssignment::CopyAllocation>(
                *eviction, MemorySpaceAssignment::MemorySpace::kAlternate,
                allocation->chunk(), use_time - 1, use_time, use_time);
        jit_single_use_prefetch->set_copy_start_schedule_after(use_time - 1);
        jit_single_use_prefetch->AddUse(use);
        EnsureParentAllocationIsAvailableForCopy(jit_single_use_prefetch.get());
        allocations.push_back(std::move(jit_single_use_prefetch));
      }
    }
  }
}

void TransformAllocationSequenceToSpill(
    MemorySpaceAssignment::AllocationSequence& allocations,
    const HloLiveRange& hlo_live_range) {
  VLOG(2) << "InstructionSchedule before transform\n";
  XLA_LOG_LINES(2, InstructionScheduleToString(hlo_live_range));
  VLOG(2) << "AllocationSequence before transform\n";
  XLA_LOG_LINES(2, AllocationSequenceToString(allocations, true));
  ProcessPrefetchesToAlternateMemory(allocations, hlo_live_range);
  VLOG(2) << "AllocationSequence after processing prefetches\n";
  XLA_LOG_LINES(2, AllocationSequenceToString(allocations, true));
  ProcessBuffersProducedInAlternateMemory(allocations, hlo_live_range);
  VLOG(2) << "AllocationSequence after processing buffers produced in kAlt\n";
  XLA_LOG_LINES(2, AllocationSequenceToString(allocations, true));
  SortAllocationSequence(allocations);
}
}  // namespace

/*static*/ StatusOr<std::unique_ptr<MemorySpaceAssignmentCostAnalysis>>
MemorySpaceAssignmentCostAnalysis::Create(const HloCostAnalysis& cost_analysis,
                                          const Options& options,
                                          const HloModule& module) {
  TF_ASSIGN_OR_RETURN(auto alias_analysis, HloAliasAnalysis::Run(&module));
  TF_ASSIGN_OR_RETURN(auto hlo_live_range,
                      HloLiveRange::Run(module.schedule(), *alias_analysis,
                                        module.entry_computation()));
  auto call_graph = CallGraph::Build(&module);
  return absl::WrapUnique(new MemorySpaceAssignmentCostAnalysis(
      cost_analysis, options, std::move(alias_analysis),
      std::move(hlo_live_range), std::move(call_graph)));
}

float MemorySpaceAssignmentCostAnalysis::GetAlternateMemoryBenefit(
    const HloInstruction& instruction, float elapsed_time_due_to_alternate_mem,
    MemorySpaceAssignmentCostAnalysis::Cache* cache) const {
  float elapsed_time_due_to_compute =
      GetInstructionElapsedDueToCompute(instruction);
  float elapsed_time_due_to_memory =
      GetInstructionElapsedDueToMemory(instruction);
  if (elapsed_time_due_to_memory > elapsed_time_due_to_compute) {
    // Memory bound, return how much alternate memory is better.
    float while_nest_multiplier;
    if (cache) {
      // If there is a cache provided, memoize the while nest multiplier.
      auto it = cache->while_nest_multiplier.find(&instruction);
      if (it != cache->while_nest_multiplier.end()) {
        while_nest_multiplier = it->second;
      } else {
        while_nest_multiplier = IPow<float>(
            options_.xla_tpu_memory_space_assignment_while_execution_count,
            CalculateComputationNestLevel(&instruction,
                                          /*while_only=*/true));
        cache->while_nest_multiplier[&instruction] = while_nest_multiplier;
      }
    } else {
      while_nest_multiplier = IPow<float>(
          options_.xla_tpu_memory_space_assignment_while_execution_count,
          CalculateComputationNestLevel(&instruction,
                                        /*while_only=*/true));
    }
    return (elapsed_time_due_to_memory - elapsed_time_due_to_alternate_mem) *
           while_nest_multiplier;
  } else {
    // Compute bound, return how far off are we to memory boundedness.
    return elapsed_time_due_to_memory - elapsed_time_due_to_compute;
  }
}

float MemorySpaceAssignmentCostAnalysis::GetMemoryBoundedness(
    const GlobalDecreasingSizeBestFitHeap<HloValue>::BufferInterval& interval,
    MemorySpaceAssignmentCostAnalysis::Cache* cache) const {
  if (cache) {
    auto it =
        cache->memory_boundedness.find(interval.buffer->defining_position());
    if (it != cache->memory_boundedness.end()) {
      return it->second;
    }
  }
  float alternate_mem_benefit =
      GetAlternateMemoryBenefit(interval.buffer->defining_position(), cache);

  for (const HloBuffer* buffer : alias_analysis_->ComputeBuffersAt(
           interval.buffer->defining_position().instruction,
           interval.buffer->defining_position().index)) {
    for (const HloValue* value : buffer->values()) {
      for (const HloUse& use : value->GetUses()) {
        // We look inside the called computations of while and conditional, so
        // don't use the benefit of while and conditional directly.
        if (use.instruction->opcode() == HloOpcode::kWhile ||
            use.instruction->opcode() == HloOpcode::kConditional) {
          continue;
        }
        float use_alternate_mem_benefit = GetAlternateMemoryBenefit(use, cache);
        // If the benefit is positive (memory bound), add it to this buffer's
        // benefit. If the benefit is negative (compute bound), calculate the
        // maximum.
        if (alternate_mem_benefit > 0 && use_alternate_mem_benefit > 0) {
          alternate_mem_benefit += use_alternate_mem_benefit;
        } else {
          alternate_mem_benefit =
              std::max(alternate_mem_benefit, use_alternate_mem_benefit);
        }
      }
    }
  }

  // Penalize larger buffers by dividing the benefit by the square root of
  // the size. Empirically, we observed this resulted in better performance
  // compared to dividing by the size.
  float memory_boundedness = 1;
  if (options_
          .xla_tpu_alternate_memory_benefit_scaling_factor_for_large_buffers ==
      "NO_SCALE") {
    memory_boundedness = alternate_mem_benefit;
  } else {
    memory_boundedness = alternate_mem_benefit / std::sqrt(interval.size);
  }

  if (cache) {
    cache->memory_boundedness[interval.buffer->defining_position()] =
        memory_boundedness;
  }
  return memory_boundedness;
}

float MemorySpaceAssignmentCostAnalysis::GetAlternateMemoryBenefit(
    const HloPosition& position,
    MemorySpaceAssignmentCostAnalysis::Cache* cache) const {
  return GetAlternateMemoryBenefit(
      *position.instruction,
      GetInstructionElapsedDueToMemory(
          *position.instruction,
          /*operands_in_alternate_mem=*/{},
          /*outputs_in_alternate_mem=*/{position.index}),
      cache);
}

float MemorySpaceAssignmentCostAnalysis::GetAlternateMemoryBenefit(
    const HloUse& use, MemorySpaceAssignmentCostAnalysis::Cache* cache) const {
  return GetAlternateMemoryBenefit(
      *use.instruction,
      GetInstructionElapsedDueToMemory(
          *use.instruction,
          /*operands_in_alternate_mem=*/{std::make_pair(use.operand_number,
                                                        use.operand_index)}),
      cache);
}

int MemorySpaceAssignmentCostAnalysis::CalculateComputationNestLevel(
    const HloInstruction* instruction, bool while_only) const {
  int nest_level = 0;
  const HloComputation* computation = instruction->parent();
  while (!computation->IsEntryComputation()) {
    auto& node = call_graph_->GetNode(computation);
    auto callsites = node.caller_callsites();
    CHECK(node.computation()->IsAsyncComputation() || callsites.size() == 1)
        << "The module is not flattened!";
    auto& callsite = callsites[0];
    if (!while_only || callsite.instruction()->opcode() == HloOpcode::kWhile) {
      ++nest_level;
    }
    computation = callsite.instruction()->parent();
  }
  return nest_level;
}

float MemorySpaceAssignmentCostAnalysis::GetDefaultMemoryAccessOverhead(
    const HloInstruction& instruction,
    absl::Span<const std::pair<int64_t, ShapeIndex>> operands_in_alternate_mem,
    absl::Span<const ShapeIndex> outputs_in_alternate_mem) const {
  // Calculate the pipeline overhead of accessing the default memory. We use the
  // maximum of the window size heuristic and the actual default memory bytes
  // accessed multiplied with the compute as the overhead. So, the math is:
  //
  // overhead = compute_per_iteration
  //          = compute_elapsed / num_iterations
  //          = compute_elapsed / (bytes_accessed / window_size)
  //          = (window_size / bytes_accessed) * compute_elapsed
  const float window_size_bytes =
      options_.pipeline_overhead_window_size_mib * 1024 * 1024;
  const float bytes_accessed = cost_analysis_.bytes_accessed(instruction);
  const float default_memory_bytes_accessed =
      bytes_accessed -
      GetBytesAccessedFromAlternateMemory(
          instruction, operands_in_alternate_mem, outputs_in_alternate_mem);
  const float compute_elapsed = GetInstructionElapsedDueToCompute(instruction);
  const float effective_window_size_bytes =
      std::min(window_size_bytes, default_memory_bytes_accessed);
  float overhead = 0;
  if (bytes_accessed > 0) {
    overhead = (effective_window_size_bytes / bytes_accessed) * compute_elapsed;
  }
  return overhead;
}

float MemorySpaceAssignmentCostAnalysis::GetDefaultMemoryBandwidthIdleTime(
    const HloInstruction& instruction,
    absl::Span<const std::pair<int64_t, ShapeIndex>> operands_in_alternate_mem,
    absl::Span<const ShapeIndex> outputs_in_alternate_mem) const {
  const float default_memory_bytes_accessed =
      cost_analysis_.bytes_accessed(instruction) -
      GetBytesAccessedFromAlternateMemory(
          instruction, operands_in_alternate_mem, outputs_in_alternate_mem);
  const float elapsed_due_to_default_mem =
      default_memory_bytes_accessed /
      cost_analysis_.per_second_rate(HloCostAnalysis::kBytesAccessedKey);
  const float elapsed = GetInstructionElapsedInAlternateMemory(
      instruction, operands_in_alternate_mem, outputs_in_alternate_mem);
  return elapsed - elapsed_due_to_default_mem;
}

float MemorySpaceAssignmentCostAnalysis::GetBytesAccessedFromAlternateMemory(
    const HloInstruction& instruction,
    absl::Span<const std::pair<int64_t, ShapeIndex>> operands_in_alternate_mem,
    absl::Span<const ShapeIndex> outputs_in_alternate_mem) const {
  float bytes_accessed_from_alternate_mem = 0.0;
  for (auto& operand : operands_in_alternate_mem) {
    const float operand_bytes_accessed = cost_analysis_.operand_bytes_accessed(
        instruction, operand.first, operand.second);
    bytes_accessed_from_alternate_mem += operand_bytes_accessed;
  }

  for (auto& shape_idx : outputs_in_alternate_mem) {
    const float output_bytes_accessed =
        cost_analysis_.output_bytes_accessed(instruction, shape_idx);
    bytes_accessed_from_alternate_mem += output_bytes_accessed;
  }
  return bytes_accessed_from_alternate_mem;
}

float MemorySpaceAssignmentCostAnalysis::GetInstructionElapsedDueToCompute(
    const HloInstruction& instruction) const {
  return std::max(
      cost_analysis_.flop_count(instruction) /
          cost_analysis_.per_second_rate(HloCostAnalysis::kFlopsKey),
      cost_analysis_.transcendental_count(instruction) /
          cost_analysis_.per_second_rate(HloCostAnalysis::kTranscendentalsKey));
}

float MemorySpaceAssignmentCostAnalysis::GetInstructionElapsedDueToMemory(
    const HloInstruction& instruction,
    absl::Span<const std::pair<int64_t, ShapeIndex>> operands_in_alternate_mem,
    absl::Span<const ShapeIndex> outputs_in_alternate_mem) const {
  float total_bytes_accessed = cost_analysis_.bytes_accessed(instruction);
  float bytes_accessed_from_alternate_mem = GetBytesAccessedFromAlternateMemory(
      instruction, operands_in_alternate_mem, outputs_in_alternate_mem);
  float elapsed_due_to_alternate_mem =
      bytes_accessed_from_alternate_mem /
      options().alternate_mem_bandwidth_bytes_per_second;
  float elapsed_due_to_default_mem =
      (total_bytes_accessed - bytes_accessed_from_alternate_mem) /
      cost_analysis_.per_second_rate(HloCostAnalysis::kBytesAccessedKey);
  return elapsed_due_to_alternate_mem + elapsed_due_to_default_mem;
}

float MemorySpaceAssignmentCostAnalysis::GetInstructionElapsedDueToMemory(
    const HloInstruction& instruction,
    IsInAlternateMemoryFun is_in_alternate_mem) const {
  float total_bytes_accessed = cost_analysis_.bytes_accessed(instruction);
  float bytes_accessed_from_alternate_mem = 0.0;
  for (int operand_num = 0; operand_num < instruction.operand_count();
       ++operand_num) {
    ShapeUtil::ForEachSubshape(
        instruction.operand(operand_num)->shape(),
        [&](const Shape& subshape, const ShapeIndex& index) {
          if (!subshape.IsArray()) {
            return;
          }
          if (is_in_alternate_mem(operand_num, index, subshape)) {
            bytes_accessed_from_alternate_mem +=
                cost_analysis_.operand_bytes_accessed(instruction, operand_num,
                                                      index);
          }
        });
  }
  ShapeUtil::ForEachSubshape(instruction.shape(), [&](const Shape& subshape,
                                                      const ShapeIndex& index) {
    if (!subshape.IsArray()) {
      return;
    }
    if (is_in_alternate_mem(/*operand_num=*/std::nullopt, index, subshape)) {
      bytes_accessed_from_alternate_mem +=
          cost_analysis_.output_bytes_accessed(instruction, index);
    }
  });
  float elapsed_due_to_alternate_mem =
      bytes_accessed_from_alternate_mem /
      options().alternate_mem_bandwidth_bytes_per_second;
  float elapsed_due_to_default_mem =
      (total_bytes_accessed - bytes_accessed_from_alternate_mem) /
      cost_analysis_.per_second_rate(HloCostAnalysis::kBytesAccessedKey);
  return elapsed_due_to_alternate_mem + elapsed_due_to_default_mem;
}

float MemorySpaceAssignmentCostAnalysis::GetInstructionElapsed(
    const HloInstruction& instruction) const {
  float overhead = GetDefaultMemoryAccessOverhead(instruction);
  return std::max(GetInstructionElapsedDueToCompute(instruction),
                  GetInstructionElapsedDueToMemory(instruction) + overhead);
}

float MemorySpaceAssignmentCostAnalysis::GetInstructionElapsedInAlternateMemory(
    const HloInstruction& instruction,
    absl::Span<const std::pair<int64_t, ShapeIndex>> operands_in_alternate_mem,
    absl::Span<const ShapeIndex> outputs_in_alternate_mem) const {
  float overhead = GetDefaultMemoryAccessOverhead(
      instruction, operands_in_alternate_mem, outputs_in_alternate_mem);
  return std::max(
      GetInstructionElapsedDueToCompute(instruction),
      GetInstructionElapsedDueToMemory(instruction, operands_in_alternate_mem,
                                       outputs_in_alternate_mem) +
          overhead);
}

float MemorySpaceAssignmentCostAnalysis::GetInstructionElapsedInAlternateMemory(
    const HloInstruction& instruction,
    IsInAlternateMemoryFun is_in_alternate_mem) const {
  return std::max(
      GetInstructionElapsedDueToCompute(instruction),
      GetInstructionElapsedDueToMemory(instruction, is_in_alternate_mem));
}

float MemorySpaceAssignmentCostAnalysis::GetAsyncCopyElapsed(
    const Shape& shape) const {
  int64_t size_in_bytes = cost_analysis_.GetShapeSize(shape);
  return static_cast<float>(size_in_bytes) /
         (options().async_copy_bandwidth_bytes_per_second *
          options().async_copy_bandwidth_scaling_factor);
}

int64_t MemorySpaceAssignmentCostAnalysis::GetScheduleEndTime() const {
  return hlo_live_range_->schedule_end_time();
}

bool InstructionCountPrefetchIntervalPicker::CanAllocateInAlternateMemoryNoCopy(
    const Shape& shape, int64_t start_time, int64_t end_time) const {
  return end_time - start_time <= max_overlap_count_;
}

int64_t InstructionCountPrefetchIntervalPicker::PreferredEvictionEndTime(
    const Shape& shape, int64_t start_time, int64_t latest_end_time) const {
  return std::min(start_time + min_overlap_count_, latest_end_time);
}

int64_t InstructionCountPrefetchIntervalPicker::LatestPrefetchStartTime(
    const Shape& shape, int64_t start_time, int64_t end_time,
    const HloUse* use) const {
  return end_time - min_overlap_count_;
}

int64_t InstructionCountPrefetchIntervalPicker::PreferredPrefetchStartTime(
    const Shape& shape, int64_t earliest_prefetch_start_time,
    int64_t latest_prefetch_start_time, int64_t prefetch_end_time) const {
  return std::max(earliest_prefetch_start_time,
                  prefetch_end_time - max_overlap_count_);
}

int64_t InstructionCountPrefetchIntervalPicker::EstimatedPrefetchEndTime(
    const Shape& shape, int64_t start_time, int64_t end_time) const {
  // For testing, assume the end time is the estimated prefetch end time.
  return end_time;
}

float InstructionCountPrefetchIntervalPicker::GetLogicalIntervalElapsed(
    int64_t start_time, int64_t end_time) const {
  // For testing, just assume every HLO takes 1 second.
  return static_cast<float>(end_time - start_time - 1);
}

void InstructionCountPrefetchIntervalPicker::Begin(
    const HloUse& use, int64_t start_time, int64_t end_time,
    std::optional<int64_t> preferred_time) {
  end_time_ = end_time;
  const Shape& shape = ShapeUtil::GetSubshape(
      use.instruction->operand(use.operand_number)->shape(), use.operand_index);
  if (preferred_time) {
    current_prefetch_time_ = *preferred_time;
  } else {
    current_prefetch_time_ =
        PreferredPrefetchStartTime(shape, start_time, end_time, end_time);
  }
}

int64_t InstructionCountPrefetchIntervalPicker::Next() {
  CHECK(!Done()) << "Prefetch interval picker's Next() is called even though "
                    "Done() is false";
  return current_prefetch_time_++;
}

bool InstructionCountPrefetchIntervalPicker::Done() const {
  return end_time_ - current_prefetch_time_ <= min_overlap_count_;
}

int64_t InstructionCountPrefetchIntervalPicker::latest_time() const {
  return end_time_ - min_overlap_count_ - 1;
}

std::string InstructionCountPrefetchIntervalPicker::ToDebugString() const {
  return absl::StrCat("Overlapped HLOs = ", end_time_ - current_prefetch_time_);
}

std::string InstructionCountPrefetchIntervalPicker::ToNoCopyDebugString(
    const Shape& shape, int64_t start_time, int64_t end_time) const {
  return absl::StrCat("Overlapped HLOs = ", end_time - start_time);
}

CostAnalysisPrefetchIntervalPicker::CostAnalysisPrefetchIntervalPicker(
    const MemorySpaceAssignmentCostAnalysis& cost_analysis,
    float min_overlap_to_async_copy_ratio,
    float preferred_overlap_to_async_copy_ratio,
    float max_overlap_to_mem_size_async_copy_ratio, int64_t mem_size_bytes,
    const Shape* shape_override)
    : while_nest_level_(
          cost_analysis.hlo_live_range().instruction_schedule().size() + 1, 0),
      computation_nest_level_(
          cost_analysis.hlo_live_range().instruction_schedule().size() + 1, 0),
      cost_analysis_(cost_analysis),
      min_overlap_to_async_copy_ratio_(min_overlap_to_async_copy_ratio),
      preferred_overlap_to_async_copy_ratio_(
          preferred_overlap_to_async_copy_ratio),
      max_async_copy_elapsed_(
          cost_analysis_.GetAsyncCopyElapsed(
              ShapeUtil::MakeShape(S32, {mem_size_bytes / 4})) *
          max_overlap_to_mem_size_async_copy_ratio),
      shape_override_(shape_override ? std::optional(*shape_override)
                                     : std::nullopt) {
  instruction_schedule_ =
      &cost_analysis_.hlo_live_range().instruction_schedule();

  // Create a vector of elapsed times and while nesting levels of HLO
  // instructions. The elapsed times are multiplied by
  // pow(while_execution_count, nest_level) to account for executing the HLOs
  // multiple times in while loops.
  std::vector<float> instructions_elapsed_time(
      instruction_schedule_->size() + 1, 0.0);
  int max_while_nest_level = 0;
  for (const auto& instruction_and_logical_time : *instruction_schedule_) {
    // To avoid double counting, don't include the elapsed time of while and
    // conditional HLOs.
    const HloInstruction* instruction = instruction_and_logical_time.first;
    int64_t logical_time = instruction_and_logical_time.second;
    if (logical_time >= instructions_elapsed_time.size()) {
      instructions_elapsed_time.resize(logical_time + 1, 0.0);
      while_nest_level_.resize(logical_time + 1, 0);
    }
    int while_nest_level = cost_analysis_.CalculateComputationNestLevel(
        instruction_and_logical_time.first, /*while_only=*/true);
    while_nest_level_[logical_time] = while_nest_level;
    max_while_nest_level = std::max(max_while_nest_level, while_nest_level);
    int computation_nest_level = cost_analysis_.CalculateComputationNestLevel(
        instruction_and_logical_time.first, /*while_only=*/false);
    computation_nest_level_[logical_time] = computation_nest_level;
    if (instruction->opcode() == HloOpcode::kWhile ||
        instruction->opcode() == HloOpcode::kConditional) {
      continue;
    }
    float elapsed_time = cost_analysis_.GetInstructionElapsed(
        *instruction_and_logical_time.first);
    instructions_elapsed_time[logical_time] =
        elapsed_time *
        IPow<float>(cost_analysis_.options()
                        .xla_tpu_memory_space_assignment_while_execution_count,
                    while_nest_level);
  }
  // As an optimization, create a cumulative sum vector of elapsed time.
  float cumsum = 0.0;
  elapsed_time_cumsum_.reserve(instructions_elapsed_time.size());
  for (float elapsed_time : instructions_elapsed_time) {
    cumsum += elapsed_time;
    elapsed_time_cumsum_.push_back(cumsum);
  }
  // To be able to accurately determine the minimum nest level between a start
  // time and an end time efficiently, populate a data structure that stores the
  // closest 'smaller' nest level change index.
  const int64_t size = instructions_elapsed_time.size();
  CHECK_EQ(size, while_nest_level_.size());
  std::vector<int> most_recent_by_level(while_nest_level_.size(), -1);
  int prev_nest_level = 0;
  int change_idx = -1;
  while_nest_level_change_.reserve(size);
  for (int i = 0; i < size; ++i) {
    int nest_level = while_nest_level_[i];
    if (nest_level != prev_nest_level) {
      prev_nest_level = nest_level;
      // Compute last change index by choosing the most recent instruction index
      // with smaller nesting level. Note that it may happen that even though
      // there were few different regions with other nest levels before, all of
      // then are same or bigger than this one, in which case we'll end up with
      // -1, e.g. if you got nest level 0 no need checking anything else.
      change_idx = -1;
      for (int smaller_level = 0; smaller_level < nest_level; smaller_level++) {
        change_idx = std::max(change_idx, most_recent_by_level[smaller_level]);
      }
    }
    most_recent_by_level[nest_level] = i;
    while_nest_level_change_.push_back(change_idx);
  }
  for (int i = 0; i <= max_while_nest_level; ++i) {
    while_execution_counts_.push_back(
        IPow<float>(cost_analysis_.options()
                        .xla_tpu_memory_space_assignment_while_execution_count,
                    i));
  }
}

float CostAnalysisPrefetchIntervalPicker::GetMaxElapsedInAlternateMemory(
    float async_copy_elapsed) const {
  return max_async_copy_elapsed_;
}

bool CostAnalysisPrefetchIntervalPicker::CanAllocateInAlternateMemoryNoCopy(
    const Shape& shape, int64_t start_time, int64_t end_time) const {
  // Even though this method returns if we allow the buffer in alternate memory
  // _without_ asynchronous copies, calculate how long it would have taken to
  // copy it and compare it to the elapsed time in the logical interval.
  float async_copy_elapsed = cost_analysis_.GetAsyncCopyElapsed(
      shape_override_ ? *shape_override_ : shape);
  float logical_interval_elapsed =
      GetLogicalIntervalElapsed(start_time, end_time);
  return GetMaxElapsedInAlternateMemory(async_copy_elapsed) >
         logical_interval_elapsed;
}

int64_t CostAnalysisPrefetchIntervalPicker::PreferredEvictionEndTime(
    const Shape& shape, int64_t start_time, int64_t latest_end_time) const {
  float async_copy_elapsed = cost_analysis_.GetAsyncCopyElapsed(
      shape_override_ ? *shape_override_ : shape);
  int64_t end_time;
  for (end_time = start_time + 1; end_time <= latest_end_time; ++end_time) {
    float logical_interval_elapsed =
        GetLogicalIntervalElapsed(start_time, end_time);
    if (logical_interval_elapsed >=
        (1 + kEvictionRetryMultiplier * retry_number_) *
            preferred_overlap_to_async_copy_ratio_ * async_copy_elapsed) {
      break;
    }
  }
  return end_time;
}

int64_t CostAnalysisPrefetchIntervalPicker::LatestPrefetchStartTime(
    const Shape& shape, int64_t start_time, int64_t end_time,
    const HloUse* use) const {
  // Find the earliest time that satisfies max_overlap_to_async_copy_ratio_.
  float async_copy_elapsed = cost_analysis_.GetAsyncCopyElapsed(
      shape_override_ ? *shape_override_ : shape);
  // If there is a use, estimate the time we would save by having this op in
  // alternate memory.
  float inst_elapsed_reduction = 0.0f;
  if (use) {
    float elapsed_time =
        cost_analysis_.GetInstructionElapsed(*use->instruction);
    float elapsed_time_in_alternate_mem =
        cost_analysis_.GetInstructionElapsedInAlternateMemory(
            *use->instruction,
            /*operands_in_alternate_mem=*/
            {std::make_pair(use->operand_number, use->operand_index)},
            /*outputs_in_alternate_mem=*/{});
    inst_elapsed_reduction = elapsed_time - elapsed_time_in_alternate_mem;
  }
  int end_nest_level = computation_nest_level_[end_time];

  // Find the latest time we're allowed to start prefetching.
  float min_interval = min_overlap_to_async_copy_ratio_ * async_copy_elapsed;
  int latest_prefetch_time;
  for (latest_prefetch_time = end_time - 1;
       latest_prefetch_time >= start_time &&
       (computation_nest_level_[latest_prefetch_time] != end_nest_level ||
        min_interval >
            GetLogicalIntervalElapsed(latest_prefetch_time, end_time) +
                inst_elapsed_reduction);
       --latest_prefetch_time) {
  }

  return latest_prefetch_time;
}

int64_t CostAnalysisPrefetchIntervalPicker::PreferredPrefetchStartTime(
    const Shape& shape, int64_t earliest_prefetch_start_time,
    int64_t latest_prefetch_start_time, int64_t prefetch_end_time) const {
  // Between the earliest and latest prefetch interval, find the interval
  // closest to the preferred interval and start iterating from there.
  float async_copy_elapsed = cost_analysis_.GetAsyncCopyElapsed(
      shape_override_ ? *shape_override_ : shape);
  int64_t preferred_prefetch_start_time = earliest_prefetch_start_time;
  float preferred_interval =
      preferred_overlap_to_async_copy_ratio_ * async_copy_elapsed;
  float best_interval = GetLogicalIntervalElapsed(earliest_prefetch_start_time,
                                                  prefetch_end_time);
  int end_nest_level = computation_nest_level_[prefetch_end_time];
  for (int64_t prefetch_start_time = earliest_prefetch_start_time + 1;
       prefetch_start_time <= latest_prefetch_start_time;
       ++prefetch_start_time) {
    float interval =
        GetLogicalIntervalElapsed(prefetch_start_time, prefetch_end_time);
    if (computation_nest_level_[prefetch_start_time] == end_nest_level &&
        std::abs(preferred_interval - interval) <
            std::abs(preferred_interval - best_interval)) {
      best_interval = interval;
      preferred_prefetch_start_time = prefetch_start_time;
    }
  }
  return preferred_prefetch_start_time;
}

int64_t CostAnalysisPrefetchIntervalPicker::LatestPrefetchEndTime(
    int64_t original_prefetch_end_time,
    int64_t proposed_prefetch_end_time) const {
  // Iterate towards the beginning until we find a suitable end time that is the
  // same while nest level as the original prefetch end time.
  int64_t original_nest_level =
      computation_nest_level_[original_prefetch_end_time];
  int64_t new_prefetch_end_time;
  for (new_prefetch_end_time = proposed_prefetch_end_time;
       computation_nest_level_[new_prefetch_end_time] != original_nest_level;
       --new_prefetch_end_time) {
  }
  return new_prefetch_end_time;
}

int64_t CostAnalysisPrefetchIntervalPicker::EstimatedPrefetchEndTime(
    const Shape& shape, int64_t start_time, int64_t end_time) const {
  float async_copy_elapsed = cost_analysis_.GetAsyncCopyElapsed(
      shape_override_ ? *shape_override_ : shape);
  int64_t estimated_end_time;
  for (estimated_end_time = start_time + 1; estimated_end_time < end_time;
       ++estimated_end_time) {
    float interval = GetLogicalIntervalElapsed(start_time, estimated_end_time);
    if (interval >= async_copy_elapsed) {
      break;
    }
  }
  return estimated_end_time;
}

void CostAnalysisPrefetchIntervalPicker::Begin(
    const HloUse& use, int64_t start_time, int64_t end_time,
    std::optional<int64_t> preferred_time) {
  const Shape& shape = ShapeUtil::GetSubshape(
      use.instruction->operand(use.operand_number)->shape(), use.operand_index);
  // Find the earliest time that satisfies max_overlap_to_async_copy_ratio_.
  async_copy_elapsed_ = cost_analysis_.GetAsyncCopyElapsed(
      shape_override_ ? *shape_override_ : shape);
  // Estimate the time we would save by having this op in alternate memory.
  float elapsed_time = cost_analysis_.GetInstructionElapsed(*use.instruction);
  float elapsed_time_in_alternate_mem =
      cost_analysis_.GetInstructionElapsedInAlternateMemory(
          *use.instruction, /*operands_in_alternate_mem=*/
          {std::make_pair(use.operand_number, use.operand_index)},
          /*outputs_in_alternate_mem=*/{});
  inst_elapsed_reduction_ = elapsed_time - elapsed_time_in_alternate_mem;
  end_logical_time_ = end_time;
  int end_nest_level = computation_nest_level_[end_logical_time_];

  // Find the latest time we're allowed to start prefetching.
  float min_interval = min_overlap_to_async_copy_ratio_ * async_copy_elapsed_;
  latest_prefetch_time_ =
      LatestPrefetchStartTime(shape, start_time, end_time, &use);

  // Find the earliest time we're allowed to start prefetching.
  float max_interval = GetMaxElapsedInAlternateMemory(async_copy_elapsed_);
  for (earliest_prefetch_time_ = start_time;
       earliest_prefetch_time_ < latest_prefetch_time_ &&
       (computation_nest_level_[earliest_prefetch_time_] != end_nest_level ||
        max_interval < GetLogicalIntervalElapsed(earliest_prefetch_time_,
                                                 end_logical_time_));
       ++earliest_prefetch_time_) {
  }
  if (earliest_prefetch_time_ > latest_prefetch_time_) {
    // There is no available prefetch interval for the given start and end
    // times. Set the iterators accordingly to ensure Done() returns true.
    increasing_prefetch_time_iterator_ = earliest_prefetch_time_;
    decreasing_prefetch_time_iterator_ = latest_prefetch_time_;
    CHECK(Done());
    return;
  }

  int64_t starting_prefetch_time;
  if (preferred_time && *preferred_time <= latest_prefetch_time_) {
    starting_prefetch_time = *preferred_time;
  } else {
    starting_prefetch_time =
        PreferredPrefetchStartTime(shape, earliest_prefetch_time_,
                                   latest_prefetch_time_, end_logical_time_);
  }
  float preferred_interval =
      preferred_overlap_to_async_copy_ratio_ * async_copy_elapsed_;
  VLOG(4) << "Interval min/max/preferred = " << min_interval << " "
          << max_interval << " " << preferred_interval
          << " prefetch time earliest/latest/starting = "
          << earliest_prefetch_time_ << " " << latest_prefetch_time_ << " "
          << starting_prefetch_time;

  increasing_prefetch_time_iterator_ = starting_prefetch_time;
  decreasing_prefetch_time_iterator_ = starting_prefetch_time;
  using_increasing_prefetch_time_iterator_ = true;
  // Since both iterators start at the same position, call Next() once to
  // advance one of the iterators.
  Next();
}

int64_t CostAnalysisPrefetchIntervalPicker::Next() {
  CHECK(!Done()) << "Prefetch interval picker's Next() is called even though "
                    "Done() is false";
  if (using_increasing_prefetch_time_iterator_) {
    int64_t prefetch_time = increasing_prefetch_time_iterator_++;
    while (increasing_prefetch_time_iterator_ <= latest_prefetch_time_ &&
           computation_nest_level_[increasing_prefetch_time_iterator_] !=
               computation_nest_level_[end_logical_time_]) {
      ++increasing_prefetch_time_iterator_;
    }
    if (decreasing_prefetch_time_iterator_ >= earliest_prefetch_time_) {
      using_increasing_prefetch_time_iterator_ = false;
    }
    return prefetch_time;
  } else {
    int64_t prefetch_time = decreasing_prefetch_time_iterator_--;
    // As a compilation time optimization, reduce the number of intervals that
    // this prefetch interval picker returns. When we run out of the increasing
    // prefetch time iterator, only explore up to
    // kNumExploredDecreasingIntervals intervals. To do that, calculate the
    // 1/kNumExploredDecreasingIntervals of the elapsed time between the
    // earliest prefetch time and the use, and decrement the iterator until the
    // prefetch elapsed time is at least as large as this target value. This
    // allows us to reduce the number of expensive heap fit and resource checks
    // when the graph consists of a large number of fast-executing HLOs.
    //
    // Shown pictorially, assuming kNumExploredDecreasingIntervals = 3 and the
    // numbers indicating the elapsed time of the HLOs, only the indicated
    // options for prefetch start time would be explored:
    //
    //    ---1---1---3---1---1---1---1---0---0---0---0---1---5---X
    //     ^           ^                                   ^     ^
    //  Option3     Option2                             Option1 Use
    // (Earliest)
    float next_target_interval_elapsed = 0;
    if (increasing_prefetch_time_iterator_ > latest_prefetch_time_) {
      next_target_interval_elapsed =
          GetLogicalIntervalElapsed(prefetch_time, end_logical_time_) +
          (GetLogicalIntervalElapsed(earliest_prefetch_time_,
                                     end_logical_time_) /
           kNumExploredDecreasingIntervals);
      VLOG(3) << "Next target interval elapsed: "
              << next_target_interval_elapsed;
    }
    while (decreasing_prefetch_time_iterator_ >= earliest_prefetch_time_ &&
           (computation_nest_level_[decreasing_prefetch_time_iterator_] !=
                computation_nest_level_[end_logical_time_] ||
            GetLogicalIntervalElapsed(decreasing_prefetch_time_iterator_,
                                      end_logical_time_) <
                next_target_interval_elapsed)) {
      --decreasing_prefetch_time_iterator_;
    }
    if (increasing_prefetch_time_iterator_ <= latest_prefetch_time_) {
      using_increasing_prefetch_time_iterator_ = true;
    }
    return prefetch_time;
  }
}

bool CostAnalysisPrefetchIntervalPicker::Done() const {
  return increasing_prefetch_time_iterator_ > latest_prefetch_time_ &&
         decreasing_prefetch_time_iterator_ < earliest_prefetch_time_;
}

int64_t CostAnalysisPrefetchIntervalPicker::latest_time() const {
  return latest_prefetch_time_;
}

void CostAnalysisPrefetchIntervalPicker::SetRetryNumber(int retry_number) {
  retry_number_ = retry_number;
}

int CostAnalysisPrefetchIntervalPicker::GetMinWhileNestLevel(
    int64_t start_time, int64_t end_time) const {
  int min_nest_level =
      std::min(while_nest_level_[start_time], while_nest_level_[end_time]);
  int change_idx = while_nest_level_change_[end_time];
  while (change_idx >= start_time) {
    min_nest_level = std::min(min_nest_level, while_nest_level_[change_idx]);
    change_idx = while_nest_level_change_[change_idx];
  }
  return min_nest_level;
}

float CostAnalysisPrefetchIntervalPicker::GetLogicalIntervalElapsed(
    int64_t start_time, int64_t end_time) const {
  CHECK_LE(start_time, end_time);
  if (start_time == end_time) {
    return 0.0;
  }
  if (start_time < 0) {
    start_time = 0;
  }
  // Since elapsed_time_cumsum_ is already weighed by the while loop nesting
  // level, normalize the elapsed time by dividing with the nesting factor of
  // the interval (start and end times).
  int interval_while_nest_level = GetMinWhileNestLevel(start_time, end_time);
  return (elapsed_time_cumsum_[end_time - 1] -
          elapsed_time_cumsum_[start_time]) /
         while_execution_counts_[interval_while_nest_level];
}

std::string CostAnalysisPrefetchIntervalPicker::ToDebugString() const {
  int current_logical_prefetch_time = using_increasing_prefetch_time_iterator_
                                          ? increasing_prefetch_time_iterator_
                                          : decreasing_prefetch_time_iterator_;
  float logical_interval_elapsed = GetLogicalIntervalElapsed(
      current_logical_prefetch_time, end_logical_time_);
  return absl::StrCat(
      "Async copy elapsed (s) = ", async_copy_elapsed_,
      ", inst elapsed reduction (s) = ", inst_elapsed_reduction_,
      ", logical interval elapsed (s) = ", logical_interval_elapsed,
      ", interval = (", current_logical_prefetch_time, ", ", end_logical_time_,
      ")");
}

std::string CostAnalysisPrefetchIntervalPicker::ToNoCopyDebugString(
    const Shape& shape, int64_t start_time, int64_t end_time) const {
  float async_copy_elapsed = cost_analysis_.GetAsyncCopyElapsed(
      shape_override_ ? *shape_override_ : shape);
  float logical_interval_elapsed =
      GetLogicalIntervalElapsed(start_time, end_time);
  return absl::StrCat(
      "Async copy elapsed (s) = ", async_copy_elapsed,
      ", logical interval elapsed (s) = ", logical_interval_elapsed);
}

std::optional<float>
CostAnalysisPrefetchIntervalPicker::BufferIntervalAlternateMemoryBenefit(
    const GlobalDecreasingSizeBestFitHeap<HloValue>::BufferInterval& interval)
    const {
  return cost_analysis_.GetMemoryBoundedness(interval);
}

/*static*/ StatusOr<std::vector<FilterUpdatePreferredPrefetch>>
FilterUpdatePreferredPrefetch::ParseFilterUpdatePreferredPrefetches(
    std::string config) {
  if (config.empty()) {
    return std::vector<FilterUpdatePreferredPrefetch>();
  }
  std::vector<FilterUpdatePreferredPrefetch> filter_update_prefetches;
  std::vector<std::string> filter_update_configs = absl::StrSplit(config, ';');
  for (const auto& config : filter_update_configs) {
    TF_ASSIGN_OR_RETURN(auto filter_update_prefetch,
                        ParseFilterUpdatePreferredPrefetch(config));
    filter_update_prefetches.push_back(filter_update_prefetch);
  }
  return filter_update_prefetches;
}

/*static*/ StatusOr<bool> FilterUpdatePreferredPrefetch::IsOpSizeGte(
    int64_t operand_size, std::string config) {
  int64_t config_value;
  if (!absl::SimpleAtoi(config, &config_value)) {
    return InvalidArgument("Expected integer, got %s for operand size filter",
                           config);
  }
  return operand_size >= config_value;
}

/*static*/ StatusOr<bool> FilterUpdatePreferredPrefetch::IsOpSizeLte(
    int64_t operand_size, std::string config) {
  int64_t config_value;
  if (!absl::SimpleAtoi(config, &config_value)) {
    return InvalidArgument("Expected integer, got %s for operand size filter",
                           config);
  }
  return operand_size <= config_value;
}

/*static*/ StatusOr<bool> FilterUpdatePreferredPrefetch::IsInstructionNameExact(
    const absl::string_view instruction_name, std::string config) {
  return instruction_name == config;
}

/*static*/ StatusOr<bool> FilterUpdatePreferredPrefetch::IsOpNumberExact(
    int64_t operand_number, std::string config) {
  int64_t config_value;
  if (!absl::SimpleAtoi(config, &config_value)) {
    return InvalidArgument("Expected integer, got %s for operand number filter",
                           config);
  }
  return operand_number == config_value;
}

/*static*/ StatusOr<bool> FilterUpdatePreferredPrefetch::IsOpIndexExact(
    const ShapeIndex& operand_index, std::string config) {
  TF_ASSIGN_OR_RETURN(auto config_value, ParseOperandIndex(config));
  return operand_index == config_value;
}

StatusOr<std::optional<int64_t>>
FilterUpdatePreferredPrefetch::GetPrefetchByEagerness(
    int64_t earliest_prefetch_time, int64_t latest_prefetch_time) const {
  if (earliest_prefetch_time > latest_prefetch_time) {
    return static_cast<std::optional<int64_t>>(std::nullopt);
  }
  float override_value;
  if (!absl::SimpleAtof(override_value_, &override_value)) {
    return InvalidArgument("Expected float, got %s for prefetch eagerness",
                           override_value_);
  }
  return static_cast<std::optional<int64_t>>(
      earliest_prefetch_time * override_value +
      latest_prefetch_time * (1.0 - override_value));
}

StatusOr<std::optional<int64_t>>
FilterUpdatePreferredPrefetch::GetPrefetchTimeAfterInstruction(
    const absl::flat_hash_map<const xla::HloInstruction*,
                              xla::HloLiveRange::LogicalTime>& schedule) const {
  TF_ASSIGN_OR_RETURN(auto reference_instruction_time,
                      GetScheduleTimeFromInstructionName(schedule));
  return static_cast<std::optional<int64_t>>(reference_instruction_time);
}

StatusOr<std::optional<int64_t>>
FilterUpdatePreferredPrefetch::GetPrefetchTimeBeforeInstruction(
    const absl::flat_hash_map<const xla::HloInstruction*,
                              xla::HloLiveRange::LogicalTime>& schedule) const {
  TF_ASSIGN_OR_RETURN(auto reference_instruction_time,
                      GetScheduleTimeFromInstructionName(schedule));
  return static_cast<std::optional<int64_t>>(reference_instruction_time - 1);
}

StatusOr<xla::HloLiveRange::LogicalTime>
FilterUpdatePreferredPrefetch::GetScheduleTimeFromInstructionName(
    const absl::flat_hash_map<const xla::HloInstruction*,
                              xla::HloLiveRange::LogicalTime>& schedule) const {
  for (auto schedule_entry : schedule) {
    if (schedule_entry.first->name() == override_value_) {
      return schedule_entry.second;
    }
  }
  return NotFound("Reference instruction %s was not found in the schedule.",
                  override_value_);
}

/*static*/ StatusOr<FilterUpdatePreferredPrefetch::FilterType>
FilterUpdatePreferredPrefetch::ParseFilterType(std::string config) {
  if (config == "op_size_lte") {
    return FilterType::OP_SIZE_LTE;
  }
  if (config == "op_size_gte") {
    return FilterType::OP_SIZE_GTE;
  }
  if (config == "instruction_name_exact") {
    return FilterType::INSTRUCTION_NAME_EXACT;
  }
  if (config == "op_number_exact") {
    return FilterType::OP_NUMBER_EXACT;
  }
  if (config == "op_index_exact") {
    return FilterType::OP_INDEX_EXACT;
  }
  return InvalidArgument("Failed to parse filter type %s", config);
}

/*static*/ StatusOr<FilterUpdatePreferredPrefetch::OverrideType>
FilterUpdatePreferredPrefetch::ParseOverrideType(std::string config) {
  if (config == "prefetch_eagerness") {
    return OverrideType::PREFETCH_EAGERNESS;
  }
  if (config == "put_after_instruction") {
    return OverrideType::PUT_AFTER_INSTRUCTION;
  }
  if (config == "put_before_instruction") {
    return OverrideType::PUT_BEFORE_INSTRUCTION;
  }
  return InvalidArgument("Failed to parse override type %s", config);
}

/*static*/ StatusOr<ShapeIndex>
FilterUpdatePreferredPrefetch::ParseOperandIndex(std::string config) {
  ShapeIndex operand_index{};
  if (config.empty()) {
    return operand_index;
  }
  for (const absl::string_view& index_string : absl::StrSplit(config, '#')) {
    int64_t index;
    if (!absl::SimpleAtoi(index_string, &index)) {
      return InvalidArgument("Failed to parse operand_index %s", config);
    }
    operand_index.push_back(index);
  }
  return operand_index;
}

/*static*/ StatusOr<FilterUpdatePreferredPrefetch>
FilterUpdatePreferredPrefetch::ParseFilterUpdatePreferredPrefetch(
    std::string config) {
  std::vector<std::string> filter_update_config = absl::StrSplit(config, ':');
  if (filter_update_config.size() < 4 || filter_update_config.size() % 2 != 0) {
    return InvalidArgument(
        "Failed to parse filter update config %s, incorrect number of "
        "arguments",
        config);
  }
  FilterUpdatePreferredPrefetch result;
  result.config_string_ = config;
  for (int i = 0; i < filter_update_config.size() - 2; i += 2) {
    TF_ASSIGN_OR_RETURN(auto filter_type,
                        ParseFilterType(filter_update_config[i]));
    result.filter_list_.push_back(
        std::make_pair(filter_type, filter_update_config[i + 1]));
  }
  TF_ASSIGN_OR_RETURN(
      result.override_type_,
      ParseOverrideType(filter_update_config[filter_update_config.size() - 2]));
  result.override_value_ = filter_update_config.back();
  return result;
}

bool MemorySpaceAssignment::Allocation::operator==(
    const MemorySpaceAssignment::Allocation& other) const {
  return defining_position() == other.defining_position() &&
         uses() == other.uses() && memory_space() == other.memory_space() &&
         chunk() == other.chunk() && start_time() == other.start_time() &&
         end_time() == other.end_time() &&
         earliest_available_time() == other.earliest_available_time() &&
         is_copy_allocation() == other.is_copy_allocation() &&
         is_scoped_allocation() == other.is_scoped_allocation();
}

bool MemorySpaceAssignment::CopyAllocation::operator==(
    const MemorySpaceAssignment::CopyAllocation& other) const {
  return static_cast<const Allocation&>(*this) ==
             static_cast<const Allocation&>(other) &&
         copy_done_schedule_before() == other.copy_done_schedule_before() &&
         copy_start_schedule_after() == other.copy_start_schedule_after() &&
         copy_start() == other.copy_start() && copy_done() == other.copy_done();
}

std::string MemorySpaceAssignment::AllocationValue::ToString() const {
  std::string out = absl::StrCat("computation = ", computation()->name());
  absl::StrAppend(&out,
                  (requires_contiguous_allocation_ ? " (cont alloc)" : ""));
  absl::StrAppend(&out, "\n position:\n");
  absl::StrAppend(&out, "  ", defining_position_.ToString(), "\n");
  absl::StrAppend(&out, " uses:\n");
  for (const Use& use : uses_) {
    absl::StrAppend(&out, "  ", use.hlo_use.ToString(), "\n");
  }
  return out;
}

std::string MemorySpaceAssignment::AllocationValue::ToShortString() const {
  return absl::StrCat("computation = ", computation()->name(),
                      ", position = ", defining_position_.ToString(),
                      ", value = ", value_->ToShortString(),
                      (requires_contiguous_allocation_ ? " (cont alloc)" : ""));
}

AlternateMemoryBestFitHeap::AlternateMemoryBestFitHeap(
    MemorySpaceAssignment::AllocationSequence* allocations,
    const Options& options, const HloAliasAnalysis& alias_analysis,
    const HloLiveRange& hlo_live_range)
    : GlobalDecreasingSizeBestFitHeap(options.alignment_in_bytes),
      allocations_(allocations),
      options_(options),
      alias_analysis_(alias_analysis),
      hlo_live_range_(hlo_live_range),
      peak_memory_usage_(hlo_live_range.schedule_end_time() + 1) {
  // Override buffer interval compare if provided.
  if (options.buffer_interval_compare) {
    buffer_interval_compare_ = *options.buffer_interval_compare;
  }

  call_graph_ = CallGraph::Build(&alias_analysis_.dataflow_analysis().module());

  std::vector<float> initial_resources(hlo_live_range.schedule_end_time(), 1.0);
  if (options.cost_analysis) {
    const std::vector<HloInstruction*>& flattened_instructions =
        hlo_live_range.flattened_instruction_sequence().instructions();
    for (int i = 0; i < flattened_instructions.size(); ++i) {
      const HloInstruction* inst = flattened_instructions[i];
      if (inst->opcode() == HloOpcode::kWhile ||
          inst->opcode() == HloOpcode::kConditional) {
        initial_resources[i] = 0;
      } else {
        initial_resources[i] =
            options.cost_analysis->GetInstructionElapsed(*inst);
        if (options_.use_repeated_instance_for_preferred_prefetch_time ||
            options_.memory_bound_loop_optimizer_options.enabled()) {
          std::string fingerprint;
          absl::StrAppend(&fingerprint, inst->shape().ToString(), " ",
                          HloOpcodeString(inst->opcode()), "(");
          for (int operand_idx = 0; operand_idx < inst->operands().size();
               ++operand_idx) {
            if (operand_idx > 0) {
              absl::StrAppend(&fingerprint, ", ");
            }
            absl::StrAppend(&fingerprint,
                            inst->operand(operand_idx)->shape().ToString());
          }
          absl::StrAppend(&fingerprint, ")");
          fingerprint_map_[inst] = fingerprint;
          repeated_inst_map_[fingerprint].push_back(inst);
        }
      }
      VLOG(2) << "Initial resource[" << i << "] = " << initial_resources[i]
              << " (" << inst->name() << ")";
    }
  }
  prefetch_async_copy_resource_ = AsynchronousCopyResource(initial_resources);
  eviction_async_copy_resource_ = AsynchronousCopyResource(initial_resources);
}

void AlternateMemoryBestFitHeap::CreateAllocationValues(
    const AlternateMemoryBestFitHeap::BufferInterval& buffer_interval,
    std::vector<AllocationValue>& allocation_values) const {
  const HloValue* value = buffer_interval.buffer;
  VLOG(3) << "Creating AllocationValues for: " << value->ToString();

  // Find and sort all non-trivial (excluding GTE, Tuple, and bitcast)
  // positions. We create an AllocationValue object for each non-trivial
  // position. And for each AllocationValue object, we create an
  // AllocationSequence consisting of one or more Allocation objects.The reason
  // why we exclude the trivial positions from AllocationValue is because
  // Allocation objects have special support for tuples and bitcasts.
  const absl::flat_hash_map<const HloInstruction*, int64_t>&
      instruction_schedule = hlo_live_range_.instruction_schedule();
  std::vector<HloPosition> positions;
  for (const HloPosition& position : value->positions()) {
    const HloInstruction* instruction = position.instruction;
    if (instruction->opcode() != HloOpcode::kGetTupleElement &&
        instruction->opcode() != HloOpcode::kTuple &&
        instruction->opcode() != HloOpcode::kBitcast) {
      positions.push_back(position);
    }
  }
  absl::c_stable_sort(positions,
                      [&](const HloPosition& pos1, const HloPosition& pos2) {
                        return instruction_schedule.at(pos1.instruction) <
                               instruction_schedule.at(pos2.instruction);
                      });

  // Create an AllocationValue for each non-trivial position.
  absl::flat_hash_set<const HloComputation*> computations;
  int beginning_idx = allocation_values.size();
  for (int i = 0; i < positions.size(); ++i) {
    const HloPosition& position = positions.at(i);
    allocation_values.emplace_back(value, position, buffer_interval.size);
  }

  std::vector<HloUse> uses(value->GetUses().begin(), value->GetUses().end());
  absl::c_stable_sort(uses, [&](const HloUse& use1, const HloUse& use2) {
    return instruction_schedule.at(use1.instruction) <
           instruction_schedule.at(use2.instruction);
  });

  // Associate each use with an AllocationValue. Each AllocationValue contains a
  // position and uses in the same computation. Furthermore, if the original
  // HloValue had multiple non-trivial positions in the same computation, those
  // will get their own AllocationValue as well. We split these HloValues so
  // that when we insert CopyStart/CopyDone in CopyAllocation::Process, they
  // point to the latest position. We then replace the operand of the use with
  // CopyStart/CopyDone with an operand of the latest position.
  for (const HloUse& use : uses) {
    int64_t use_time = instruction_schedule.at(use.instruction);
    HloComputation* use_computation = use.instruction->parent();

    AllocationValue* last_allocation_value = nullptr;
    for (int i = beginning_idx; i < allocation_values.size(); ++i) {
      AllocationValue* allocation_value = &allocation_values.at(i);
      if (HloDataflowAnalysis::IsAsynchronousOperationDone(
              use.instruction->opcode())) {
        if (allocation_value->defining_instruction() ==
                use.instruction->operand(0) &&
            use.operand_index == allocation_value->defining_position().index) {
          last_allocation_value = allocation_value;
        }
      } else if (!HloDataflowAnalysis::IsAsynchronousOperationStart(
                     allocation_value->defining_instruction()->opcode()) &&
                 allocation_value->computation() == use_computation &&
                 instruction_schedule.at(
                     allocation_value->defining_position().instruction) <
                     use_time) {
        last_allocation_value = allocation_value;
      }
    }
    CHECK(last_allocation_value != nullptr);
    last_allocation_value->AddUse(use, use_time);
  }

  for (int i = beginning_idx; i < allocation_values.size(); ++i) {
    AllocationValue& allocation_value = allocation_values.at(i);
    if (HloDataflowAnalysis::IsAsynchronousOperationStart(
            allocation_value.defining_instruction()->opcode())) {
      CHECK_EQ(allocation_value.uses().size(), 1);
      CHECK(HloDataflowAnalysis::IsAsynchronousOperationDone(
          allocation_value.uses().at(0).hlo_use.instruction->opcode()));
      VLOG(3) << "Mark " << allocation_value.ToShortString()
              << " to require contiguous allocation.";
      allocation_value.set_requires_contiguous_allocation(true);
    }
    VLOG(3) << "Created allocation value: "
            << allocation_values.at(i).ToString();
  }
}

void AlternateMemoryBestFitHeap::FindAliases(
    std::vector<AllocationValue>* allocation_values) const {
  absl::flat_hash_map<const HloInstruction*,
                      std::vector<const AllocationValue*>>
      values_by_defining_inst;
  for (AllocationValue& value : *allocation_values) {
    values_by_defining_inst[value.defining_instruction()].push_back(&value);
  }
  auto maybe_add_alias_with_instruction = [&](const HloInstruction* instruction,
                                              AllocationValue::Use* use) {
    auto aliased_values_it = values_by_defining_inst.find(instruction);
    if (aliased_values_it != values_by_defining_inst.end()) {
      for (const AllocationValue* aliased_value : aliased_values_it->second) {
        VLOG(3) << "Adding aliasing for use " << use->hlo_use.ToString()
                << " to " << aliased_value->ToShortString();
        use->aliases.push_back(aliased_value->defining_position());
      }
    }
  };

  for (AllocationValue& value : *allocation_values) {
    for (AllocationValue::Use& use : value.uses()) {
      // Find any aliases with the instruction itself (operand and output must
      // alias).
      maybe_add_alias_with_instruction(use.hlo_use.instruction, &use);

      // Find any aliases with the parameters of called computations.
      for (const HloComputation* called_computation :
           use.hlo_use.instruction->called_computations()) {
        for (const HloInstruction* parameter_instruction :
             called_computation->parameter_instructions()) {
          maybe_add_alias_with_instruction(parameter_instruction, &use);
        }
      }

      // Special case for kWhile: the root of the body computation must alias as
      // well.
      if (use.hlo_use.instruction->opcode() == HloOpcode::kWhile) {
        HloPosition root_alias{
            use.hlo_use.instruction->while_body()->root_instruction(),
            use.hlo_use.operand_index};
        VLOG(3) << "Adding while body root aliasing for use "
                << use.hlo_use.ToString() << " to " << root_alias;
        use.aliases.push_back(root_alias);
      }
    }
  }
}

std::vector<const AlternateMemoryBestFitHeap::BufferInterval*>
AlternateMemoryBestFitHeap::GetSortedColocatedIntervals(
    const AlternateMemoryBestFitHeap::BufferInterval& interval) const {
  std::vector<const BufferInterval*> colocated_intervals;
  std::vector<const BufferInterval*> worklist = {&interval};
  while (!worklist.empty()) {
    const BufferInterval* item = worklist.back();
    worklist.pop_back();
    colocated_intervals.push_back(item);
    for (const HloValue* buffer_colocated : item->colocations) {
      worklist.push_back(&buffer_intervals_.at(buffer_colocated));
    }
  }

  absl::c_stable_sort(colocated_intervals, [&](const BufferInterval* x,
                                               const BufferInterval* y) {
    return std::make_pair(x->start, x->end) < std::make_pair(y->start, y->end);
  });
  return colocated_intervals;
}

bool AlternateMemoryBestFitHeap::IsUseAllowedInAlternateMemory(
    const AllocationValue& value, const HloUse& use) const {
  const auto& instruction_schedule = hlo_live_range_.instruction_schedule();
  if (!options_.is_use_allowed_in_alternate_mem_fn(use)) {
    return false;
  }
  if (use.instruction->opcode() == HloOpcode::kWhile) {
    HloComputation* while_body = use.instruction->while_body();

    // We don't want to allocate this buffer in alternate memory if it will be
    // evicted anyway. Find out if it has an early use or a late definition that
    // would make sense to keep it in the alternate memory.
    HloValue* parameter_value =
        &alias_analysis_.dataflow_analysis().GetUniqueValueAt(
            while_body->parameter_instruction(0), use.operand_index);
    int64_t parameter_time =
        instruction_schedule.at(while_body->parameter_instruction(0));
    int64_t root_time = instruction_schedule.at(while_body->root_instruction());
    int64_t min_use_time = root_time;
    for (const HloUse& parameter_use : parameter_value->GetUses()) {
      int64_t use_time = instruction_schedule.at(parameter_use.instruction);
      if (parameter_use.instruction->opcode() != HloOpcode::kGetTupleElement &&
          parameter_use.instruction->opcode() != HloOpcode::kTuple &&
          parameter_use.instruction->opcode() != HloOpcode::kBitcast &&
          use_time > parameter_time) {
        min_use_time = std::min(min_use_time, use_time);
      }
    }
    // If there is no use of this buffer inside the while loop, there is no need
    // to allocate it in the loop.
    if (min_use_time == root_time) {
      VLOG(4) << "While allocation not allowed in alternate memory. "
              << "use time = " << min_use_time << ", root time = " << root_time;
      return false;
    }
    const Shape& shape = parameter_value->shape();
    // Allow the buffer in alternate memory if the buffer has a short live range
    // either at the beginning or end of the while loop body.
    if (!options_.prefetch_interval_picker->CanAllocateInAlternateMemoryNoCopy(
            shape, parameter_time, min_use_time)) {
      VLOG(4) << "While allocation not allowed in alternate memory. "
              << "use time = " << min_use_time << ", root time = " << root_time;
      return false;
    }
    // Check if there is a required assignment for the while loop output.
    HloValue* while_value =
        &alias_analysis_.dataflow_analysis().GetUniqueValueAt(
            use.instruction, use.operand_index);
    int64_t while_time = instruction_schedule.at(use.instruction);
    auto existing_required_assignment =
        RequiredMemoryAssignmentAt(while_value, while_time);
    if (existing_required_assignment &&
        existing_required_assignment->memory_space == MemorySpace::kDefault) {
      VLOG(4) << "While allocation not allowed in alternate memory because "
                 "there is a required default memory assignment.";
      return false;
    }
  } else if (use.instruction->opcode() == HloOpcode::kConditional) {
    // For any use of this conditional (the same value might be passed into
    // multiple called computations), determine if the parameter->first use
    // dependency is short.
    int64_t conditional_time = instruction_schedule.at(use.instruction);
    for (const AllocationValue::Use& other_use : value.uses()) {
      if (other_use.hlo_use.instruction != use.instruction) {
        continue;
      }
      // Operand 0 is not passed into the computation.
      if (other_use.hlo_use.operand_number == 0) {
        continue;
      }
      HloComputation* called_computation =
          use.instruction->called_computations().at(
              other_use.hlo_use.operand_number - 1);
      const HloInstruction* parameter_instruction =
          called_computation->parameter_instruction(0);
      HloValue* parameter_value =
          &alias_analysis_.dataflow_analysis().GetUniqueValueAt(
              parameter_instruction, other_use.hlo_use.operand_index);
      int64_t parameter_time = instruction_schedule.at(parameter_instruction);
      int64_t min_use_time = conditional_time;
      for (const HloUse& parameter_use : parameter_value->GetUses()) {
        if (parameter_use.instruction->parent() == called_computation &&
            parameter_use.instruction->opcode() !=
                HloOpcode::kGetTupleElement &&
            parameter_use.instruction->opcode() != HloOpcode::kTuple &&
            parameter_use.instruction->opcode() != HloOpcode::kBitcast) {
          min_use_time = std::min(
              min_use_time, instruction_schedule.at(parameter_use.instruction));
        }
      }
      if (options_.prefetch_interval_picker->CanAllocateInAlternateMemoryNoCopy(
              parameter_value->shape(), parameter_time, min_use_time)) {
        VLOG(4) << "Conditional allocation allowed in alternate memory for "
                   "computation = "
                << called_computation->name()
                << ", parameter time = " << parameter_time
                << ", min use time = " << min_use_time;
        return true;
      } else {
        VLOG(4) << "Conditional allocation not allowed in alternate memory for "
                   "computation = "
                << called_computation->name()
                << ", parameter time = " << parameter_time
                << ", min use time = " << min_use_time;
      }
    }
    return false;
  }

  return true;
}

namespace {
// Columns in buffer information:
// buffer_id: int. This value can be used to match the allocation in
// allocation information.
// buffer_name: string.
// alt_mem_benefit: float. Roughly corresponds to how much the cost analysis
// thought it would be beneficial to put this in the alternate memory. The
// higher the value, the more it is memory bound.
// size: int. In bytes.
// definition_time: int. Logical time this value was defined in the schedule.
// use_times: string. This is a semicolon-separated list of integers for all
// the use times.
// use_names: string. This is a semicolon-separated list of string
// representation of uses.
// is_scoped: int. A value of 1 indicates that the buffer is a scoped
// allocation.
constexpr absl::string_view kBufferInfoColumnNames =
    "buffer_id,buffer_name,alt_mem_benefit,size,definition_time,use_times,use_"
    "names,is_scoped";
}  // namespace

void AlternateMemoryBestFitHeap::AppendBufferInfoDebugString(
    const AlternateMemoryBestFitHeap::BufferInterval& interval,
    std::string* debug_str) const {
  if (debug_str->empty()) {
    // Append the column names.
    absl::StrAppend(debug_str, kBufferInfoColumnNames, "\n");
  }
  const HloBuffer& buffer =
      alias_analysis_.GetBufferContainingValue(*interval.buffer);
  const auto& instruction_schedule = hlo_live_range_.instruction_schedule();
  int64_t definition_time =
      instruction_schedule.at(interval.buffer->defining_position().instruction);
  std::vector<std::pair<int64_t, std::string>> uses;
  for (const HloValue* value : buffer.values()) {
    for (const HloUse& use : value->GetUses()) {
      uses.push_back(
          {instruction_schedule.at(use.instruction), use.ToString()});
    }
  }
  absl::c_sort(uses);
  std::vector<int64_t> use_times;
  std::vector<std::string> use_names;
  use_times.reserve(uses.size());
  use_names.reserve(uses.size());
  for (const auto& use : uses) {
    use_times.push_back(use.first);
    use_names.push_back(use.second);
  }

  absl::StrAppend(debug_str, buffer.id(), ",");
  absl::StrAppend(debug_str, "\"", interval.buffer->ToShortString(), "\",");
  auto alternate_memory_benefit =
      options_.prefetch_interval_picker->BufferIntervalAlternateMemoryBenefit(
          interval);
  absl::StrAppend(
      debug_str, alternate_memory_benefit ? *alternate_memory_benefit : 0, ",");
  absl::StrAppend(debug_str, interval.size, ",");
  absl::StrAppend(debug_str, definition_time, ",");
  absl::StrAppend(debug_str, "\"", absl::StrJoin(use_times, ";"), "\",");
  absl::StrAppend(debug_str, "\"", absl::StrJoin(use_names, ";"), "\",");
  absl::StrAppend(debug_str, "0");  // is_scoped
  absl::StrAppend(debug_str, "\n");
}

void AlternateMemoryBestFitHeap::AppendScopedAllocationBufferInfoDebugString(
    const HloInstruction* instruction, int64_t time, int64_t size,
    std::string& debug_str) const {
  if (debug_str.empty()) {
    // Append the column names.
    absl::StrAppend(&debug_str, kBufferInfoColumnNames, "\n");
  }
  const HloBuffer& buffer = alias_analysis_.GetUniqueBufferAt(instruction);

  // As a convention, we use negative values for scoped allocations.
  absl::StrAppend(&debug_str, -buffer.id(), ",");
  absl::StrAppend(&debug_str, "\"scoped allocation for ", instruction->name(),
                  "\",");
  absl::StrAppend(&debug_str, 0, ",");  // alt_mem_benefit
  absl::StrAppend(&debug_str, size, ",");
  absl::StrAppend(&debug_str, time, ",");
  absl::StrAppend(&debug_str, "\"\",");  // use_times
  absl::StrAppend(&debug_str, "\"\",");  // use_names
  absl::StrAppend(&debug_str, "1");      // is_scoped
  absl::StrAppend(&debug_str, "\n");
}

void AlternateMemoryBestFitHeap::AppendAllocationInfoDebugString(
    const MemorySpaceAssignment::Allocation& allocation,
    std::string& debug_str) const {
  // Columns in allocation information:
  // buffer_id: int. This value can be used the match with buffer info.
  // size: int. In bytes.
  // offset: int. In bytes.
  // start_time: int. Logical start time of the allocation.
  // end_time: int. Logical end time of the allocation.
  if (debug_str.empty()) {
    // Append the column names.
    absl::StrAppend(&debug_str, "buffer_id,size,offset,start_time,end_time\n");
  }
  if (allocation.memory_space() == MemorySpace::kAlternate) {
    const HloPosition& position = allocation.defining_position();
    const HloBuffer& buffer =
        alias_analysis_.GetUniqueBufferAt(position.instruction, position.index);
    // As a convention, we use negative values for scoped allocations.
    absl::StrAppend(
        &debug_str,
        allocation.is_scoped_allocation() ? -buffer.id() : buffer.id(), ",");
    absl::StrAppend(&debug_str, allocation.chunk().size, ",");
    absl::StrAppend(&debug_str, allocation.chunk().offset, ",");
    absl::StrAppend(&debug_str, allocation.start_time(), ",");
    absl::StrAppend(&debug_str, allocation.end_time(), "\n");
  }
}

void AlternateMemoryBestFitHeap::DumpDebugStringsIfEnabled() const {
  if (!options_.dump_fn) {
    return;
  }
  options_.dump_fn("bufferinfo", buffer_info_str_);
  options_.dump_fn("allocinfo", allocation_info_str_);
  options_.dump_fn("scheduleinfo", instruction_schedule_str_);
}

/*static*/ StatusOr<std::unique_ptr<MemoryBoundLoopOptimizer>>
MemoryBoundLoopOptimizer::Create(
    int loop_start, int loop_end, uint64_t alternate_memory_size,
    const MemoryBoundLoopOptimizerOptions& options,
    const HloLiveRange& hlo_live_range, const HloAliasAnalysis& alias_analysis,
    const MemorySpaceAssignmentCostAnalysis& cost_analysis,
    const BufferValue::SizeFunction& size_function) {
  std::unique_ptr<MemoryBoundLoopOptimizer> optimizer =
      absl::WrapUnique(new MemoryBoundLoopOptimizer(
          loop_start, loop_end, alternate_memory_size, options, hlo_live_range,
          alias_analysis, cost_analysis, size_function));
  TF_RETURN_IF_ERROR(optimizer->Initialize());
  return std::move(optimizer);
}

MemoryBoundLoopOptimizer::MemoryBoundLoopOptimizer(
    int loop_start, int loop_end, uint64_t alternate_memory_size,
    const MemoryBoundLoopOptimizerOptions& options,
    const HloLiveRange& hlo_live_range, const HloAliasAnalysis& alias_analysis,
    const MemorySpaceAssignmentCostAnalysis& cost_analysis,
    const BufferValue::SizeFunction& size_function)
    : loop_start_(loop_start),
      loop_end_(loop_end),
      loop_size_(loop_end - loop_start),
      alternate_memory_size_(alternate_memory_size),
      options_(options),
      hlo_live_range_(hlo_live_range),
      alias_analysis_(alias_analysis),
      cost_analysis_(cost_analysis),
      size_function_(size_function) {}

Status MemoryBoundLoopOptimizer::Initialize() {
  const auto& instruction_sequence =
      hlo_live_range_.flattened_instruction_sequence().instructions();
  VLOG(3) << "MemoryBoundLoopOptimizer::Initialize, loop start: " << loop_start_
          << ", loop end: " << loop_end_ << ", loop size: " << loop_size_;
  const HloComputation* loop_computation = nullptr;
  // Initialize the remaining memory array with the size of the alternate
  // memory. Also populate instructions_in_loop_ and
  // instructions_in_{prev,next}_iterations_ data structures to help find the
  // loop values.
  for (int i = loop_start_; i < loop_end_; ++i) {
    const HloInstruction* inst = instruction_sequence[i];
    instructions_in_loop_[inst] = i - loop_start_;
    VLOG(3) << "  inst in loop [" << (i - loop_start_) << "]: " << inst->name();
    if (!loop_computation) {
      loop_computation = inst->parent();
    } else {
      TF_RET_CHECK(loop_computation == inst->parent());
    }
    remaining_memory_.push_back(alternate_memory_size_);
  }

  for (int i = loop_start_ - loop_size_; i < loop_start_; ++i) {
    const HloInstruction* inst = instruction_sequence[i];
    instructions_in_prev_iteration_[inst] = i - loop_start_ + loop_size_;
  }
  for (int i = loop_end_; i < loop_end_ + loop_size_; ++i) {
    const HloInstruction* inst = instruction_sequence[i];
    instructions_in_next_iteration_[inst] = i - loop_end_;
  }

  // Create a tree set to keep track of all the values that the loop
  // instructions produce and consume. We use a tree set instead of a hash set
  // to ensure the iteration order is the same as insertion order. Since we
  // traverse the program in instruction order, the buffers would be inserted in
  // a deterministic order, so we'll be able to iterate over these buffers in a
  // deterministic order.
  std::set<const HloBuffer*> buffers_to_process;
  for (const auto& [instruction, idx] : instructions_in_loop_) {
    auto maybe_add_buffer = [&](const HloInstruction* instruction) {
      return [this, &buffers_to_process, instruction](const Shape& subshape,
                                                      const ShapeIndex& index) {
        if (!subshape.IsArray()) {
          return;
        }
        const HloBuffer& buffer =
            alias_analysis_.GetUniqueBufferAt(instruction, index);
        if (buffers_to_process.find(&buffer) == buffers_to_process.end()) {
          buffers_to_process.insert(&buffer);
        }
      };
    };
    ShapeUtil::ForEachSubshape(instruction->shape(),
                               maybe_add_buffer(instruction));
    for (const HloInstruction* operand : instruction->operands()) {
      ShapeUtil::ForEachSubshape(operand->shape(), maybe_add_buffer(operand));
    }
  }

  // Process the buffers and decide if they should be added as LoopValues.
  for (const HloBuffer* buffer : buffers_to_process) {
    MaybeCreateLoopValue(*buffer, loop_computation);
  }
  return OkStatus();
}

void MemoryBoundLoopOptimizer::MaybeCreateLoopValue(
    const HloBuffer& buffer, const HloComputation* loop_computation) {
  // Define helper lambdas to get the loop-relative index of the given
  // instruction.
  auto get_index_in_loop =
      [&](const HloInstruction* instruction,
          const absl::flat_hash_map<const HloInstruction*, int64_t>&
              instructions_in_loop,
          int64_t relative_index = 0) {
        std::optional<int64_t> loop_index;
        if (instructions_in_loop.contains(instruction)) {
          loop_index = hlo_live_range_.instruction_schedule().at(instruction) -
                       loop_start_ + relative_index;
          CHECK_GE(*loop_index, 0);
          CHECK_LT(*loop_index, loop_size_);
        }
        return loop_index;
      };
  auto get_index_in_current_iteration = [&](const HloInstruction* instruction) {
    return get_index_in_loop(instruction, instructions_in_loop_);
  };
  auto get_index_in_prev_iteration = [&](const HloInstruction* instruction) {
    return get_index_in_loop(instruction, instructions_in_prev_iteration_,
                             loop_size_);
  };
  auto get_index_in_next_iteration = [&](const HloInstruction* instruction) {
    return get_index_in_loop(instruction, instructions_in_next_iteration_,
                             -loop_size_);
  };

  loop_values_.push_back({});
  LoopValue& loop_value = loop_values_.back();
  float pos_bytes = 0;
  float use_bytes = 0;
  bool has_footer_consumer = false;
  for (const HloValue* value : buffer.values()) {
    // For each position and use of the value, populate the respecive position
    // and use fields for the current, previous, and next iterations along with
    // the loop indices.
    for (const HloPosition& position : value->positions()) {
      if (position.instruction->opcode() == HloOpcode::kGetTupleElement) {
        continue;
      }
      std::optional<int64_t> loop_index =
          get_index_in_current_iteration(position.instruction);
      std::optional<int64_t> prev_iteration_index;
      if (loop_index) {
        loop_value.loop_positions.push_back({*loop_index, position});
        VLOG(3) << "Pos match: " << position.instruction->name() << " at "
                << *loop_index;
      } else if ((prev_iteration_index =
                      get_index_in_prev_iteration(position.instruction))) {
        loop_value.prev_iteration_positions.push_back(
            {*prev_iteration_index, position});
        VLOG(3) << "Pos match (prev iteration): "
                << position.instruction->name() << " at "
                << *prev_iteration_index;
      } else if (loop_value.prev_iteration_positions.empty() &&
                 loop_value.loop_positions.empty() &&
                 position.instruction->parent() == loop_computation &&
                 !loop_value.header_position) {
        loop_value.header_position = position;
      }

      // Keep track of bytes accessed by this value.
      if (loop_index || prev_iteration_index) {
        float bytes_accessed =
            cost_analysis_.cost_analysis().output_bytes_accessed(
                *position.instruction, position.index);
        pos_bytes += bytes_accessed;
        VLOG(3) << " accessed: " << bytes_accessed;
      }
    }

    for (const HloUse& use : value->GetUses()) {
      if (use.instruction->opcode() == HloOpcode::kGetTupleElement) {
        continue;
      }
      std::optional<int64_t> loop_index =
          get_index_in_current_iteration(use.instruction);
      std::optional<int64_t> next_iteration_index;
      if (loop_index) {
        loop_value.loop_uses.push_back({*loop_index, use});
        VLOG(3) << "Use match: " << use.instruction->name() << " at "
                << *loop_index;
      } else if ((next_iteration_index =
                      get_index_in_next_iteration(use.instruction))) {
        loop_value.next_iteration_uses.push_back({*next_iteration_index, use});
        VLOG(3) << "Use match (next iteration): " << use.instruction->name()
                << " at " << *next_iteration_index;
      } else if (!loop_value.loop_positions.empty() ||
                 !loop_value.loop_uses.empty()) {
        has_footer_consumer = true;
      }

      // Keep track of bytes accessed by this value.
      if (loop_index || next_iteration_index) {
        float bytes_accessed =
            cost_analysis_.cost_analysis().operand_bytes_accessed(
                *use.instruction, use.operand_number, use.operand_index);
        use_bytes += bytes_accessed;
        VLOG(3) << " accessed: " << bytes_accessed;
      }
    }
  }

  // We only add the loop position if it has a position or use in the current
  // iteration and its previous iteration positions are empty. The reason why we
  // disallow values with previous iteration positions is because there will be
  // a different value that corresponds to the same value but one iteration
  // later, so we will add that one instead.
  if ((!loop_value.loop_positions.empty() || !loop_value.loop_uses.empty()) &&
      loop_value.prev_iteration_positions.empty()) {
    loop_value.size = size_function_(**buffer.values().begin());
    VLOG(3) << "Size: " << loop_value.size;
    // Classify the type of allocation. See the comment in LoopValue definition.
    loop_value.allocation_type = LoopValue::AllocationType::kUnsupported;
    auto position_compare = [](const std::pair<int64_t, HloPosition>& a,
                               const std::pair<int64_t, HloPosition>& b) {
      return a.first < b.first;
    };
    auto use_compare = [](const std::pair<int64_t, HloUse>& a,
                          const std::pair<int64_t, HloUse>& b) {
      return a.first < b.first;
    };
    absl::c_sort(loop_value.loop_positions, position_compare);
    absl::c_sort(loop_value.prev_iteration_positions, position_compare);
    absl::c_sort(loop_value.loop_uses, use_compare);
    absl::c_sort(loop_value.next_iteration_uses, use_compare);
    if (!loop_value.loop_positions.empty()) {
      if (loop_value.next_iteration_uses.empty() &&
          !loop_value.loop_uses.empty()) {
        loop_value.allocation_type = LoopValue::AllocationType::kTemporary;
      } else if (!loop_value.next_iteration_uses.empty()) {
        if (loop_value.next_iteration_uses.back().first >=
            loop_value.loop_positions.front().first) {
          loop_value.allocation_type =
              LoopValue::AllocationType::kLoopCarriedDependence;
        } else {
          loop_value.allocation_type = LoopValue::AllocationType::kTemporary;
        }
      }
    } else if (loop_value.header_position && !loop_value.loop_uses.empty()) {
      if (loop_value.loop_uses.size() ==
              loop_value.next_iteration_uses.size() &&
          loop_value.loop_uses.front().first ==
              loop_value.next_iteration_uses.front().first) {
        loop_value.allocation_type = LoopValue::AllocationType::kPinned;
      } else if (loop_value.next_iteration_uses.empty() ||
                 loop_value.next_iteration_uses.back().first <
                     loop_value.loop_uses.front().first) {
        loop_value.allocation_type = LoopValue::AllocationType::kPrefetch;
      }
    }

    VLOG(3) << "Allocation type "
            << LoopValue::AllocationTypeToString(loop_value.allocation_type);
    VLOG(3) << "Pos bytes: " << pos_bytes << " use bytes: " << use_bytes;

    // We calculate the savings of allocating this buffer in the alternate
    // memory.
    float savings = pos_bytes + use_bytes;
    if (loop_value.header_position) {
      savings -= loop_value.size;
    }
    if (!loop_value.loop_positions.empty() && has_footer_consumer) {
      savings -= loop_value.size;
    }
    loop_value.savings = savings;
    loop_value.savings_per_byte = savings / loop_value.size;
    VLOG(3) << "Savings: " << loop_value.savings;
    VLOG(3) << "Savings per byte: " << loop_value.savings_per_byte;
    for (const HloValue* value : buffer.values()) {
      VLOG(3) << value->ToString();
    }
    auto sort_positions = [](const std::pair<int64_t, HloPosition>& a,
                             const std::pair<int64_t, HloPosition>& b) {
      return a.first < b.first;
    };
    auto sort_uses = [](const std::pair<int64_t, HloUse>& a,
                        const std::pair<int64_t, HloUse>& b) {
      return a.first < b.first;
    };
    absl::c_sort(loop_value.loop_positions, sort_positions);
    absl::c_sort(loop_value.prev_iteration_positions, sort_positions);
    absl::c_sort(loop_value.loop_uses, sort_uses);
    absl::c_sort(loop_value.next_iteration_uses, sort_uses);
    loop_value.hlo_values = buffer.values();
  } else {
    loop_values_.pop_back();
  }
}

void MemoryBoundLoopOptimizer::Optimize() {
  SortLoopValues();
  AllocateLoopValues();
  PostProcess();
}

float MemoryBoundLoopOptimizer::CalculateExecutionTime() const {
  // First populate the list of prefetches.
  std::vector<std::pair<const MemorySpaceAssignment::CopyAllocation*, float>>
      prefetches;
  for (const LoopValue& value : loop_values_) {
    if (!value.allocations.empty() &&
        value.allocations.back()->is_copy_allocation()) {
      prefetches.push_back(
          {static_cast<const MemorySpaceAssignment::CopyAllocation*>(
               value.allocations.back().get()),
           cost_analysis_.GetAsyncCopyElapsed(
               value.hlo_values.front()->shape())});
    }
  }

  // Returns the effective prefetch completion time. The effective time is a
  // value that will be larger than loop size for prefetches that start in this
  // iteration but complete in the next iteration.
  auto get_effective_done_time =
      [&](int64_t copy_start_schedule_after,
          int64_t copy_done_schedule_before) -> int64_t {
    if (copy_start_schedule_after == loop_size_ - 1 &&
        copy_done_schedule_before == 0) {
      return 2 * loop_size_;
    }
    if (copy_start_schedule_after + 1 >= copy_done_schedule_before) {
      return copy_done_schedule_before + loop_size_;
    }
    return copy_done_schedule_before;
  };

  // Sort the prefetches by first the start time, then the effective done time.
  absl::c_sort(
      prefetches,
      [&](const std::pair<const MemorySpaceAssignment::CopyAllocation*, float>&
              a,
          const std::pair<const MemorySpaceAssignment::CopyAllocation*, float>&
              b) {
        return std::forward_as_tuple(
                   a.first->copy_start_schedule_after(),
                   get_effective_done_time(
                       a.first->copy_start_schedule_after(),
                       a.first->copy_done_schedule_before())) <
               std::forward_as_tuple(b.first->copy_start_schedule_after(),
                                     get_effective_done_time(
                                         b.first->copy_start_schedule_after(),
                                         b.first->copy_done_schedule_before()));
      });
  // Populate the required prefetch completions array. For each instruction in
  // the loop, this vector holds the index of the latest-issued prefetch that
  // needs to be completed before the instruction executes, or nullopt if there
  // is no prefetch that needs to finish by this instruction. To represent
  // prefetches that started in the previous iteration, we use negative numbers.
  std::vector<std::optional<int>> required_prefetch_completions(loop_size_);
  for (int i = 0; i < prefetches.size(); ++i) {
    const auto& [prefetch, elapsed] = prefetches[i];
    int required_prefetch_completion = i;
    if (prefetch->copy_start_schedule_after() == loop_size_ - 1 &&
        prefetch->copy_done_schedule_before() == 0) {
      required_prefetch_completion -= 2 * prefetches.size();
    } else if (prefetch->copy_start_schedule_after() + 1 >=
               prefetch->copy_done_schedule_before()) {
      required_prefetch_completion -= prefetches.size();
    }
    VLOG(3) << "Prefetch #" << i << " (elapsed " << elapsed
            << "): " << prefetch->ToString();
    if (required_prefetch_completions[prefetch->copy_done_schedule_before()]) {
      required_prefetch_completions[prefetch->copy_done_schedule_before()] =
          std::max(
              *required_prefetch_completions[prefetch
                                                 ->copy_done_schedule_before()],
              required_prefetch_completion);
    } else {
      required_prefetch_completions[prefetch->copy_done_schedule_before()] =
          required_prefetch_completion;
    }
    VLOG(4)
        << "Required completion at " << prefetch->copy_done_schedule_before()
        << " = "
        << *required_prefetch_completions[prefetch
                                              ->copy_done_schedule_before()];
  }

  // Populate the elapsed times of instructions and bandwidth idle times at each
  // point.
  float result;
  std::vector<float> bandwidth_idle_times;
  std::vector<float> instructions_elapsed;
  bandwidth_idle_times.reserve(loop_size_);
  instructions_elapsed.reserve(loop_size_);
  for (int i = 0; i < loop_size_; ++i) {
    bandwidth_idle_times.push_back(GetBandwidthIdleTime(i));
    instructions_elapsed.push_back(GetInstructionElapsed(i));
  }
  // We simulate the loop for three iterations to measure the steady state.
  const int kNumIterations = 3;
  // This data structure keeps track of the elapsed time remaining of each
  // prefetch. Note that there is a separate entry for each prefetch in each
  // iteration simulated.
  std::vector<float> prefetch_remaining_elapsed_times(prefetches.size() *
                                                      kNumIterations);
  int prefetch_start_index = 0;
  int prefetch_done_index = 0;
  int prefetch_completed_index = 0;

  for (int iteration = 0; iteration < kNumIterations; ++iteration) {
    float total_elapsed = 0;
    float total_bandwidth_idle_time = 0;
    float total_critical_prefetch = 0;
    for (int i = 0; i < loop_size_; ++i) {
      // If any prefetches are expected to be completed, check if they have any
      // remaining elapsed time associated with them, and if so add this to
      // critical prefetch time.
      std::optional<int> required_prefetch_completion =
          required_prefetch_completions[i];
      if (required_prefetch_completion) {
        int required_prefetch_done_index =
            iteration * static_cast<int>(prefetches.size()) +
            *required_prefetch_completion;
        VLOG(4) << "Prefetch #"
                << ((*required_prefetch_completion + prefetches.size()) %
                    prefetches.size())
                << " (" << required_prefetch_done_index
                << ") is required to be completed at " << i;
        for (; prefetch_done_index <= required_prefetch_done_index;
             ++prefetch_done_index) {
          CHECK_LE(prefetch_done_index, prefetch_start_index);
          if (prefetch_done_index == prefetch_completed_index) {
            float& prefetch_remaining =
                prefetch_remaining_elapsed_times[prefetch_done_index];
            VLOG(4) << "Prefetch #" << (prefetch_done_index % prefetches.size())
                    << " (" << prefetch_done_index
                    << ") did not complete, remaining elapsed = "
                    << prefetch_remaining;
            total_critical_prefetch += prefetch_remaining;
            prefetch_remaining = 0;
            ++prefetch_completed_index;
          }
        }
      }

      float elapsed = instructions_elapsed[i];
      total_elapsed += elapsed;
      float bandwidth_idle_time = bandwidth_idle_times[i];
      // Find the outstanding prefetches during this instruction, and if any of
      // them have remaining time, spend some or all of the bandwidth idle time
      // to satisfy them.
      for (; prefetch_completed_index < prefetch_start_index;
           ++prefetch_completed_index) {
        float& prefetch_remaining =
            prefetch_remaining_elapsed_times[prefetch_completed_index];
        if (bandwidth_idle_time < prefetch_remaining) {
          prefetch_remaining -= bandwidth_idle_time;
          bandwidth_idle_time = 0;
          VLOG(4) << "Prefetch #"
                  << (prefetch_completed_index % prefetches.size()) << " ("
                  << prefetch_completed_index << ") still ongoing at " << i
                  << ", remaining elapsed = " << prefetch_remaining;
          break;
        }
        bandwidth_idle_time -= prefetch_remaining;
        prefetch_remaining = 0;
        VLOG(4) << "Prefetch #"
                << (prefetch_completed_index % prefetches.size()) << " ("
                << prefetch_completed_index << ") completed at " << i
                << ", bandwidth idle time = " << bandwidth_idle_time;
      }
      if (bandwidth_idle_time > 0) {
        VLOG(4) << "Bandwidth idle time at " << i << " = "
                << bandwidth_idle_time;
        total_bandwidth_idle_time += bandwidth_idle_time;
      }

      // Start new prefetches that are scheduled to start after this
      // instruction.
      for (; prefetch_start_index < (iteration + 1) * prefetches.size() &&
             prefetches[prefetch_start_index % prefetches.size()]
                     .first->copy_start_schedule_after() == i;
           ++prefetch_start_index) {
        float& prefetch_remaining =
            prefetch_remaining_elapsed_times[prefetch_start_index];
        prefetch_remaining =
            prefetches[prefetch_start_index % prefetches.size()].second;
        VLOG(4) << "Prefetch #" << (prefetch_start_index % prefetches.size())
                << " (" << prefetch_start_index << ") started at " << i
                << ", remaining elapsed = " << prefetch_remaining;
      }
    }
    VLOG(3) << "Iteration " << iteration;
    VLOG(3) << "Total elapsed: " << total_elapsed
            << ", total critical prefetch: " << total_critical_prefetch
            << ", total bandwidth idle time: " << total_bandwidth_idle_time;
    result = total_elapsed + total_critical_prefetch;
  }
  return result;
}

/*static*/ std::string
MemoryBoundLoopOptimizer::LoopValue::AllocationTypeToString(
    LoopValue::AllocationType allocation_type) {
  switch (allocation_type) {
    case AllocationType::kTemporary:
      return "temporary";
    case AllocationType::kLoopCarriedDependence:
      return "loop-carried dependence";
    case AllocationType::kPinned:
      return "pinned";
    case AllocationType::kPrefetch:
      return "prefetch";
    default:
      CHECK(allocation_type == AllocationType::kUnsupported);
      return "unsupported";
  }
}

std::string MemoryBoundLoopOptimizer::LoopValue::ToString() const {
  std::string values_str;
  absl::StrAppend(&values_str, "Values:");
  for (const HloValue* hlo_value : hlo_values) {
    absl::StrAppend(&values_str, "\n  - ", hlo_value->ToShortString());
  }
  std::string allocations_str;
  if (!allocations.empty()) {
    absl::StrAppend(&allocations_str, "Allocations:");
  }
  for (const auto& allocation : allocations) {
    absl::StrAppend(&allocations_str, "\n  - ", allocation->ToString());
  }
  return absl::StrCat(
      "Size: ", size, " savings: ", savings,
      " savings per byte: ", savings_per_byte,
      " allocation type: ", AllocationTypeToString(allocation_type), "\n",
      values_str, "\n", allocations_str);
}

void MemoryBoundLoopOptimizer::SortLoopValues() {
  absl::c_stable_sort(loop_values_, [](const LoopValue& a, const LoopValue& b) {
    return a.savings_per_byte > b.savings_per_byte;
  });
}

void MemoryBoundLoopOptimizer::AllocateLoopValues() {
  // This function allocates loop values.
  std::vector<LoopValue*> prefetch_values;
  VLOG(3) << "Pre optimization execution time: " << CalculateExecutionTime();
  for (LoopValue& value : loop_values_) {
    switch (value.allocation_type) {
      case LoopValue::AllocationType::kTemporary:
        AllocateTemporary(value);
        break;
      case LoopValue::AllocationType::kPinned:
        AllocatePinned(value);
        break;
      case LoopValue::AllocationType::kPrefetch:
        prefetch_values.push_back(&value);
        break;
      case LoopValue::AllocationType::kLoopCarriedDependence:
      case LoopValue::AllocationType::kUnsupported:
        VLOG(1) << "Unsupported allocation: " << value.ToString();
    }
  }
  VLOG(3) << "Execution time after allocating temporaries: "
          << CalculateExecutionTime();
  AllocatePrefetches(absl::MakeSpan(prefetch_values));
  VLOG(3) << "Execution time after allocating prefetches:  "
          << CalculateExecutionTime();
}

void MemoryBoundLoopOptimizer::PostProcess() {
  // At the end, ensure that all loop uses have a corresponding Allocation and
  // create one in the default memory space if they don't.
  for (LoopValue& value : loop_values_) {
    absl::flat_hash_set<HloUse> allocated_uses;
    for (const auto& allocation : value.allocations) {
      for (const HloUse& use : allocation->uses()) {
        allocated_uses.insert(use);
      }
    }
    std::vector<HloUse> unallocated_uses;
    absl::flat_hash_set<int> use_indices;
    for (const auto& [idx, use] : value.loop_uses) {
      use_indices.insert(idx);
      if (!allocated_uses.contains(use)) {
        unallocated_uses.push_back(use);
      }
    }
    for (const auto& [next_iteration_idx, use] : value.next_iteration_uses) {
      if (use_indices.contains(next_iteration_idx)) {
        continue;
      }
      HloInstruction* loop_instruction =
          hlo_live_range_.flattened_instruction_sequence().instructions().at(
              loop_start_ + next_iteration_idx);
      HloUse loop_use{loop_instruction, use.operand_number, use.operand_index};
      if (!allocated_uses.contains(loop_use)) {
        unallocated_uses.push_back(loop_use);
      }
    }
    if (!unallocated_uses.empty()) {
      // TODO(b/281582241): We should find the correct position. For now, we're
      // using the defining position on the first HLO value.
      value.allocations.push_back(
          std::make_unique<MemorySpaceAssignment::Allocation>(
              value.hlo_values.front()->defining_position(),
              MemorySpaceAssignment::MemorySpace::kDefault, std::nullopt, 0,
              loop_size_, /*is_scoped_allocation=*/false));
      for (const HloUse& use : unallocated_uses) {
        value.allocations.back()->AddUse(use);
      }
    }
  }
}

bool MemoryBoundLoopOptimizer::AllocateBetween(int64_t begin_idx,
                                               int64_t end_idx, int64_t size) {
  int64_t end_idx_sentinel = end_idx;
  if (end_idx < begin_idx) {
    end_idx_sentinel += loop_size_;
  }
  for (int64_t i = begin_idx; i <= end_idx_sentinel; ++i) {
    if (remaining_memory_[i % loop_size_] < size) {
      return false;
    }
  }
  for (int64_t i = begin_idx; i <= end_idx_sentinel; ++i) {
    remaining_memory_[i % loop_size_] -= size;
  }
  return true;
}

bool MemoryBoundLoopOptimizer::AllocateTemporary(LoopValue& value) {
  VLOG(3) << "AllocateTemporary: " << value.ToString();
  if (value.hlo_values.size() > 1) {
    VLOG(3) << "LoopValue has more than one hlo value associated.";
    return false;
  }
  int64_t definition_idx = value.loop_positions.front().first;
  int64_t max_use_idx;
  if (!value.next_iteration_uses.empty()) {
    max_use_idx = value.next_iteration_uses.back().first;
    // If max_use_idx >= definition_idx, then this is a loop carried dependence
    // and we should not have called this function.
    CHECK_LT(max_use_idx, definition_idx);
  } else {
    max_use_idx = value.loop_uses.back().first;
  }
  bool success = AllocateBetween(definition_idx, max_use_idx, value.size);
  if (success) {
    VLOG(3) << "Pos: " << value.loop_positions[0].second;
    value.allocations.push_back(
        std::make_unique<MemorySpaceAssignment::Allocation>(
            value.loop_positions[0].second,
            MemorySpaceAssignment::MemorySpace::kAlternate, std::nullopt,
            definition_idx, max_use_idx,
            /*is_scoped_allocation=*/false));
    AddAllLoopPositionsAndUses(value, /*allocate_next_iteration_uses=*/true);
  }
  return success;
}

bool MemoryBoundLoopOptimizer::AllocatePinned(LoopValue& value) {
  bool success = AllocateBetween(0, loop_size_, value.size);
  if (success) {
    CHECK(value.header_position);
    value.allocations.push_back(
        std::make_unique<MemorySpaceAssignment::Allocation>(
            *value.header_position,
            MemorySpaceAssignment::MemorySpace::kAlternate, std::nullopt, 0,
            loop_size_,
            /*is_scoped_allocation=*/false));
    AddAllLoopPositionsAndUses(value, /*allocate_next_iteration_uses=*/false);
  }
  return success;
}

bool MemoryBoundLoopOptimizer::AllocatePrefetches(
    absl::Span<LoopValue*> values) {
  VLOG(3) << "Allocating prefetches num values: " << values.size();
  AllocatePrefetchesContext context;
  context.values = values;
  // Populate value_indices, which is a list of indices into values array sorted
  // by the start time of the first use.
  context.value_indices.resize(values.size());
  absl::c_iota(context.value_indices, 0);
  absl::c_stable_sort(context.value_indices, [&](int a, int b) {
    return std::forward_as_tuple(
               values[a]->loop_uses.begin()->first,
               values[a]->loop_uses.begin()->second.operand_number) >
           std::forward_as_tuple(
               values[b]->loop_uses.begin()->first,
               values[b]->loop_uses.begin()->second.operand_number);
  });

  // Populate the data structures that contain additional positions and uses
  // that would get alternate memory allocations if all of the prefetches were
  // successful.
  absl::flat_hash_map<const HloInstruction*,
                      std::vector<std::pair<int64_t, ShapeIndex>>>
      additional_uses_in_alternate_mem;
  absl::flat_hash_map<const HloInstruction*, std::vector<ShapeIndex>>
      additional_positions_in_alternate_mem;
  for (const LoopValue* value : values) {
    VLOG(3) << "  prefetch value: " << value->ToString();
    for (const auto& [idx, use] : value->loop_uses) {
      additional_uses_in_alternate_mem[use.instruction].push_back(
          {use.operand_number, use.operand_index});
    }
    for (const auto& [idx, position] : value->loop_positions) {
      additional_positions_in_alternate_mem[position.instruction].push_back(
          position.index);
    }
  }
  // Calculate the default-memory remaining bandwidths assuming all prefetches
  // succeed.
  for (int i = 0; i < loop_size_; ++i) {
    context.bandwidth_idle_times.push_back(
        GetBandwidthIdleTime(i, additional_uses_in_alternate_mem,
                             additional_positions_in_alternate_mem));
    VLOG(3) << "Remaining bandwidth at " << i << " = "
            << *context.bandwidth_idle_times.rbegin();
  }

  context.additional_memory_used.resize(loop_size_, 0);

  // Allocate prefetches by traversing the loop values in reverse order of
  // the first uses.
  for (int value_index : context.value_indices) {
    AllocatePrefetch(value_index, context);
  }

  for (int i = 0; i < loop_size_; ++i) {
    remaining_memory_[i] -= context.additional_memory_used[i];
    VLOG(3) << "Additional memory [" << i
            << "]: " << context.additional_memory_used[i];
    VLOG(3) << "Remaining memory [" << i << "]: " << remaining_memory_[i];
    VLOG(3) << "Remaining bandwidth [" << i
            << "] : " << context.bandwidth_idle_times[i];
  }
  return true;
}

bool MemoryBoundLoopOptimizer::AllocatePrefetch(
    int value_index, AllocatePrefetchesContext& context) {
  LoopValue* value = context.values.at(value_index);
  VLOG(3) << "Allocating value: " << value->ToString();
  int first_use_idx = value->loop_uses.front().first;
  int last_use_idx = value->loop_uses.back().first;
  int last_use_idx_sentinel = last_use_idx;
  if (!value->next_iteration_uses.empty()) {
    last_use_idx = value->next_iteration_uses.back().first;
    last_use_idx_sentinel = last_use_idx + loop_size_;
    CHECK_LT(last_use_idx, first_use_idx);
  }
  bool out_of_memory = false;
  for (int i = first_use_idx; i <= last_use_idx_sentinel; ++i) {
    int loop_idx = i % loop_size_;
    if (context.additional_memory_used[loop_idx] + value->size >
        remaining_memory_[loop_idx]) {
      VLOG(3) << "Ran out of memory allocating for uses.";
      out_of_memory = true;
    }
  }
  if (out_of_memory) {
    return false;
  }
  float copy_resource =
      cost_analysis_.GetAsyncCopyElapsed(value->hlo_values.front()->shape());
  VLOG(3) << "First use: " << value->loop_uses.begin()->second
          << " use idx: " << first_use_idx
          << " copy resource: " << copy_resource;
  std::optional<int> copy_start_time;
  // The general allocation algorithm for prefetches is to first calculate the
  // default-memory bandwidth idle times at each point (assuming all prefetches
  // succeeded).  We show this pictorially below. We also show the previous
  // iteration for clarity. The algorithm solves allocation for one iteration
  // and this will be used for all iterations.
  //
  //               idx:  0  1  2  3  4  5| 0  1  2  3  4  5|
  //      bw idle time:  2  2  1  2  3  1| 2  2  1  2  3  1|
  // additional memory:  0  0  0  0  0  0| 0  0  0  0  0  0|
  //         iteration:       prev       |      current    |
  //
  // Now, let's assume there are two prefetches that need to be scheduled. For
  // the sake of the example, assume 1 MiB of prefetch uses 1 memory bandwidth
  // resource:
  //   - Prefetch 1 is 4 MiB and is first used at index 5.
  //   - Prefetch 2 is 5 MiB and is first used at index 1.
  //
  // We first order these prefetches by their first use from latest to earliest.
  // Then starting from the prefetch completion time (i.e. the first use time),
  // move the prefetch start time earlier until the copy resource is satisfied
  // (or reaching another resource satisfaction criteria explained below) by
  // consuming the bandwidth idle time of the overlapped instructions. We also
  // keep track of the additional memory required. Note that index 5 also
  // accounts for the additional 4 MiB consumed since the data needs to reside
  // during the execution of the instruction at index 5.  Below is the updated
  // state after scheduling prefetch 1:
  //
  //        prefetch 1:          +====+            +====+
  //               idx:  0  1  2  3  4  5| 0  1  2  3  4  5|
  //      bw idle time:  2  2  1  1  0  1| 2  2  1  1  0  1|
  // additional memory:  0  0  0  4  4  4| 0  0  0  4  4  4|
  //         iteration:       prev       |      current    |
  //
  // To schedule prefetch 2, we similarly start the same way, from its first use
  // and bring the prefetch start earlier. We first reach index 0 with still an
  // unsatisfied copy resource of 3:
  //
  //        prefetch 2: +=+               +=+                unsat res: 3
  //        prefetch 1:          +====+            +====+
  //               idx:  0  1  2  3  4  5| 0  1  2  3  4  5|
  //      bw idle time:  0  2  1  1  0  1| 0  2  1  1  0  1|
  // additional memory:  5  5  0  4  4  4| 5  5  0  4  4  4|
  //         iteration:       prev       |      current    |
  //
  // We continue onto the previous iteration:
  //
  //        prefetch 2:===+            +====+            +== unsat res: 2
  //        prefetch 1:          +====+            +====+
  //               idx:  0  1  2  3  4  5| 0  1  2  3  4  5|
  //      bw idle time:  0  2  1  1  0  0| 0  2  1  1  0  0|
  // additional memory:  5  5  0  4  4  9| 5  5  0  4  4  9|
  //         iteration:       prev       |      current    |
  //
  // As we bring the start time of prefetch 2 earlier, it starts overlapping
  // with prefetch 1:
  //
  //        prefetch 2:===+      +==========+      +======== unsat res: 1
  //        prefetch 1:          +====+            +====+
  //               idx:  0  1  2  3  4  5| 0  1  2  3  4  5|
  //      bw idle time:  0  2  1  0  0  0| 0  2  1  0  0  0|
  // additional memory:  5  5  0  9  9  9| 5  5  0  9  9  9|
  //         iteration:       prev       |      current    |
  //
  // The prefetch resource is still unsatisfied at this point. We can bring the
  // prefetch earlier. However, the first prefetch's end time is earlier than
  // the second and we need to maintain FIFO order with regard to prefetches. In
  // order to maintain this FIFO order, we "early force" prefetches that are
  // already scheduled by moving the start time earlier along with prefetch 2:
  //
  //        prefetch 2:===+   +=============+   +===========
  //        prefetch 1:       +=======+         +=======+
  //               idx:  0  1  2  3  4  5| 0  1  2  3  4  5|
  //      bw idle time:  0  2  0  0  0  0| 0  2  0  0  0  0|
  // additional memory:  5  5  9  9  9  9| 5  5  9  9  9  9|
  //         iteration:       prev       |      current    |
  //
  // Depending on the options provided, we can use alternative resource
  // satisfaction criteria. One option is to specify a percentage of the copy
  // resource that needs to be satisfied instead of the complete amount (100%).
  // This is called the "desired copy ratio". The reason why desired copy ratio
  // can be less than 100% is that in a memory-bound loop, we probably do not
  // have enough aggregate bandwidth resources to satisfy all of the prefetches,
  // but using up all of the default-memory bandwidth is more important than
  // having some prefetches with unsatisfied resources. In a similar vein,
  // another option is to accept prefetches that are fully pipelined, i.e.
  // their copy start time is scheduled the same time as the copy done time in
  // the previous iteration, regardless of how much of its copy resources are
  // actually satisfied. To illustrate a fully pipelined prefetch, consider
  // prefetch 3 (assume no prefetch 1 or 2 in this example) which is 15 MiB and
  // its first use is at index 4:
  //
  //        prefetch 3:=============+=================+===== unsat res: 4
  //               idx:  0  1  2  3  4  5| 0  1  2  3  4  5|
  //      bw idle time:  0  0  0  0  0  0| 0  0  0  0  0  0|
  // additional memory: 15 15 15 15 30 15|15 15 15 15 30 15|
  //         iteration:       prev       |      current    |
  //
  // Note that the additional memory consumption at index 4 is actually twice
  // the size of the prefetch as we are effectively double buffering. Also note
  // that the prefetch has an unsatisfied copy resource of 4 meaning the copy
  // will be in the critical path, but this actually will be faster than not
  // scheduling this particular prefetch in the first place since the bandwidth
  // idle time resource would go unused.
  float accumulated_copy_resource = 0;
  std::vector<int> early_forced_prefetch_value_indices;
  int early_forced_prefetch_value_search_index = 0;
  float early_forced_prefetch_additional_memory = 0;
  for (int i = first_use_idx - 1; i >= last_use_idx_sentinel - loop_size_;
       --i) {
    int loop_idx = (i + loop_size_) % loop_size_;
    // Check if this prefetch rolls over to the previous iteration, check if any
    // already-scheduled prefetches would violate the FIFO order, and if so,
    // "early-force" them to be co-scheduled with this prefetch to maintain the
    // FIFO order. This of course increases the required memory, so also keep
    // track of additional memory that would be consumed.
    if (i < 0) {
      for (; context.value_indices[early_forced_prefetch_value_search_index] !=
             value_index;
           ++early_forced_prefetch_value_search_index) {
        VLOG(3) << "Searching for early forced: "
                << early_forced_prefetch_value_search_index;
        LoopValue* early_forced_value = context.values.at(
            context.value_indices[early_forced_prefetch_value_search_index]);
        if (early_forced_value->allocations.empty()) {
          continue;
        }
        const MemorySpaceAssignment::CopyAllocation* early_forced_prefetch =
            static_cast<const MemorySpaceAssignment::CopyAllocation*>(
                early_forced_value->allocations.back().get());
        VLOG(3) << "Prefetch: " << early_forced_prefetch->ToString();

        // If the prefetch is already a roll-around prefetch, no need to further
        // early force it.
        if (early_forced_prefetch->copy_done_schedule_before() <=
                early_forced_prefetch->copy_start_schedule_after() + 1 ||
            (early_forced_prefetch->copy_start_schedule_after() ==
                 loop_size_ - 1 &&
             early_forced_prefetch->copy_done_schedule_before() == 0)) {
          break;
        }
        if (early_forced_prefetch->copy_start_schedule_after() != loop_idx) {
          break;
        }
        early_forced_prefetch_value_indices.push_back(
            early_forced_prefetch_value_search_index);
        early_forced_prefetch_additional_memory += early_forced_value->size;
        VLOG(3) << "Found early-forced prefetch value: "
                << early_forced_value->ToString();
        VLOG(3) << "Early forced prefetch additional memory: "
                << early_forced_prefetch_additional_memory;
      }
    }

    // Overlap memory overhead only happens if the copy start overlaps with the
    // first use (i.e. fully pipelined), so we'd need to account for 2X the
    // buffer at this time.
    int64_t overlap_memory_overhead = 0;
    if (loop_idx == last_use_idx) {
      overlap_memory_overhead = value->size;
      VLOG(3) << "Loop idx == last use idx (" << loop_idx
              << "), overlap memory overhead = " << overlap_memory_overhead;
    }

    // OOM; give up prefetch.
    if (context.additional_memory_used[loop_idx] + value->size +
            overlap_memory_overhead + early_forced_prefetch_additional_memory >
        remaining_memory_[loop_idx]) {
      VLOG(3) << "Ran out of memory. Accumulated copy resource "
              << accumulated_copy_resource << " out of " << copy_resource
              << " at " << loop_idx;
      break;
    }

    // We ideally find a time to overlap the prefetch fully where the previous
    // iteration's memory use is disjoint from this iteration. If that is not
    // possible, there are two compromises we could pick:
    //   - Find a prefetch time that satisfies a desired ratio < 1 of the
    //      prefetch elapsed time. This means the prefetch will be critical.
    //   - Overlap the prefetch with the previous iteration's buffer use, i.e.
    //     full pipelining. This would increase the peak memory consumption.
    float bandwidth_idle_time = context.bandwidth_idle_times[loop_idx];
    VLOG(3) << "Idx " << loop_idx
            << " bandwidth_idle_time: " << bandwidth_idle_time
            << " copy resource remaining: "
            << (copy_resource - accumulated_copy_resource) << " diff: "
            << (bandwidth_idle_time -
                (copy_resource - accumulated_copy_resource));
    if (bandwidth_idle_time >= copy_resource - accumulated_copy_resource) {
      accumulated_copy_resource = copy_resource;
      copy_start_time = loop_idx;
      VLOG(3) << "Found the complete copy ratio and updated accumulated copy "
                 "resource: "
              << accumulated_copy_resource;
      break;
    } else if (!copy_start_time &&
               accumulated_copy_resource + bandwidth_idle_time >=
                   copy_resource * options_.desired_copy_ratio()) {
      accumulated_copy_resource += bandwidth_idle_time;
      copy_start_time = loop_idx;
      VLOG(3) << "Found the desired copy ratio and updated accumulated copy "
                 "resource: "
              << accumulated_copy_resource;
    } else if (options_.allow_unsatisfied_fully_pipelined_prefetch() &&
               loop_idx == last_use_idx) {
      // Even if desired resource isn't reached, and if the options allow it,
      // allow a fully pipelined prefetch.
      accumulated_copy_resource += bandwidth_idle_time;
      copy_start_time = loop_idx;
      VLOG(3) << "Could not reach the desired copy ratio but scheduling "
                 "fully pipelined prefetch anyway: "
              << accumulated_copy_resource;
      break;
    } else {
      accumulated_copy_resource += bandwidth_idle_time;
      VLOG(3) << "Updated accumulated copy resource: "
              << accumulated_copy_resource;
    }
  }

  // Could not find a suitable copy start time.
  if (!copy_start_time) {
    return false;
  }

  VLOG(3) << "Success: copy_start_time: " << *copy_start_time
          << " leftover copy resource: "
          << (copy_resource - accumulated_copy_resource);
  auto update_additional_memory_used = [&](int loop_idx, int64_t addition) {
    VLOG(4) << "Updating additional memory used at " << loop_idx << ". "
            << context.additional_memory_used[loop_idx] << " + " << addition
            << " => " << (context.additional_memory_used[loop_idx] + addition)
            << " (remaining: " << remaining_memory_[loop_idx] << ")";
    context.additional_memory_used[loop_idx] += addition;
    CHECK_LE(context.additional_memory_used[loop_idx],
             remaining_memory_[loop_idx]);
  };
  for (int i = first_use_idx; i <= last_use_idx_sentinel; ++i) {
    int loop_idx = i % loop_size_;
    update_additional_memory_used(loop_idx, value->size);
  }
  for (int i = first_use_idx - 1; i >= last_use_idx_sentinel - loop_size_;
       --i) {
    int loop_idx = (i + loop_size_) % loop_size_;
    float& bandwidth_idle_time = context.bandwidth_idle_times[loop_idx];
    // Overlap memory overhead only happens if the copy start overlaps with the
    // first use (i.e. fully pipelined), so we'd need to account for 2X the
    // buffer at this time.
    int64_t overlap_memory_overhead = 0;
    update_additional_memory_used(loop_idx,
                                  value->size + overlap_memory_overhead);
    if (bandwidth_idle_time < copy_resource) {
      copy_resource -= bandwidth_idle_time;
      bandwidth_idle_time = 0;
      if (loop_idx == *copy_start_time) {
        VLOG(3) << "Remaining copy resource: " << copy_resource;
        break;
      }
    } else {
      bandwidth_idle_time -= copy_resource;
      copy_resource = 0;
      CHECK_EQ(loop_idx, *copy_start_time);
      break;
    }
  }

  // Create the Allocation objects that correspond to the scheduled prefetch.
  CHECK(value->header_position);
  value->allocations.push_back(
      std::make_unique<MemorySpaceAssignment::Allocation>(
          *value->header_position, MemorySpaceAssignment::MemorySpace::kDefault,
          std::nullopt, 0, loop_size_, /*is_scoped_allocation=*/false));
  value->allocations.push_back(
      std::make_unique<MemorySpaceAssignment::CopyAllocation>(
          *value->allocations.back(),
          MemorySpaceAssignment::MemorySpace::kAlternate, std::nullopt,
          ((*copy_start_time - 1) + loop_size_) % loop_size_,
          last_use_idx_sentinel, first_use_idx));
  AddAllLoopPositionsAndUses(*value, /*allocate_next_iteration_uses=*/true);

  // Account for the additional memory used by early forcing the already
  // scheduled prefetches. Also modify the start times of these to this
  // prefetch's copy start time.
  for (int early_forced_prefetch_value_index :
       early_forced_prefetch_value_indices) {
    LoopValue* early_forced_value = context.values.at(
        context.value_indices[early_forced_prefetch_value_index]);
    CHECK(!early_forced_value->allocations.empty());
    MemorySpaceAssignment::CopyAllocation* early_forced_prefetch =
        static_cast<MemorySpaceAssignment::CopyAllocation*>(
            early_forced_value->allocations.back().get());
    for (int index = early_forced_prefetch->copy_start_schedule_after();
         index >= *copy_start_time; --index) {
      update_additional_memory_used(index, early_forced_value->size);
      VLOG(3) << "Additional memory used: " << index << " "
              << context.additional_memory_used[index];
    }
    early_forced_prefetch->set_copy_start_schedule_after(
        ((*copy_start_time - 1) + loop_size_) % loop_size_);
    VLOG(3) << "Updated prefetch: " << early_forced_prefetch->ToString();
  }
  return true;
}

void MemoryBoundLoopOptimizer::AddAllLoopPositionsAndUses(
    LoopValue& value, bool allocate_next_iteration_uses) {
  CHECK_GE(value.allocations.size(), 1);
  MemorySpaceAssignment::Allocation& allocation = *value.allocations.back();
  for (const auto& [idx, position] : value.loop_positions) {
    positions_in_alternate_mem_[position.instruction].push_back(position.index);
  }
  for (const auto& [idx, use] : value.loop_uses) {
    uses_in_alternate_mem_[use.instruction].push_back(
        {use.operand_number, use.operand_index});
    allocation.AddUse(use);
  }
  if (allocate_next_iteration_uses) {
    for (const auto& [next_iteration_idx, use] : value.next_iteration_uses) {
      HloInstruction* loop_instruction =
          hlo_live_range_.flattened_instruction_sequence().instructions().at(
              loop_start_ + next_iteration_idx);
      uses_in_alternate_mem_[loop_instruction].push_back(
          {use.operand_number, use.operand_index});
      allocation.AddUse(
          {loop_instruction, use.operand_number, use.operand_index});
    }
  }
}

float MemoryBoundLoopOptimizer::GetBandwidthIdleTime(int idx) const {
  const HloInstruction* inst =
      hlo_live_range_.flattened_instruction_sequence().instructions().at(
          loop_start_ + idx);
  std::vector<std::pair<int64_t, ShapeIndex>> empty_operands;
  std::vector<ShapeIndex> empty_outputs;
  const std::vector<std::pair<int64_t, ShapeIndex>>* operands_in_alternate_mem =
      &empty_operands;
  const std::vector<ShapeIndex>* outputs_in_alternate_mem = &empty_outputs;
  auto uses_it = uses_in_alternate_mem_.find(inst);
  if (uses_it != uses_in_alternate_mem_.end()) {
    operands_in_alternate_mem = &uses_it->second;
  }
  auto positions_it = positions_in_alternate_mem_.find(inst);
  if (positions_it != positions_in_alternate_mem_.end()) {
    outputs_in_alternate_mem = &positions_it->second;
  }
  return cost_analysis_.GetDefaultMemoryBandwidthIdleTime(
      *inst, *operands_in_alternate_mem, *outputs_in_alternate_mem);
}

float MemoryBoundLoopOptimizer::GetBandwidthIdleTime(
    int idx,
    const absl::flat_hash_map<const HloInstruction*,
                              std::vector<std::pair<int64_t, ShapeIndex>>>&
        additional_uses_in_alternate_mem,
    const absl::flat_hash_map<const HloInstruction*, std::vector<ShapeIndex>>&
        additional_positions_in_alternate_mem) const {
  const HloInstruction* inst =
      hlo_live_range_.flattened_instruction_sequence().instructions().at(
          loop_start_ + idx);
  std::vector<std::pair<int64_t, ShapeIndex>> operands_in_alternate_mem;
  std::vector<ShapeIndex> outputs_in_alternate_mem;
  auto uses_it = uses_in_alternate_mem_.find(inst);
  if (uses_it != uses_in_alternate_mem_.end()) {
    operands_in_alternate_mem = uses_it->second;
  }
  auto additional_uses_it = additional_uses_in_alternate_mem.find(inst);
  if (additional_uses_it != additional_uses_in_alternate_mem.end()) {
    absl::c_copy(additional_uses_it->second,
                 std::back_inserter(operands_in_alternate_mem));
  }
  auto positions_it = positions_in_alternate_mem_.find(inst);
  if (positions_it != positions_in_alternate_mem_.end()) {
    outputs_in_alternate_mem = positions_it->second;
  }
  auto additional_positions_it =
      additional_positions_in_alternate_mem.find(inst);
  if (additional_positions_it != additional_positions_in_alternate_mem.end()) {
    absl::c_copy(additional_positions_it->second,
                 std::back_inserter(outputs_in_alternate_mem));
  }
  return cost_analysis_.GetDefaultMemoryBandwidthIdleTime(
      *inst, operands_in_alternate_mem, outputs_in_alternate_mem);
}

float MemoryBoundLoopOptimizer::GetInstructionElapsed(int idx) const {
  const HloInstruction* inst =
      hlo_live_range_.flattened_instruction_sequence().instructions().at(
          loop_start_ + idx);
  std::vector<std::pair<int64_t, ShapeIndex>> empty_operands;
  std::vector<ShapeIndex> empty_outputs;
  const std::vector<std::pair<int64_t, ShapeIndex>>* operands_in_alternate_mem =
      &empty_operands;
  const std::vector<ShapeIndex>* outputs_in_alternate_mem = &empty_outputs;
  auto uses_it = uses_in_alternate_mem_.find(inst);
  if (uses_it != uses_in_alternate_mem_.end()) {
    operands_in_alternate_mem = &uses_it->second;
  }
  auto positions_it = positions_in_alternate_mem_.find(inst);
  if (positions_it != positions_in_alternate_mem_.end()) {
    outputs_in_alternate_mem = &positions_it->second;
  }
  return cost_analysis_.GetInstructionElapsedInAlternateMemory(
      *inst, *operands_in_alternate_mem, *outputs_in_alternate_mem);
}

Status AlternateMemoryBestFitHeap::OptimizeMemoryBoundLoop(int loop_start_idx,
                                                           int loop_end_idx,
                                                           int loop_size) {
  // The MemoryBoundLoopOptimizer works with a minimum of three unrolled loop
  // iterations: previous, current, and next. So, we pick the second iteration
  // out of the loop as the current iteration.
  const int iteration_start_idx = loop_start_idx + loop_size;
  const int iteration_end_idx = iteration_start_idx + loop_size;

  TF_ASSIGN_OR_RETURN(
      std::unique_ptr<MemoryBoundLoopOptimizer> optimizer,
      MemoryBoundLoopOptimizer::Create(
          iteration_start_idx, iteration_end_idx, options_.max_size_in_bytes,
          options_.memory_bound_loop_optimizer_options, hlo_live_range_,
          alias_analysis_, *options_.cost_analysis, options_.size_fn));
  optimizer->Optimize();

  const int loop_optimized_allocations_original_size =
      loop_optimized_allocations_.size();
  for (MemoryBoundLoopOptimizer::LoopValue& value : optimizer->loop_values()) {
    if (!value.allocations.empty()) {
      loop_optimized_allocations_.push_back(std::move(value.allocations));
    }
  }

  // Check if this unrolled loop is in a while loop.
  const auto& instruction_sequence =
      hlo_live_range_.flattened_instruction_sequence().instructions();
  std::vector<HloInstruction*> callers = call_graph_->GetComputationCallers(
      instruction_sequence[loop_start_idx]->parent());
  const bool is_in_while_loop =
      callers.size() == 1 && callers.front()->opcode() == HloOpcode::kWhile;

  // Update the loop_optimized_allocations_map_ with the output of the
  // optimizer.
  for (int i = loop_optimized_allocations_original_size;
       i < loop_optimized_allocations_.size(); ++i) {
    const MemorySpaceAssignment::AllocationSequence& sequence =
        loop_optimized_allocations_.at(i);
    CHECK(!sequence.empty());
    VLOG(3) << "  alloc: " << sequence.back()->ToString();
    for (const auto& allocation : sequence) {
      // Check if the loop is in a while loop and the position needs to be
      // allocated in the default memory.
      const bool require_pos_in_default_space =
          is_in_while_loop &&
          (allocation->memory_space() == MemorySpace::kDefault ||
           allocation->is_copy_allocation());
      for (const HloUse& use : allocation->uses()) {
        const int64_t use_idx =
            hlo_live_range_.instruction_schedule().at(use.instruction) -
            iteration_start_idx;
        CHECK_GE(use_idx, 0);
        CHECK_LT(use_idx, loop_size);
        for (int64_t i = loop_start_idx + use_idx; i <= loop_end_idx;
             i += loop_size) {
          HloInstruction* repeated_inst = instruction_sequence[i];
          HloUse repeated_use{repeated_inst, use.operand_number,
                              use.operand_index};
          loop_optimized_allocations_map_[repeated_use] = {use_idx, loop_size,
                                                           allocation.get()};
          VLOG(3) << " Setting optimized allocations map. Use: "
                  << repeated_use.ToString() << " idx: " << use_idx
                  << " allocation: " << allocation->ToString();
          if (require_pos_in_default_space) {
            const HloValue& value =
                alias_analysis_.dataflow_analysis().GetUniqueValueAt(
                    repeated_inst->operand(use.operand_number),
                    use.operand_index);
            // If any of the positions is a parameter in a while loop, we add a
            // required assignment in the default memory space.
            for (const HloPosition& value_position : value.positions()) {
              if (value_position.instruction->parent() ==
                      repeated_inst->parent() &&
                  value_position.instruction->opcode() ==
                      HloOpcode::kParameter) {
                AddRequiredAssignment(value_position.instruction,
                                      value_position.index,
                                      MemorySpace::kDefault);
                break;
              }
            }
          }
        }
      }
    }
  }
  return OkStatus();
}

namespace {
// A helper function to get the distance between a use and its producer (or -1
// if producer is a gte, parameter or tuple).
std::function<int(const HloInstruction*)> GetOperandDistanceFunction(
    const HloLiveRange& hlo_live_range, const HloInstruction* use_inst) {
  const int use_idx = hlo_live_range.instruction_schedule().at(use_inst);
  return [&, use_idx](const HloInstruction* operand) -> int {
    // We just use -1 for parameter, tuple, and gte instructions. We could make
    // this "see through" the gtes if we get too many false positives.
    if (operand->opcode() == HloOpcode::kParameter ||
        operand->opcode() == HloOpcode::kTuple ||
        operand->opcode() == HloOpcode::kGetTupleElement) {
      return -1;
    }
    return use_idx - hlo_live_range.instruction_schedule().at(operand);
  };
}

// A helper function to check if the operand distances of two instructions
// are compatible. This assumes `a` is scheduled loop size candidate
// instructions before `b`. The operand distances are compatible if either
// distance is -1, or if they are the same, or if they are separated by loop
// size candidate.
bool AreOperandCandidatesCompatible(int loop_size_candidate,
                                    absl::Span<const int> a_distances,
                                    absl::Span<const int> b_distances) {
  if (a_distances.size() != b_distances.size()) {
    return false;
  }
  for (int i = 0; i < a_distances.size(); ++i) {
    const int a_value = a_distances.at(i);
    const int b_value = b_distances.at(i);
    if (a_value != -1 && b_value != -1 &&
        a_value + loop_size_candidate != b_value && a_value != b_value) {
      return false;
    }
  }
  return true;
}
}  // namespace

void AlternateMemoryBestFitHeap::IdentifyAndOptimizeMemoryBoundLoops() {
  absl::flat_hash_map<absl::string_view, int> fingerprint_schedule_map;
  const auto& instruction_sequence =
      hlo_live_range_.flattened_instruction_sequence().instructions();
  // The minimum and maximum loop sizes that we consider.
  const int kMinLoopSize = 4;
  const int kMaxLoopSize = 400;
  const float kMinNumIterations = 3.0;
  int optimized_loop_idx = 0;
  while (optimized_loop_idx < instruction_sequence.size()) {
    // Iterate over the flattened instruction sequence. We first try to find a
    // loop candidate where the fingerprint between two instructions matches by
    // the loop size candidate.
    int loop_size_candidate = -1;
    int loop_start_idx = -1;
    int loop_end_idx = -1;
    for (; optimized_loop_idx < instruction_sequence.size();
         ++optimized_loop_idx) {
      const HloInstruction* inst = instruction_sequence[optimized_loop_idx];
      auto fingerprint_it = fingerprint_map_.find(inst);
      if (inst->opcode() != HloOpcode::kParameter &&
          inst->opcode() != HloOpcode::kTuple &&
          inst->opcode() != HloOpcode::kGetTupleElement &&
          fingerprint_it != fingerprint_map_.end()) {
        // Find and the latest instruction with the same fingerprint as this.
        auto fingerprint_schedule_it =
            fingerprint_schedule_map.find(fingerprint_it->second);
        if (fingerprint_schedule_it != fingerprint_schedule_map.end()) {
          int distance = optimized_loop_idx - fingerprint_schedule_it->second;
          if (distance >= kMinLoopSize && distance <= kMaxLoopSize) {
            // We found two instructions with the same fingerprint. The distance
            // between the two is the loop size candidate.
            loop_size_candidate = distance;
            break;
          }
        }
        fingerprint_schedule_map[fingerprint_it->second] = optimized_loop_idx;
      }

      VLOG(3) << " " << optimized_loop_idx << ": "
              << instruction_sequence[optimized_loop_idx]->parent()->name()
              << " " << instruction_sequence[optimized_loop_idx]->name()
              << " fingerprint: "
              << (fingerprint_it == fingerprint_map_.end()
                      ? "none"
                      : fingerprint_it->second);
    }
    VLOG(3) << "Loop size candidate: " << loop_size_candidate;
    if (loop_size_candidate == -1) {
      break;
    }

    std::vector<std::vector<int>> operand_distances;

    // Scan the instructions with the candidate loop size. We try to calculate
    // the size of the loop by finding the instructions that are loop size
    // candidate apart, have the same fingerprint and compatible operand
    // distances. We start scanning the candidate loop a few instructions
    // earlier than the fingerprint identified in case the loop starts a bit
    // earlier than the fingerprint logic.
    const int kLoopScanHeadStart = 10;
    for (int i = std::max(
             0, optimized_loop_idx - loop_size_candidate - kLoopScanHeadStart);
         i < instruction_sequence.size(); ++i) {
      const HloInstruction* inst = instruction_sequence[i];
      auto fingerprint_it = fingerprint_map_.find(inst);
      auto ignore_op = [](const HloInstruction* instruction) {
        return instruction->opcode() == HloOpcode::kParameter ||
               instruction->opcode() == HloOpcode::kTuple ||
               instruction->opcode() == HloOpcode::kGetTupleElement;
      };
      if (loop_start_idx == -1) {
        if (i > optimized_loop_idx - loop_size_candidate) {
          break;
        }
        if (ignore_op(inst) || fingerprint_it == fingerprint_map_.end()) {
          continue;
        }
        if (i + loop_size_candidate >= instruction_sequence.size()) {
          break;
        }
        const HloInstruction* candidate_inst =
            instruction_sequence[i + loop_size_candidate];
        auto candidate_fingerprint_it = fingerprint_map_.find(candidate_inst);
        if (ignore_op(candidate_inst) ||
            candidate_fingerprint_it == fingerprint_map_.end() ||
            fingerprint_it->second != candidate_fingerprint_it->second) {
          // Fingerprint mismatch.
          continue;
        }
        std::vector<int> inst_operand_distances;
        absl::c_transform(inst->operands(),
                          std::back_inserter(inst_operand_distances),
                          GetOperandDistanceFunction(hlo_live_range_, inst));
        std::vector<int> candidate_inst_operand_distances;
        absl::c_transform(
            candidate_inst->operands(),
            std::back_inserter(candidate_inst_operand_distances),
            GetOperandDistanceFunction(hlo_live_range_, candidate_inst));
        VLOG(3) << "i : " << i << " "
                << absl::StrJoin(inst_operand_distances, ", ") << " | "
                << absl::StrJoin(candidate_inst_operand_distances, ", ");
        if (!AreOperandCandidatesCompatible(loop_size_candidate,
                                            inst_operand_distances,
                                            candidate_inst_operand_distances)) {
          // Operand distance mistatch.
          continue;
        }
        // Found the start of the loop.
        loop_start_idx = i;
      }
      if (inst->parent() != instruction_sequence[loop_start_idx]->parent()) {
        VLOG(3) << "Mismatch (computation) at " << i << ": "
                << inst->parent()->name() << " vs "
                << instruction_sequence[loop_start_idx]->parent()->name();
        break;
      }
      operand_distances.push_back({});
      if (ignore_op(inst) || fingerprint_it == fingerprint_map_.end()) {
        continue;
      }
      absl::c_transform(inst->operands(),
                        std::back_inserter(operand_distances.back()),
                        GetOperandDistanceFunction(hlo_live_range_, inst));
      if (i >= loop_start_idx + loop_size_candidate) {
        // Verify that this still obeys the fingerprint and operand distance
        // invariants.
        const HloInstruction* prev_inst =
            instruction_sequence[i - loop_size_candidate];
        auto prev_fingerprint_it = fingerprint_map_.find(prev_inst);
        if (prev_fingerprint_it == fingerprint_map_.end()) {
          break;
        }
        if (fingerprint_it->second != prev_fingerprint_it->second) {
          VLOG(3) << "Mismatch (fp) at " << i << ", "
                  << (i - loop_size_candidate) << ": " << fingerprint_it->second
                  << " vs " << prev_fingerprint_it->second;
          break;
        }
        if (!AreOperandCandidatesCompatible(
                loop_size_candidate,
                *(operand_distances.rbegin() + loop_size_candidate),
                operand_distances.back())) {
          VLOG(3) << "Mismatch (op) at " << i << ", "
                  << (i - loop_size_candidate) << ": "
                  << absl::StrJoin(operand_distances.back(), ", ") << " vs "
                  << absl::StrJoin(
                         *(operand_distances.rbegin() + loop_size_candidate),
                         ", ");
          break;
        }
      }
      loop_end_idx = i;
    }
    float num_iterations = 0;
    if (loop_start_idx != -1) {
      num_iterations = static_cast<float>(loop_end_idx + 1 - loop_start_idx) /
                       loop_size_candidate;
    }
    VLOG(3) << "Loop start: " << loop_start_idx << " loop end: " << loop_end_idx
            << " num iterations: " << num_iterations;

    optimized_loop_idx = std::max(optimized_loop_idx, loop_end_idx) + 1;

    if (num_iterations >= kMinNumIterations) {
      VLOG(2) << "Found valid loop. Loop start: " << loop_start_idx
              << " loop end: " << loop_end_idx
              << " num iterations: " << num_iterations;

      TF_CHECK_OK(OptimizeMemoryBoundLoop(loop_start_idx, loop_end_idx,
                                          loop_size_candidate));
    }
  }
}

HeapSimulator::Result<HloValue> AlternateMemoryBestFitHeap::Finish() {
  if (options_.autotuning_config.has_value()) {
    CHECK_EQ((*options_.autotuning_config).size(), buffer_intervals_.size());
  }
  VLOG(1) << "Slicing is "
          << (options_.sliced_prefetch_options.max_slices() >= 2 ? "enabled"
                                                                 : "disabled");

  AllocateReservedScopedAllocations();
  std::vector<BufferInterval> sorted_buffer_intervals =
      GetSortedBufferIntervals();
  memory_space_assignment::CustomizeSortedBufferInterval(
      options_.autotuning_config, sorted_buffer_intervals);

  // Calculate the memory pressure for the buffers that can be assigned in the
  // alternate memory.
  memory_pressure_ = 0;
  for (auto& interval : sorted_buffer_intervals) {
    if (!interval.need_allocation ||
        !MemorySpaceAssignmentUtils::IsIntervalAllowedInAlternateMemory(
            interval) ||
        interval.size > available_heap_size()) {
      continue;
    }
    memory_pressure_ += interval.size;
  }
  VLOG(1) << "Memory pressure = " << memory_pressure_;

  if (options_.enable_cross_program_prefetch) {
    std::vector<AlternateMemoryBestFitHeap::BufferInterval>
        prefetch_candidates = FindCrossProgramPrefetchCandidates(
            alias_analysis_, hlo_live_range_, options_);
    for (auto& prefetch_candidate : prefetch_candidates) {
      HloModule* module = prefetch_candidate.buffer->instruction()->GetModule();
      if (0 <= options().max_cross_program_prefetches &&
          options().max_cross_program_prefetches <=
              module->CrossProgramPrefetches().size()) {
        break;
      }
      AllocateCrossProgramPrefetchBuffer(module, prefetch_candidate);
    }
  }

  VLOG(1) << "Assigning buffers to alternate memory. Max heap size = "
          << options_.max_size_in_bytes;

  AddInputAndOutputRequiredAssignments();

  if (VLOG_IS_ON(3) || options_.dump_fn != nullptr) {
    VLOG(3) << "Flattened instruction sequence:";
    const auto& instruction_sequence =
        hlo_live_range_.flattened_instruction_sequence().instructions();
    absl::StrAppend(&instruction_schedule_str_, "time,instruction_name\n");
    for (int i = 0; i < instruction_sequence.size(); ++i) {
      VLOG(3) << " " << i << ": " << instruction_sequence[i]->parent()->name()
              << " " << instruction_sequence[i]->name();
      absl::StrAppend(&instruction_schedule_str_, i, ",",
                      instruction_sequence[i]->name(), "\n");
    }
  }

  if (options_.memory_bound_loop_optimizer_options.enabled()) {
    IdentifyAndOptimizeMemoryBoundLoops();
  }

  for (const auto& interval : sorted_buffer_intervals) {
    auto colocated_intervals = GetSortedColocatedIntervals(interval);
    if (AreIntervalsReservedInAlternateMemory(colocated_intervals)) {
      // Increment the reserved part of alternate memory so that it is not
      // available for other buffers.
      reserved_in_bytes_ += options_.size_fn(*interval.buffer);
    }
  }
  VLOG(2) << "Total reserved bytes = " << reserved_in_bytes_;

  for (auto& interval : sorted_buffer_intervals) {
    if (!interval.need_allocation) {
      continue;
    }

    if (!MemorySpaceAssignmentUtils::IsIntervalAllowedInAlternateMemory(
            interval)) {
      continue;
    }

    HloInstruction* inst = interval.buffer->instruction();
    HloModule* module = inst->GetModule();

    // Don't intra-program prefetch a cross program prefetch
    auto cross_program_prefetches = module->CrossProgramPrefetches();
    if (inst->opcode() == HloOpcode::kParameter &&
        absl::c_find_if(cross_program_prefetches, [&](auto& info) {
          return info.parameter == inst->parameter_number() &&
                 info.index == interval.buffer->index();
        }) != module->CrossProgramPrefetches().end()) {
      VLOG(3) << "Skip " << interval.buffer->ToShortString()
              << " because it is cross-program prefetched.";
      continue;
    }

    if (interval.size > available_heap_size()) {
      VLOG(3) << "Skip " << interval.buffer->ToShortString()
              << " because the buffer is larger than the heap size.";
      continue;
    }

    auto colocated_intervals = GetSortedColocatedIntervals(interval);

    if (AreIntervalsReservedInAlternateMemory(colocated_intervals)) {
      VLOG(3) << "Interval " << interval.buffer->ToShortString()
              << " is reserved in the alternate memory.";
      for (const BufferInterval* colocated_interval : colocated_intervals) {
        const HloValue* value = colocated_interval->buffer;
        // Color all of the aliased reserved buffers here because reserved
        // alternate memory allocations will not have an entry in preset
        // allocations that is normally used for coloring.
        for (auto& position : value->positions()) {
          VLOG(4) << "Coloring " << position.ToString();
          Shape* shape = ShapeUtil::GetMutableSubshape(
              position.instruction->mutable_shape(), position.index);
          CHECK(shape->IsArray()) << "Coloring a shape that is not an array: "
                                  << position.ToString();
          shape->mutable_layout()->set_memory_space(
              options_.alternate_memory_space);
        }
      }
      continue;
    }

    if (colocated_intervals.size() > 1 &&
        !options_.allocate_across_sequential_calls) {
      VLOG(4) << "Not allocating " << interval.buffer->ToShortString()
              << " because it aliases with another interval and "
              << " allocate_across_sequential_calls is false.";
      continue;
    }

    if (!ConsumeFuel("memory_space_assignment", [&] {
          return absl::StrCat("Ran out of fuel at buffer: ",
                              colocated_intervals[0]->buffer->ToShortString());
        })) {
      continue;
    }

    if (options_.dump_fn != nullptr || VLOG_IS_ON(3)) {
      // Only fill buffer_info_str_ if needed.
      AppendBufferInfoDebugString(interval, &buffer_info_str_);
    }

    std::vector<AllocationValue> allocation_values;
    CreateAllocationValuesFromColocatedIntervals(colocated_intervals,
                                                 allocation_values);

    // Retry allocating this value with larger limits if allocation fails.
    bool repacked = false;
    for (int retry_number = 0; retry_number < options_.max_retries;
         retry_number++) {
      AddRequiredAssignmentsForColocatedIntervals(colocated_intervals);
      options_.prefetch_interval_picker->SetRetryNumber(retry_number);
      Result result =
          AllocateAllocationValues(absl::MakeSpan(allocation_values));
      VLOG(2) << "Allocation result = "
              << absl::StrFormat("%x", static_cast<int>(result));
      if (result_requires_uncommit(result)) {
        UncommitPendingChunks(absl::MakeSpan(allocation_values));
        VLOG(2) << "Couldn't allocate. Retry number " << retry_number;
      } else if ((result_is(result, Result::kFailOutOfMemory) ||
                  options_.repack_after_every_allocation) &&
                 num_repacks_ < options_.max_repacks && !repacked) {
        UncommitPendingChunks(absl::MakeSpan(allocation_values));
        ++num_repacks_;
        repacked = true;
        CHECK_NE(options_.repacker, nullptr);
        std::vector<MemorySpaceAssignmentRepacker::AllocationBlock*>
            repack_allocation_blocks;
        ExportAllocationsForRepacking(repack_allocation_blocks);
        VLOG(2) << "Repacking.";
        auto repack_status =
            options_.repacker->Repack(absl::MakeSpan(repack_allocation_blocks));
        CHECK_EQ(repack_status.status(), OkStatus());
        VLOG(2) << "Repack complete. Modified = " << *repack_status;
        // For debug and testing purpose, also update allocations if
        // repack_after_every_allocation is on.
        if (*repack_status || options_.repack_after_every_allocation) {
          ImportRepackedAllocations();
          --retry_number;
        }
      } else {
        // Check if any of the allocation sites are inefficient. If so, get rid
        // of the pending allocation, require all of the inefficient sites in
        // the default memory, and perform allocation again.
        std::vector<HloPositionOrUse> inefficient_sites =
            GetInefficientAllocationSites(allocation_values);
        if (!inefficient_sites.empty()) {
          UncommitPendingChunks(absl::MakeSpan(allocation_values));
          for (const HloPositionOrUse& site : inefficient_sites) {
            // To avoid a livelock situation, we commit the required assignments
            // right away. Otherwise, reallocation can find alternate memory
            // allocations at other sites, which can also be inefficient.
            std::visit(
                [this](const auto& site) {
                  VLOG(3) << "Inefficient site: " << site.ToString();
                  AddRequiredAssignment(site, MemorySpace::kDefault,
                                        /*offset=*/nullptr,
                                        /*add_to_pending=*/false);
                },
                site);
          }
          --retry_number;
          continue;
        }

        FinalizeAllocations(absl::MakeSpan(allocation_values));
        break;
      }
    }
  }
  if (options_.repack_after_every_allocation) {
    CHECK_NE(options_.repacker, nullptr);
    std::vector<MemorySpaceAssignmentRepacker::AllocationBlock*>
        repack_allocation_blocks;
    ExportAllocationsForRepacking(repack_allocation_blocks);
    VLOG(2) << "Final Repacking.";
    auto repack_status =
        options_.repacker->Repack(absl::MakeSpan(repack_allocation_blocks));
    CHECK_EQ(repack_status.status(), OkStatus());
    VLOG(2) << "Final Repack complete. Modified = " << *repack_status;
  }

  if (options_.dump_fn != nullptr || VLOG_IS_ON(3)) {
    for (auto& allocation : *allocations_) {
      // Only fill allocation_info_str_ if needed.
      AppendAllocationInfoDebugString(*allocation, allocation_info_str_);
    }
  }

  VLOG(3) << "Debug buffer info: ";
  XLA_VLOG_LINES(3, buffer_info_str_);
  VLOG(3) << "Debug allocation info: ";
  XLA_VLOG_LINES(3, allocation_info_str_);
  DumpDebugStringsIfEnabled();

  HeapSimulator::Result<HloValue> result;
  result.heap_size = result_.heap_size;
  result.heap_results.emplace_back(std::move(result_));
  return result;
}

namespace {

// Convert a tuple HloUse to its equivalent HloPosition.
HloPosition TupleUseToPosition(const HloUse& use) {
  CHECK_EQ(use.instruction->opcode(), HloOpcode::kTuple);
  ShapeIndex index = use.operand_index;
  index.push_front(use.operand_number);
  return {use.instruction, index};
}

// Returns the memory space of the defining position of an Allocation object.
MemorySpaceAssignment::MemorySpace GetDefiningPositionMemorySpace(
    const MemorySpaceAssignment::Allocation& allocation) {
  if (!allocation.is_copy_like_allocation()) {
    return allocation.memory_space();
  }
  if (allocation.memory_space() ==
      MemorySpaceAssignment::MemorySpace::kDefault) {
    return MemorySpaceAssignment::MemorySpace::kAlternate;
  }
  return MemorySpaceAssignment::MemorySpace::kDefault;
}

}  // namespace

std::vector<std::vector<const MemorySpaceAssignment::Allocation*>>
AlternateMemoryBestFitHeap::GetLinkedAllocationsInAlternateMemory(
    absl::Span<const AlternateMemoryBestFitHeap::AllocationValue>
        allocation_values) const {
  std::vector<std::vector<const MemorySpaceAssignment::Allocation*>>
      linked_allocations;
  // A map from position to index into linked_allocations.
  absl::flat_hash_map<HloPosition, int> link_id_map;
  // Iterate over the allocation values. Find Allocation objects across the
  // allocation values that are part of the same linked allocation group. We
  // define a linked allocation group as Allocation objects that have aliased
  // positions or uses. An example would be an Allocation object that has a
  // dynamic-update-slice use and another Allocation object that has the same
  // dynamic-update-slice as its defining position.
  for (const AllocationValue& allocation_value : allocation_values) {
    absl::flat_hash_map<HloUse, std::vector<HloPosition>> aliases;
    for (const AllocationValue::Use& allocation_value_use :
         allocation_value.uses()) {
      if (!allocation_value_use.aliases.empty()) {
        aliases[allocation_value_use.hlo_use] = allocation_value_use.aliases;
      }
    }
    for (const auto& allocation : *allocation_value.allocation_sequence()) {
      MemorySpace position_memory_space =
          GetDefiningPositionMemorySpace(*allocation);
      if (allocation->memory_space() == MemorySpace::kDefault &&
          position_memory_space == MemorySpace::kDefault) {
        // This is just a regular allocation in the default memory, skip.
        continue;
      }
      int link_id = -1;
      // For every position and use in the alternate memory space, check if
      // there is already a linked allocation group, and if so, use that link
      // index.
      if (position_memory_space == MemorySpace::kAlternate) {
        auto link_id_map_it = link_id_map.find(allocation->defining_position());
        if (link_id_map_it != link_id_map.end()) {
          link_id = link_id_map_it->second;
        }
      }
      if (allocation->memory_space() == MemorySpace::kAlternate) {
        for (const HloUse& use : allocation->uses()) {
          if (use.instruction->opcode() == HloOpcode::kTuple) {
            auto link_id_map_it = link_id_map.find(TupleUseToPosition(use));
            if (link_id_map_it != link_id_map.end()) {
              if (link_id != -1 && link_id != link_id_map_it->second) {
                // We found multiple link indices for the given allocation. We
                // merge the two linked allocation groups in that case.
                int old_link_id = link_id_map_it->second;
                if (old_link_id < link_id) {
                  std::swap(link_id, old_link_id);
                }
                absl::c_copy(linked_allocations[old_link_id],
                             std::back_inserter(linked_allocations[link_id]));
                linked_allocations[old_link_id].clear();
                for (auto it = link_id_map.begin(); it != link_id_map.end();
                     ++it) {
                  if (it->second == old_link_id) {
                    it->second = link_id;
                  }
                }
              }
              link_id = link_id_map_it->second;
            }
          }
        }
      }
      if (link_id == -1) {
        // Create a new linked allocation group if we couldn't find one.
        link_id = linked_allocations.size();
        linked_allocations.push_back({allocation.get()});
      } else {
        linked_allocations[link_id].push_back(allocation.get());
      }
      // Propagate the link index to all of the aliases of uses in the alternate
      // memory.
      if (allocation->memory_space() == MemorySpace::kAlternate) {
        for (const HloUse& use : allocation->uses()) {
          auto alias_it = aliases.find(use);
          if (alias_it != aliases.end()) {
            for (const HloPosition& aliased_position : alias_it->second) {
              link_id_map[aliased_position] = link_id;
            }
          }
        }
      }
    }
  }

  linked_allocations.erase(
      std::remove_if(
          linked_allocations.begin(), linked_allocations.end(),
          [](const auto& allocations) { return allocations.empty(); }),
      linked_allocations.end());

  if (VLOG_IS_ON(3)) {
    for (int i = 0; i < linked_allocations.size(); ++i) {
      VLOG(3) << "Link id = " << i;
      for (const MemorySpaceAssignment::Allocation* allocation :
           linked_allocations[i]) {
        VLOG(3) << "  " << allocation->ToString();
      }
    }
  }
  return linked_allocations;
}

std::vector<AlternateMemoryBestFitHeap::HloPositionOrUse>
AlternateMemoryBestFitHeap::GetInefficientAllocationSites(
    absl::Span<const AlternateMemoryBestFitHeap::AllocationValue>
        allocation_values) const {
  // The logic below is used mostly for testing, allowing a test case to inject
  // some custom logic for this method.
  if (options_.get_inefficient_allocation_sites_fn) {
    std::vector<HloPosition> defining_positions;
    defining_positions.reserve(allocation_values.size());
    for (const AllocationValue& value : allocation_values) {
      defining_positions.push_back(value.defining_position());
    }
    return options_.get_inefficient_allocation_sites_fn(
        absl::MakeSpan(defining_positions));
  }

  if (!options_.cost_analysis ||
      options_.inefficient_use_to_copy_ratio == 0.0) {
    return {};
  }

  int64_t size = allocation_values.at(0).size();

  if (VLOG_IS_ON(3)) {
    for (const AllocationValue& allocation_value : allocation_values) {
      for (const auto& allocation : *allocation_value.allocation_sequence()) {
        VLOG(3) << " Allocation: " << allocation->ToString();
        if (!allocation->is_copy_like_allocation()) {
          const HloPosition& defining_position =
              allocation->defining_position();
          int64_t accessed =
              options_.cost_analysis->cost_analysis().output_bytes_accessed(
                  *defining_position.instruction, defining_position.index);
          VLOG(3) << "  pos: " << defining_position.ToString()
                  << ", accessed: " << accessed << " / " << size;
        }
        for (const HloUse& use : allocation->uses()) {
          int64_t accessed =
              options_.cost_analysis->cost_analysis().operand_bytes_accessed(
                  *use.instruction, use.operand_number, use.operand_index);
          VLOG(3) << "  use: " << use.ToString() << ", accessed: " << accessed
                  << " / " << size;
        }
      }
    }
  }

  std::vector<std::vector<const MemorySpaceAssignment::Allocation*>>
      linked_allocations =
          GetLinkedAllocationsInAlternateMemory(allocation_values);
  std::vector<AlternateMemoryBestFitHeap::HloPositionOrUse> inefficient_sites;
  for (const std::vector<const MemorySpaceAssignment::Allocation*>&
           allocation_group : linked_allocations) {
    // For all of allocation in the linked allocation group, calculate the total
    // use bytes in alternate memory and async copy bytes. If the ratio between
    // the two is below inefficient_use_to_copy_ratio, add all of the
    // participating allocation sites into inefficient_sites.
    VLOG(3) << "AllocationGroup:";
    int64_t copy_bytes = 0;
    int64_t use_bytes = 0;
    for (const MemorySpaceAssignment::Allocation* allocation :
         allocation_group) {
      VLOG(3) << " Allocation: " << allocation->ToString();
      MemorySpace position_memory_space =
          GetDefiningPositionMemorySpace(*allocation);
      if (allocation->is_copy_like_allocation()) {
        copy_bytes += size;
      }
      if (position_memory_space == MemorySpace::kAlternate) {
        use_bytes +=
            options_.cost_analysis->cost_analysis().output_bytes_accessed(
                *allocation->defining_position().instruction,
                allocation->defining_position().index);
      }
      if (allocation->memory_space() == MemorySpace::kAlternate) {
        for (const HloUse& use : allocation->uses()) {
          use_bytes +=
              options_.cost_analysis->cost_analysis().operand_bytes_accessed(
                  *use.instruction, use.operand_number, use.operand_index);
        }
      }
    }
    VLOG(3) << " use bytes: " << use_bytes << ", copy bytes: " << copy_bytes;
    if (options_.inefficient_use_to_copy_ratio * copy_bytes > use_bytes) {
      for (const MemorySpaceAssignment::Allocation* allocation :
           allocation_group) {
        MemorySpace position_memory_space =
            GetDefiningPositionMemorySpace(*allocation);
        if (position_memory_space == MemorySpace::kAlternate) {
          if (!allocation->is_copy_like_allocation()) {
            inefficient_sites.push_back(allocation->defining_position());
          }
        }
        if (allocation->memory_space() == MemorySpace::kAlternate) {
          for (const HloUse& use : allocation->uses()) {
            inefficient_sites.push_back(use);
          }
        }
      }
    }
  }
  return inefficient_sites;
}

void AlternateMemoryBestFitHeap::AddRequiredAssignmentsForColocatedIntervals(
    absl::Span<const AlternateMemoryBestFitHeap::BufferInterval* const>
        colocated_intervals) {
  // TODO(berkin): For now, place the phi values due to conditionals in
  // default memory.
  for (const BufferInterval* colocated_interval : colocated_intervals) {
    const HloValue* value = colocated_interval->buffer;
    for (const auto& position : value->positions()) {
      if (position.instruction->opcode() == HloOpcode::kConditional) {
        VLOG(3) << "Adding required assignment for condition output: "
                << value->ToShortString();
        AddRequiredAssignment(position.instruction, position.index,
                              MemorySpace::kDefault);
        for (const HloComputation* called_computation :
             position.instruction->called_computations()) {
          AddRequiredAssignment(called_computation->root_instruction(),
                                position.index, MemorySpace::kDefault);
        }
      }
    }
  }
}

void AlternateMemoryBestFitHeap::CreateAllocationValuesFromColocatedIntervals(
    absl::Span<const AlternateMemoryBestFitHeap::BufferInterval* const>
        colocated_intervals,
    std::vector<MemorySpaceAssignment::AllocationValue>& allocation_values) {
  // Create AllocationValues for all the colocated intervals.
  for (const auto& colocated_interval : colocated_intervals) {
    CreateAllocationValues(*colocated_interval, allocation_values);
  }
  // Go through the AllocationValues and delete the ones that have the identical
  // defining instruction and use instructions. This is useful for async
  // operations that can read and write to the same buffer, e.g., in-place
  // asynchronous collective permute. The AllocationValues that corresponds to
  // collective-permute-start{0} (the input) and collective-permute-start{1}
  // (the output) refer to the same buffer by definition (since they are created
  // from colocated intervals). If we don't delete one of these buffers, then
  // when we try to allocate the AllocationValue, we would think they overlap.
  auto create_instruction_vector = [](const AllocationValue& allocation_value) {
    std::vector<const HloInstruction*> instruction_vector;
    instruction_vector.push_back(allocation_value.defining_instruction());
    for (const AllocationValue::Use& use : allocation_value.uses()) {
      instruction_vector.push_back(use.hlo_use.instruction);
    }
    return instruction_vector;
  };
  for (int i = 0; i < allocation_values.size() - 1; ++i) {
    for (int j = i + 1; j < allocation_values.size(); ++j) {
      const AllocationValue& allocation_value_1 = allocation_values[i];
      const AllocationValue& allocation_value_2 = allocation_values[j];
      if (create_instruction_vector(allocation_value_1) ==
          create_instruction_vector(allocation_value_2)) {
        VLOG(3) << "Allocation values " << allocation_value_1.ToShortString()
                << " and " << allocation_value_2.ToShortString()
                << " are equivalent, deleting the second one.";
        allocation_values.erase(allocation_values.begin() + j);
        --j;
      }
    }
  }

  FindAliases(&allocation_values);
}

AlternateMemoryBestFitHeap::Result
AlternateMemoryBestFitHeap::AllocateAllocationValues(
    absl::Span<MemorySpaceAssignment::AllocationValue> allocation_values) {
  const auto& instruction_schedule = hlo_live_range_.instruction_schedule();

  // Find the use times across all of the related AllocationValues and sort
  // them. We use these to find allocations that are available throughout the
  // entire live range of all the AllocationValues.
  std::vector<int64_t> all_use_times;
  for (const AllocationValue& allocation_value : allocation_values) {
    absl::c_transform(allocation_value.uses(),
                      std::back_inserter(all_use_times),
                      [](const AllocationValue::Use& use) { return use.time; });
  }
  absl::c_sort(all_use_times);

  // Data structure to contain the preferred offset for a given computation.
  // We ensure that the same offset will be allocated outside the while loop
  // as well as inside the while loop.
  absl::flat_hash_map<const HloComputation*, AliasedOffset*>
      preferred_offset_for_computation;

  Result result = Result::kSuccess;
  for (AllocationValue& allocation_value : allocation_values) {
    int64_t definition_time =
        instruction_schedule.at(allocation_value.defining_instruction());

    if (!options_.is_position_allowed_in_alternate_mem_fn(
            allocation_value.defining_position())) {
      AddRequiredAssignment(allocation_value.value(),
                            allocation_value.defining_instruction(),
                            MemorySpace::kDefault, definition_time);
    }

    AliasedOffset* preferred_offset = nullptr;
    auto preferred_offset_it =
        preferred_offset_for_computation.find(allocation_value.computation());
    if (preferred_offset_it != preferred_offset_for_computation.end()) {
      preferred_offset = preferred_offset_it->second;
    }

    // Iterate over the uses.
    for (int use_idx = 0; use_idx < allocation_value.uses().size(); ++use_idx) {
      const AllocationValue::Use& use = allocation_value.uses().at(use_idx);
      const HloUse hlo_use = use.hlo_use;
      int64_t use_time = instruction_schedule.at(hlo_use.instruction);
      int64_t latest_prefetch_time = use_time;
      bool allow_no_copy_alternate_mem_allocation = true;
      bool allow_prefetch = true;
      bool prefer_no_copy_alternate_mem_allocation = false;
      std::optional<int64_t> earliest_prefetch_time = std::nullopt;

      // Control flow  calls include kWhile, kCall, and kConditional opcodes.
      bool is_sequential_call =
          (GetInstructionCallContext(hlo_use.instruction->opcode()) ==
           CallContext::kControlFlow);
      if (is_sequential_call) {
        for (const HloComputation* called_computation :
             hlo_use.instruction->called_computations()) {
          const HloLiveRange::TimeBound& computation_span =
              hlo_live_range_.computation_span_times().at(called_computation);
          latest_prefetch_time =
              std::min(computation_span.start - 1, latest_prefetch_time);
        }
        if (hlo_use.instruction->opcode() == HloOpcode::kWhile) {
          // Given an example while loop and flattened schedule (logical times
          // shown on the left):
          //
          // 0:  a = ...
          // 1:  ...
          //     cond {
          // 2:   p = param(0)
          // 3:   ...
          //     }
          //     body {
          // 4:   p = param(0)
          // 5:   ...
          // 6:   ROOT ...
          //     }
          // 7:  w = while(a), body=body, cond=cond
          //
          // When processing "a" (time 0) and its while use (time 7), we update
          // the interval to time 0-4. This is so that the remaining interval
          // (5-6) can be allocated separately and this buffer doesn't waste
          // alternate memory space within the while loop body.
          HloComputation* while_body = hlo_use.instruction->while_body();
          // We require while body ROOTs to be the last in the schedule.
          CHECK_EQ(instruction_schedule.at(while_body->root_instruction()) + 1,
                   instruction_schedule.at(hlo_use.instruction))
              << "While body ROOTs need to be the last in the schedule! "
                 "Please run RootInstructionSinker.";
          // Replace the use time with the parameter time so that we can decide
          // on alternate memory allocations within the while loop body when we
          // look at uses within the while loop body.
          use_time =
              instruction_schedule.at(while_body->parameter_instruction(0));
        } else if (hlo_use.instruction->opcode() == HloOpcode::kConditional) {
          // Replace the use time with the earliest parameter of called
          // computations.
          for (const HloComputation* called_computation :
               hlo_use.instruction->called_computations()) {
            use_time = std::min(
                use_time, instruction_schedule.at(
                              called_computation->parameter_instruction(0)));
          }
        }
      }

      // Add a required assignment in default memory if the use not allowed in
      // alternate memory.
      if (!IsUseAllowedInAlternateMemory(allocation_value, hlo_use)) {
        AddRequiredAssignment(allocation_value.value(), hlo_use.instruction,
                              MemorySpace::kDefault, use_time);
      } else if (use_idx > 0) {
        // We allow buffers in alternate memory that are passed into
        // conditionals to give up their alternate memory allocation inside the
        // called computation. This means that if a conditional operator has an
        // alternate memory allocation, subsequent uses cannot use the same
        // alternate memory allocation in order not to clobber data. So we force
        // default memory allocation for these subsequent uses.
        const AllocationValue::Use& previous_use =
            allocation_value.uses().at(use_idx - 1);
        if (previous_use.hlo_use.instruction->opcode() ==
                HloOpcode::kConditional &&
            previous_use.hlo_use.instruction != hlo_use.instruction) {
          allow_no_copy_alternate_mem_allocation = false;
          earliest_prefetch_time =
              instruction_schedule.at(previous_use.hlo_use.instruction);
          VLOG(3) << "Previous use (" << previous_use.hlo_use.ToString()
                  << ") of use (" << hlo_use.ToString()
                  << ") is a conditional, so this use will need to evict. "
                  << "Earliest prefetch time = " << *earliest_prefetch_time;
        }
      }

      // Bitcasts don't define buffers and don't directly consume buffers. Skip
      // allocating buffers for bitcast uses (unless they are the root
      // instruction). The uses that feed from bitcasts will be handled
      // specially.
      if (hlo_use.instruction->opcode() != HloOpcode::kBitcast ||
          hlo_use.instruction ==
              hlo_use.instruction->parent()->root_instruction()) {
        std::optional<int64_t> preferred_prefetch_time = std::nullopt;
        auto loop_optimized_allocation_it =
            loop_optimized_allocations_map_.find(use.hlo_use);
        if (loop_optimized_allocation_it !=
            loop_optimized_allocations_map_.end()) {
          const LoopOptimizedAllocationInfo& loop_optimized_allocation_info =
              loop_optimized_allocation_it->second;
          const MemorySpaceAssignment::Allocation* allocation =
              loop_optimized_allocation_info.loop_optimized_allocation;
          VLOG(3) << "Found optimized allocation for " << use.hlo_use.ToString()
                  << " (loop idx: " << loop_optimized_allocation_info.use_index
                  << "): " << allocation->ToString();
          if (allocation->is_copy_allocation()) {
            allow_no_copy_alternate_mem_allocation = true;
            const MemorySpaceAssignment::CopyAllocation* copy_allocation =
                static_cast<const MemorySpaceAssignment::CopyAllocation*>(
                    allocation);
            int64_t effective_copy_start_time =
                copy_allocation->copy_start_schedule_after();
            if (copy_allocation->copy_start_schedule_after() ==
                    loop_optimized_allocation_info.loop_size - 1 &&
                copy_allocation->copy_done_schedule_before() == 0) {
              effective_copy_start_time =
                  -loop_optimized_allocation_info.loop_size;
            } else if (copy_allocation->copy_start_schedule_after() + 1 >=
                       copy_allocation->copy_done_schedule_before()) {
              effective_copy_start_time -=
                  loop_optimized_allocation_info.loop_size;
            }
            preferred_prefetch_time =
                hlo_live_range_.instruction_schedule().at(hlo_use.instruction) -
                loop_optimized_allocation_info.use_index +
                effective_copy_start_time;
            VLOG(3) << "Prefer prefetch at " << *preferred_prefetch_time
                    << " (effective: " << effective_copy_start_time << ")";
          } else if (allocation->memory_space() == MemorySpace::kDefault) {
            allow_prefetch = false;
            allow_no_copy_alternate_mem_allocation = false;
            VLOG(3) << "Disallowing alternate memory allocation.";
          } else {
            CHECK(allocation->memory_space() == MemorySpace::kAlternate);
            prefer_no_copy_alternate_mem_allocation = true;
            VLOG(3) << "Prefer no-copy alternate memory allocation.";
          }
        }

        if (options_.use_repeated_instance_for_preferred_prefetch_time) {
          const std::vector<const HloInstruction*>* repeated_insts =
              GetRepeatedInstructionList(hlo_use.instruction);
          if (repeated_insts) {
            for (int i = 0; i < repeated_insts->size(); ++i) {
              const HloInstruction* repeated = repeated_insts->at(i);
              VLOG(4) << "Repeated instruction for use: " << repeated->name()
                      << " "
                      << hlo_live_range_.instruction_schedule().at(repeated);
              if (repeated == hlo_use.instruction && i > 0) {
                const HloInstruction* prev_repeated = repeated_insts->at(i - 1);
                if (prev_repeated->parent() == hlo_use.instruction->parent()) {
                  preferred_prefetch_time =
                      hlo_live_range_.instruction_schedule().at(prev_repeated) +
                      1;
                  VLOG(3) << "Found a previous repeated ("
                          << prev_repeated->name() << ") at "
                          << (*preferred_prefetch_time - 1)
                          << ". Setting preferred prefetch time = "
                          << *preferred_prefetch_time;
                }
              }
            }
          }
        }
        AllocationRequest request;

        int64_t live_range_start_time =
            (earliest_prefetch_time.has_value()
                 ? earliest_prefetch_time.value()
                 : std::min(definition_time, use_time));
        auto overridden_preferred_prefetch_time =
            GetOverriddenPreferredPrefetchTime(
                options_.filter_update_preferred_prefetches,
                allocation_value.size(), hlo_use, instruction_schedule,
                live_range_start_time, latest_prefetch_time);
        TF_CHECK_OK(overridden_preferred_prefetch_time.status());
        if (overridden_preferred_prefetch_time.value().has_value()) {
          LOG(INFO) << "Overriding preferred prefetch for "
                    << hlo_use.instruction->name() << " operand number "
                    << hlo_use.operand_number << " operand index "
                    << hlo_use.operand_index.ToString() << " size "
                    << allocation_value.size() << " live range ("
                    << live_range_start_time << ", " << latest_prefetch_time
                    << ") from "
                    << (preferred_prefetch_time.has_value()
                            ? preferred_prefetch_time.value()
                            : -1)
                    << " to "
                    << overridden_preferred_prefetch_time.value().value();
          preferred_prefetch_time = overridden_preferred_prefetch_time.value();
        }

        // Rarely, (e.g., when conditional true and false parameters are the
        // same), definition time can be the time of the conditional and use
        // time is the parameter use, which is less.
        request.inclusive_start_time = std::min(definition_time, use_time);
        request.end_time = use_time;
        request.latest_prefetch_time = latest_prefetch_time;
        request.size = allocation_value.size();
        request.prefer_no_copy_alternate_mem_allocation =
            prefer_no_copy_alternate_mem_allocation;
        request.allow_no_copy_alternate_mem_allocation =
            allow_no_copy_alternate_mem_allocation;
        request.allow_prefetch = allow_prefetch;
        request.earliest_prefetch_time = earliest_prefetch_time;
        request.preferred_prefetch_time = preferred_prefetch_time;
        request.preferred_offset = preferred_offset;
        request.use = &use;
        request.allocation_value = &allocation_value;
        request.all_use_times = all_use_times;
        result_mark(AllocateSegment(request), result);
        if (result_requires_uncommit(result)) {
          // If the allocation finding failed (e.g., due to running out of
          // asynchronous copies), then fall back to allocating the buffer
          // entirely in the default memory.
          return result;
        }

        // If there are multiple uses, they can try using the memory allocation
        // already at the alternate memory.
        definition_time = instruction_schedule.at(hlo_use.instruction);
      }

      // Propagate the allocation to any aliases this use might have had.
      MemorySpaceAssignment::Allocation* aliased_allocation =
          GetLiveAllocationAt(*allocation_value.allocation_sequence(),
                              use_time);
      for (const HloPosition& aliased_position : use.aliases) {
        AddAliasedRequiredAssignment(aliased_position.instruction,
                                     aliased_position.index,
                                     aliased_allocation);
      }

      if (hlo_use.instruction->opcode() == HloOpcode::kWhile &&
          aliased_allocation->memory_space() == MemorySpace::kAlternate) {
        // For while uses that are allocated in the alternate memory space, if
        // they also have an allocation in the default memory space in their
        // allocation sequence, create a "parent" allocation that mirrors this
        // default memory space allocation. When we process the parent
        // allocation, we add an additional parameter to the while that is a
        // reference to the buffer in the default memory space. With parent
        // allocations, we don't need to unnecessarily evict buffers since they
        // already have a copy in the default memory space. We search backwards
        // (latest to earliest in execution time) for a suitable allocation in
        // order to find the most recent one.
        if (options_.enable_while_redundant_eviction_elimination &&
            absl::c_find_if(allocation_value.value()->positions(),
                            [&hlo_use](const HloPosition& position) {
                              return position.instruction ==
                                         hlo_use.instruction &&
                                     position.index == hlo_use.operand_index;
                            }) != allocation_value.value()->positions().end()) {
          auto allocation_sequence = allocation_value.allocation_sequence();
          auto prev_allocation_in_default_mem_it = std::find_if(
              allocation_sequence->rbegin(), allocation_sequence->rend(),
              [&](const auto& allocation) {
                return allocation->memory_space() == MemorySpace::kDefault &&
                       allocation->defining_position() ==
                           allocation_value.defining_position();
              });
          if (prev_allocation_in_default_mem_it !=
              allocation_sequence->rend()) {
            VLOG(3) << "Found a prev allocation in default mem for while use: "
                    << (*prev_allocation_in_default_mem_it)->ToString();
            auto body_allocation_value_it = absl::c_find_if(
                allocation_values, [&](const AllocationValue& value) {
                  return value.computation() ==
                             hlo_use.instruction->while_body() &&
                         value.defining_instruction()->opcode() ==
                             HloOpcode::kParameter;
                });
            CHECK_NE(body_allocation_value_it, allocation_values.end());
            VLOG(3) << "Body allocation value: "
                    << body_allocation_value_it->ToShortString();
            int64_t body_parameter_time = instruction_schedule.at(
                body_allocation_value_it->defining_instruction());
            body_allocation_value_it->mutable_allocation_sequence()->push_back(
                std::make_unique<MemorySpaceAssignment::ParentAllocation>(
                    **prev_allocation_in_default_mem_it, hlo_use.instruction,
                    body_allocation_value_it->defining_position(),
                    body_parameter_time));
            VLOG(3) << "Created: "
                    << body_allocation_value_it->allocation_sequence()
                           ->back()
                           ->ToString();

            auto after_while_allocation_value_it = absl::c_find_if(
                allocation_values, [&](const AllocationValue& value) {
                  return value.defining_instruction() == hlo_use.instruction;
                });
            CHECK_NE(after_while_allocation_value_it, allocation_values.end());
            VLOG(3) << "After while allocation value: "
                    << after_while_allocation_value_it->ToShortString();
            int64_t while_time = instruction_schedule.at(hlo_use.instruction);
            after_while_allocation_value_it->mutable_allocation_sequence()
                ->push_back(
                    std::make_unique<MemorySpaceAssignment::MirroredAllocation>(
                        **prev_allocation_in_default_mem_it, while_time));
            VLOG(3) << "Created: "
                    << after_while_allocation_value_it->allocation_sequence()
                           ->back()
                           ->ToString();
          }
        }
        // Special case for while loops since the root offset must agree with
        // other offsets: remember the preferred offset for the while loop body.
        preferred_offset_for_computation[hlo_use.instruction->while_body()] =
            GetAliasedOffset(*aliased_allocation);
      }
    }
  }
  return result;
}

bool operator<(const AsynchronousCopy& a, const AsynchronousCopy& b) {
  return a.AsTuple() < b.AsTuple();
}

bool operator==(const AsynchronousCopy& a, const AsynchronousCopy& b) {
  return a.AsTuple() == b.AsTuple();
}

bool operator!=(const AsynchronousCopy& a, const AsynchronousCopy& b) {
  return a.AsTuple() != b.AsTuple();
}

void AsynchronousCopyOrdering::AddCopy(const AsynchronousCopy& copy) {
  auto it = ranges_.find({copy.exclusive_start_time, copy.end_time});
  if (it != ranges_.end()) {
    CHECK_EQ(it->first.exclusive_start_time, copy.exclusive_start_time);
    CHECK(it->second.insert(copy).second);
  } else {
    ranges_[{copy.exclusive_start_time, copy.end_time}] = {copy};
  }
}

void AsynchronousCopyOrdering::RemoveCopy(const AsynchronousCopy& copy) {
  auto copy_it = ranges_.find({copy.exclusive_start_time, copy.end_time});
  CHECK(copy_it != ranges_.end());
  CHECK_EQ(copy_it->first.exclusive_start_time, copy.exclusive_start_time);
  CHECK_EQ(copy_it->second.erase(copy), 1);
  if (copy_it->second.empty()) {
    ranges_.erase(copy_it);
  }
}

bool AsynchronousCopyOrdering::ViolatesOrdering(int64_t exclusive_start_time,
                                                int64_t end_time) const {
  // We allow identical start and end times. It is enough to check for just the
  // start time in case we find a match in ranges_ because the found value will
  // either be identical to {start_time, estimated_end_time} (and this doesn't
  // violate) or its start_time will be smaller and estimated_end_time will be
  // larger (this violates).
  auto copy_it = ranges_.find({exclusive_start_time, end_time});
  if (copy_it != ranges_.end() &&
      copy_it->first.exclusive_start_time != exclusive_start_time) {
    VLOG(4) << "Violates ordering: (" << exclusive_start_time << ", "
            << end_time << ") and (" << copy_it->first.exclusive_start_time
            << ", " << copy_it->first.end_time << ")";
    return true;
  }
  return false;
}

bool AsynchronousCopyResource::ConsumeResource(
    int64_t exclusive_start_time, int64_t end_time, float resource,
    absl::flat_hash_map<int64_t, float>* delay_change_map,
    float resource_to_free) {
  std::list<AsynchronousCopy>::iterator current_copy = async_copies_.end();
  // In order to propagate the resource to the next scheduled copy, we iterate
  // over the copies in start time order until we either find enough free
  // resource (and return true), or find out that we don't have enough free
  // resource (and return false).
  while (true) {
    // resource is modified below. We save its initial value for logging below.
    const float amount_requested = resource;

    VLOG(3) << "Consume resource: start time_exclusive = "
            << exclusive_start_time << ", end time = " << end_time
            << ", resource = " << resource << ", delay = "
            << delay_[ExclusiveToInclusiveStartTime(exclusive_start_time)]
            << ", free = " << resource_to_free;
    VLOG(5) << "Available resources: "
            << VectorToString(
                   GetCurrentResources(), /*include_indices=*/true,
                   ExclusiveToInclusiveStartTime(exclusive_start_time),
                   end_time);

    // Nothing to do if we're not adding or removing any resources.
    if (resource == 0.0 && resource_to_free == 0.0) {
      return true;
    }

    // For the async copy we're adding, check the delay_ array to see how much
    // this copy would have to be delayed because of an earlier copy that wasn't
    // finished when this copy starts.
    if (current_copy == async_copies_.end()) {
      resource += delay_[ExclusiveToInclusiveStartTime(exclusive_start_time)];
    }

    // Find the copy that is right after this one. If there are leftover
    // resources by the time the next copy starts, the next copy will be pushed
    // further later in time.
    std::list<AsynchronousCopy>::iterator next_copy = async_copies_.end();
    if (current_copy != async_copies_.end()) {
      next_copy = std::next(current_copy);
    } else {
      auto async_copy_time_it =
          async_copy_time_map_.upper_bound(exclusive_start_time);
      if (async_copy_time_it != async_copy_time_map_.end()) {
        next_copy = async_copy_time_it->second;
      }
    }

    // Check if this copy will push the next copy later in time (or if removing
    // the resource, check if the removal of this copy move the next copy
    // earlier in time).
    std::optional<float> delay_for_next_copy = std::nullopt;
    float resource_freed = 0.0;
    for (int64_t time = ExclusiveToInclusiveStartTime(exclusive_start_time);
         time < end_time && resource != 0; ++time) {
      // Iterate over the logical times that this copy spans. Note that the
      // start and end time ranges are exclusive.
      float used_resource = std::min(resource, initial_resources_[time]);
      if (next_copy != async_copies_.end() &&
          next_copy->exclusive_start_time ==
              InclusiveToExclusiveStartTime(time)) {
        // This is the time where the next copy begins. If the resource is
        // non-zero at this point, the copy didn't finish by the time the next
        // copy started, so the next copy would need to be pushed later in time.
        delay_for_next_copy = resource;
        resource_to_free -= resource_freed;
      }
      if (!delay_for_next_copy.has_value()) {
        // Update the delay_ vector and resource_freed variable with the amount
        // that was freed when removing the copy.
        float old_resource =
            std::max(0.0f, initial_resources_[time] - delay_[time]);
        if (delay_change_map && !delay_change_map->contains(time)) {
          (*delay_change_map)[time] = delay_[time];
        }
        delay_[time] = std::max(0.0f, resource - resource_to_free);
        float new_resource =
            std::max(0.0f, initial_resources_[time] - delay_[time]);
        resource_freed += std::max(0.0f, new_resource - old_resource);
      }
      // Update the resource with the used amount in this logical time.
      resource -= used_resource;
    }

    // If resource isn't satisfied by the end, we didn't have enough resources.
    if (resource > 0) {
      VLOG(3) << "Doesn't have enough resource; requested resource = "
              << amount_requested << "; leftover resources = " << resource;
      return false;
    }

    if (!delay_for_next_copy.has_value()) {
      return true;
    }
    // If this copy overlapped with another one, we run for another iteration
    // with the next copy  with the amount of resource that needs to be added or
    // removed.
    exclusive_start_time = next_copy->exclusive_start_time;
    end_time = next_copy->end_time;
    resource = *delay_for_next_copy + next_copy->resource;
    current_copy = next_copy;
  }
}

void AsynchronousCopyResource::AddCopy(const AsynchronousCopy& copy) {
  CHECK(
      ConsumeResource(copy.exclusive_start_time, copy.end_time, copy.resource));

  // Find the iterator for the copy that would be right after this copy and put
  // this copy right before it in async_copies_.
  auto async_copy_time_it =
      async_copy_time_map_.upper_bound(copy.exclusive_start_time);
  auto insertion_it = (async_copy_time_it == async_copy_time_map_.end())
                          ? async_copies_.end()
                          : async_copy_time_it->second;
  auto inserted_it = async_copies_.insert(insertion_it, copy);
  // If this copy is the first copy we have seen with the start time, add the
  // inserted iterator into async_copy_time_map_ for fast lookups. Note that
  // async_copy_time_map_ always points to the very first copy with the same
  // start index. If there are multiple asynchronous copies that have the same
  // start time, the memory space assignment algorithm schedules them in the
  // same order that AddCopy was called.
  if (async_copy_time_map_.find(copy.exclusive_start_time) ==
      async_copy_time_map_.end()) {
    async_copy_time_map_[copy.exclusive_start_time] = inserted_it;
  }
}

void AsynchronousCopyResource::RemoveCopy(const AsynchronousCopy& copy) {
  // The ConsumeResource method can only correctly remove the last copy that
  // starts at a given start time. So if the copy that is requested to be
  // removed is not the last copy for this start time, we need to temporarily
  // remove later copies that has the same start time and then add them back one
  // by one. To do this, we first find the iterator that points to the earliest
  // copy after this start time. We then decrement this iterator and temporarily
  // remove the copies until we find the copy we actually want to remove. After
  // we remove the copy that we actually want to remove, we add back the
  // temporarily removed copies one by one in the same order.
  auto async_copy_time_it =
      async_copy_time_map_.upper_bound(copy.exclusive_start_time);
  auto copy_it = (async_copy_time_it == async_copy_time_map_.end())
                     ? async_copies_.end()
                     : async_copy_time_it->second;
  CHECK(copy_it != async_copies_.begin());
  --copy_it;

  std::list<AsynchronousCopy> copies_to_add_back;
  auto prev_copy_it = copy_it;
  for (; *copy_it != copy; copy_it = prev_copy_it) {
    CHECK(copy_it != async_copies_.begin());
    CHECK_EQ(copy_it->exclusive_start_time, copy.exclusive_start_time);
    copies_to_add_back.push_front(*copy_it);
    VLOG(4) << "RemoveCopy found a copy to temporarily remove and add back: "
            << copy_it->exclusive_start_time << " " << copy_it->end_time << " "
            << copy_it->resource;
    prev_copy_it = std::prev(copy_it);
    RemoveCopy(copy_it);
  }
  CHECK(*copy_it == copy);
  RemoveCopy(copy_it);

  for (const AsynchronousCopy& copy_to_add_back : copies_to_add_back) {
    AddCopy(copy_to_add_back);
  }
}

void AsynchronousCopyResource::RemoveCopy(
    std::list<AsynchronousCopy>::iterator& copy_it) {
  // This method works only for the latest copy for the given start time.
  CHECK(std::next(copy_it) == async_copies_.end() ||
        std::next(copy_it)->exclusive_start_time >
            copy_it->exclusive_start_time);
  CHECK(ConsumeResource(copy_it->exclusive_start_time, copy_it->end_time,
                        /*resource=*/0,
                        /*delay_change_map=*/nullptr,
                        /*resource_to_free=*/copy_it->resource));
  // If the copy to be removed is the value pointed by async_copy_time_map_, we
  // make the next copy with the same start time to be pointed by
  // async_copy_time_map_. If there are no such copies, we remove the key for
  // this copy start time.
  int64_t exclusive_start_time = copy_it->exclusive_start_time;
  auto async_copy_time_it = async_copy_time_map_.find(exclusive_start_time);
  if (copy_it == async_copy_time_it->second) {
    if (std::next(copy_it) != async_copies_.end() &&
        std::next(copy_it)->exclusive_start_time == exclusive_start_time) {
      async_copy_time_it->second = std::next(copy_it);
    } else {
      async_copy_time_map_.erase(async_copy_time_it);
    }
  }
  async_copies_.erase(copy_it);
}

bool AsynchronousCopyResource::HasEnoughResource(int64_t exclusive_start_time,
                                                 int64_t end_time,
                                                 float resource) {
  absl::flat_hash_map<int64_t, float> delay_changes;
  bool result =
      ConsumeResource(exclusive_start_time, end_time, resource, &delay_changes);
  for (const auto& change_pair : delay_changes) {
    delay_[change_pair.first] = change_pair.second;
  }
  return result;
}

bool AsynchronousCopyResource::HasEnoughResourceMultiCheck(
    const std::vector<ResourceSpec>& specs) {
  absl::flat_hash_map<int64_t, float> delay_changes;
  bool result = absl::c_all_of(specs, [&](const ResourceSpec& spec) {
    return ConsumeResource(spec.exclusive_start_time, spec.end_time,
                           spec.resource, &delay_changes);
  });
  for (const auto& change_pair : delay_changes) {
    delay_[change_pair.first] = change_pair.second;
  }
  return result;
}

namespace {

// A convenience struct for use in the implementation of
// AsynchronousCopyResource::Dump().
struct CopyResourceDumpData {
  float initial_resource;
  float delay;
  float available;
  std::vector<int> overlapping_copies;
};

}  // namespace

std::string AsynchronousCopyResource::Dump(
    int64_t start_time, int64_t end_time,
    MemorySpaceAssignment::MemorySpace memory_space_filter) const {
  std::vector<float> available = GetCurrentResources();
  std::vector<CopyResourceDumpData> time_dump_data;
  for (int i = start_time; i < end_time; ++i) {
    time_dump_data.push_back({
        initial_resources_[i],
        delay_[i],
        available[i],
        /*overlapping_copies=*/{},
    });
  }

  std::vector<std::string> lines;
  lines.push_back(absl::StrCat("AsynchronousCopyResource::Dump(start_time: ",
                               start_time, ", end_time: ", end_time, ")"));
  for (const AsynchronousCopy& copy : async_copies_) {
    if (copy.destination != memory_space_filter) {
      continue;
    }
    int64_t overlap_start = std::max(start_time, copy.exclusive_start_time);
    int64_t overlap_end = std::min(end_time, copy.end_time);
    if (overlap_start < overlap_end) {
      lines.push_back(absl::StrCat(
          "copy(id: ", copy.id,
          ", exclusive_start: ", copy.exclusive_start_time,
          ", end: ", copy.end_time, ", resource: ", copy.resource, ")"));
    }
    for (int i = overlap_start; i < overlap_end; ++i) {
      time_dump_data[i - start_time].overlapping_copies.push_back(copy.id);
    }
  }

  std::vector<size_t> col_sizes;
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"time", "initial", "delay", "avail", "overlapping copies"});
  for (std::string_view col : rows.front()) {
    col_sizes.push_back(col.size());
  }
  for (int i = 0; i < time_dump_data.size(); ++i) {
    rows.push_back({absl::StrCat(i + start_time),
                    absl::StrCat(time_dump_data[i].initial_resource),
                    absl::StrCat(time_dump_data[i].delay),
                    absl::StrCat(time_dump_data[i].available),
                    absl::StrJoin(time_dump_data[i].overlapping_copies, ",")});
    for (int j = 0; j < rows.back().size(); ++j) {
      col_sizes[j] = std::max(col_sizes[j], rows.back()[j].size());
    }
  }
  for (const std::vector<std::string>& row : rows) {
    std::string line;
    std::string sep;
    for (int i = 0; i < col_sizes.size(); ++i) {
      absl::StrAppend(&line, sep, row[i]);
      sep = std::string(col_sizes[i] + 2 - row[i].size(), ' ');
    }
    lines.push_back(line);
  }

  return absl::StrJoin(lines, "\n");
}

AlternateMemoryBestFitHeap::AliasedOffset*
AlternateMemoryBestFitHeap::GetAliasedOffset(
    const MemorySpaceAssignment::Allocation& allocation) {
  auto aliased_offset_it = aliased_offset_map_.find(&allocation);
  CHECK(aliased_offset_it != aliased_offset_map_.end());
  return aliased_offset_it->second;
}

void AlternateMemoryBestFitHeap::CreateOrAddToAliasedOffset(
    const MemorySpaceAssignment::Allocation& allocation,
    AlternateMemoryBestFitHeap::AliasedOffset* aliased_offset) {
  CHECK(allocation.memory_space() == MemorySpace::kAlternate);
  CHECK(!aliased_offset_map_.contains(&allocation));
  if (!aliased_offset) {
    aliased_offsets_.push_back({allocation.chunk().offset});
    aliased_offset = &aliased_offsets_.back();
  }
  CHECK_EQ(allocation.chunk().offset, aliased_offset->offset);
  CHECK(aliased_offset->allocations.insert(&allocation).second);
  aliased_offset_map_[&allocation] = aliased_offset;
}

/*static*/ MemorySpaceAssignment::Allocation*
AlternateMemoryBestFitHeap::GetLiveAllocationAt(
    const MemorySpaceAssignment::AllocationSequence& allocations,
    int64_t time) {
  for (auto allocation_it = allocations.rbegin();
       allocation_it != allocations.rend(); ++allocation_it) {
    if ((*allocation_it)->start_time() <= time &&
        (*allocation_it)->end_time() >= time) {
      return allocation_it->get();
    }
  }
  return nullptr;
}

void AlternateMemoryBestFitHeap::AllocateCrossProgramPrefetchBuffer(
    HloModule* module, const BufferInterval& prefetch_candidate) {
  Chunk chunk_candidate = FindChunkCandidate(prefetch_candidate);
  if (chunk_candidate.chunk_end() > available_heap_size()) {
    VLOG(3) << "Could not allocate preferred memory for cross program prefetch";
    return;
  }

  const HloValue* buffer = prefetch_candidate.buffer;
  int64_t parameter = buffer->instruction()->parameter_number();
  int cross_program_prefetch_index = module->CrossProgramPrefetches().size();
  module->AddCrossProgramPrefetch(parameter, buffer->index());

  MemorySpaceAssignment::AllocationSequence allocations;
  allocations.push_back(std::make_unique<MemorySpaceAssignment::Allocation>(
      buffer->defining_position(), MemorySpace::kDefault, kDummyChunk,
      prefetch_candidate.start, prefetch_candidate.end,
      /*is_scoped_allocation=*/false));

  // Find the earliest use.
  const auto& instruction_schedule = hlo_live_range_.instruction_schedule();
  auto uses = FindCrossProgramPrefetchUses(buffer->GetUses(), alias_analysis_);
  CHECK_GE(uses.size(), 1);
  auto use_schedule_compare = [&](const HloUse& lhs, const HloUse& rhs) {
    return instruction_schedule.at(lhs.instruction) <
           instruction_schedule.at(rhs.instruction);
  };
  auto first_use = absl::c_min_element(uses, use_schedule_compare);
  int64_t latest_prefetch_time =
      instruction_schedule.at(first_use->instruction);

  // Find the latest use time.
  int64_t last_use_time = instruction_schedule.at(
      absl::c_max_element(uses, use_schedule_compare)->instruction);
  for (const HloValue* colocation : prefetch_candidate.colocations) {
    auto colocation_uses = colocation->GetUses();
    if (!colocation_uses.empty()) {
      last_use_time = std::max(
          last_use_time,
          instruction_schedule.at(
              absl::c_max_element(colocation_uses, use_schedule_compare)
                  ->instruction));
    }
  }

  int64_t end_of_program_prefetch_end_time = instruction_schedule.size();
  int64_t end_of_program_prefetch_latest_start_time =
      options_.prefetch_interval_picker->LatestPrefetchStartTime(
          buffer->defining_position().shape(), last_use_time,
          end_of_program_prefetch_end_time, nullptr);
  int64_t end_of_program_inclusive_prefetch_start_time =
      options_.prefetch_interval_picker->PreferredPrefetchStartTime(
          buffer->defining_position().shape(), last_use_time,
          end_of_program_prefetch_latest_start_time,
          end_of_program_prefetch_end_time);
  VLOG(2) << "last use time = " << last_use_time
          << ", end-of-program inclusive prefetch start time = "
          << end_of_program_inclusive_prefetch_start_time;
  float total_execution_time =
      options_.prefetch_interval_picker->GetLogicalIntervalElapsed(
          0, instruction_schedule.size());
  float buffer_occupied_time =
      options_.prefetch_interval_picker->GetLogicalIntervalElapsed(
          end_of_program_inclusive_prefetch_start_time,
          end_of_program_prefetch_end_time);
  if (options_.cost_analysis) {
    buffer_occupied_time = std::max(buffer_occupied_time,
                                    options_.cost_analysis->GetAsyncCopyElapsed(
                                        buffer->defining_position().shape()));
  }
  buffer_occupied_time +=
      options_.prefetch_interval_picker->GetLogicalIntervalElapsed(
          0, last_use_time);
  float buffer_occupied_ratio = buffer_occupied_time / total_execution_time;
  VLOG(2) << "Total execution time = " << total_execution_time
          << ", buffer occupied time = " << buffer_occupied_time
          << ", buffer occupied ratio = " << buffer_occupied_ratio;
  // Freeing buffer only makes sense if the buffer will be free for a
  // substantial time. Only perform this optimization if the ratio is below the
  // limit, and if the memory pressure is above the alternate memory size.
  bool free_buffer =
      (options_.enable_cross_program_prefetch_freeing &&
       memory_pressure_ > options_.max_size_in_bytes &&
       buffer_occupied_ratio < kCrossProgramPrefetchOccupyFreeingLimit &&
       end_of_program_inclusive_prefetch_start_time > last_use_time &&
       end_of_program_inclusive_prefetch_start_time <
           end_of_program_prefetch_end_time);
  int64_t cross_program_prefetch_end_time =
      free_buffer ? last_use_time : prefetch_candidate.end;

  AddAsyncCopy(*allocations.back(), MemorySpace::kAlternate, chunk_candidate,
               /*exclusive_start_time=*/
               InclusiveToExclusiveStartTime(prefetch_candidate.start),
               cross_program_prefetch_end_time, latest_prefetch_time,
               &allocations, /*aliased_offset=*/nullptr,
               /*resource=*/0.0, cross_program_prefetch_index);

  absl::c_for_each(uses, [&](auto& use) { allocations.back()->AddUse(use); });
  AliasedOffset* cross_program_prefetch_offset =
      GetAliasedOffset(*allocations.back());

  if (free_buffer) {
    VLOG(2) << "Adding an end-of-program prefetch for freed "
               "cross-program-prefetched buffer.";
    AddAsyncCopy(*allocations.front(), MemorySpace::kAlternate, chunk_candidate,
                 /*exclusive_start_time=*/
                 InclusiveToExclusiveStartTime(
                     end_of_program_inclusive_prefetch_start_time),
                 end_of_program_prefetch_end_time,
                 end_of_program_prefetch_end_time, &allocations,
                 cross_program_prefetch_offset,
                 /*resource=*/0.0);
    CHECK_EQ(cross_program_prefetch_offset->offset,
             allocations.back()->chunk().offset);
  }

  const int allocations_initial_size = allocations_->size();
  for (auto& allocation : allocations) {
    if (allocation->memory_space() == MemorySpace::kAlternate) {
      BufferInterval buffer_interval;
      buffer_interval.start = allocation->start_time();
      buffer_interval.end = allocation->end_time();
      buffer_interval.size = allocation->chunk().size;
      buffer_interval.buffer = prefetch_candidate.buffer;
      AddToPendingChunks(buffer_interval, chunk_candidate);
    }
    allocations_->push_back(std::move(allocation));
  }

  // Add a repack allocation block for the Allocation objects in alternate
  // memory.
  std::vector<RepackAllocationBlock*> colocations;
  for (int i = allocations_initial_size; i < allocations_->size(); ++i) {
    const auto& allocation = allocations_->at(i);
    if (allocation->memory_space() == MemorySpace::kAlternate) {
      repack_allocation_blocks_.push_back(MakeRepackAllocationBlock(
          allocation->start_time(), allocation->end_time(),
          allocation->chunk().size, allocation->chunk().offset,
          static_cast<int64_t>(colocations.size()), allocation.get()));
      RepackAllocationBlock* inserted = &repack_allocation_blocks_.back();
      for (RepackAllocationBlock* colocation : colocations) {
        inserted->colocations.push_back(colocation);
        colocation->colocations.push_back(inserted);
      }
      inserted->colocations.emplace_back(inserted);
      colocations.emplace_back(inserted);
    }
  }

  ClearPendingChunks();
}

void AlternateMemoryBestFitHeap::AllocateReservedScopedAllocations() {
  const auto& instruction_sequence =
      hlo_live_range_.flattened_instruction_sequence().instructions();
  std::vector<MemorySpaceAssignmentRepacker::AllocationBlock*> colocations;
  for (int i = 0; i < instruction_sequence.size(); ++i) {
    const HloInstruction* instruction = instruction_sequence[i];
    int64_t reserved_scoped_memory = options_.reserved_scoped_memory_fn(
        instruction, /*operands_in_alternate_memory=*/{},
        /*outputs_in_alternate_memory=*/{});
    if (reserved_scoped_memory != 0) {
      VLOG(1) << "Allocate reserved scoped memory at " << i << " ("
              << instruction->name() << "): " << reserved_scoped_memory;
      MemorySpaceAssignment::BufferInterval interval;
      interval.buffer = nullptr;
      interval.size = reserved_scoped_memory;
      interval.start = i;
      interval.end = i;
      interval.need_allocation = true;
      interval.colocations = {};
      Chunk chunk_candidate =
          FindChunkCandidate(interval, /*preferred_offset=*/0);
      CHECK_EQ(chunk_candidate.offset, 0);
      AddToPendingChunks(interval, chunk_candidate);

      if (options_.dump_fn != nullptr || VLOG_IS_ON(3)) {
        AppendScopedAllocationBufferInfoDebugString(
            instruction, i, reserved_scoped_memory, buffer_info_str_);
      }

      allocations_->push_back(
          std::make_unique<MemorySpaceAssignment::Allocation>(
              HloPosition{instruction_sequence[i], {}}, MemorySpace::kAlternate,
              chunk_candidate, i, i, /*is_scoped_allocation=*/true));

      repack_allocation_blocks_.push_back(MakeRepackAllocationBlock(
          i, i, reserved_scoped_memory,
          /*initial_offset=*/0,
          static_cast<int64_t>(repack_allocation_blocks_.size()),
          allocations_->back().get()));
      colocations.push_back(&repack_allocation_blocks_.back());
    }
  }
  // If requested, make all scoped allocations to colocate with each other so
  // that when we repack, all scoped allocations get the same offsets. Since
  // they will all have the same scoped memory addresses, this increases the
  // opportunity to deduplicate different ops.  However, this may hurt the
  // memory packing efficiency.
  if (options_.allocate_reserved_scoped_memory_at_same_offset) {
    for (MemorySpaceAssignmentRepacker::AllocationBlock* repack_block :
         colocations) {
      repack_block->colocations = colocations;
    }
  } else {
    for (RepackAllocationBlock& allocation_block : repack_allocation_blocks_) {
      allocation_block.colocations.push_back(&allocation_block);
    }
  }
  ClearPendingChunks();
}

std::optional<AlternateMemoryBestFitHeap::RequiredMemoryAssignment>
AlternateMemoryBestFitHeap::RequiredMemoryAssignmentAt(const HloValue* buffer,
                                                       int64_t time) const {
  auto required_assignment_it = required_assignments_.find(buffer);
  std::optional<RequiredMemoryAssignment> required_assignment_at_time;
  if (required_assignment_it != required_assignments_.end()) {
    for (const RequiredMemoryAssignment& required_assignment :
         required_assignment_it->second) {
      if (required_assignment.time == time) {
        // Sanity check that there is only one required at time.
        CHECK(!required_assignment_at_time)
            << buffer->ToShortString() << " at time " << time;
        required_assignment_at_time = required_assignment;
      }
    }
  }
  return required_assignment_at_time;
}

std::optional<AlternateMemoryBestFitHeap::RequiredMemoryAssignment>
AlternateMemoryBestFitHeap::AliasedRequiredAssignmentForUse(
    const AllocationValue::Use& use) const {
  std::optional<RequiredMemoryAssignment> required_assignment;
  for (const HloPosition& position : use.aliases) {
    const HloValue* value =
        &alias_analysis_.dataflow_analysis().GetUniqueValueAt(
            position.instruction, position.index);
    int64_t time =
        hlo_live_range_.instruction_schedule().at(position.instruction);
    std::optional<RequiredMemoryAssignment> required_assignment_for_alias =
        RequiredMemoryAssignmentAt(value, time);
    if (required_assignment == std::nullopt) {
      required_assignment = required_assignment_for_alias;
    } else {
      CHECK(required_assignment_for_alias == std::nullopt ||
            required_assignment->equals_ignoring_time(
                *required_assignment_for_alias));
    }
  }
  return required_assignment;
}

void AlternateMemoryBestFitHeap::AddAliasedRequiredAssignment(
    const HloInstruction* instruction, ShapeIndex index,
    const MemorySpaceAssignment::Allocation* aliased_allocation) {
  AliasedOffset* offset = nullptr;
  if (aliased_allocation->memory_space() == MemorySpace::kAlternate) {
    offset = GetAliasedOffset(*aliased_allocation);
  }
  AddRequiredAssignment(instruction, index, aliased_allocation->memory_space(),
                        offset);
}

void AlternateMemoryBestFitHeap::AddRequiredAssignment(
    const HloValue* value, const HloInstruction* instruction,
    MemorySpaceAssignment::MemorySpace memory_space, int64_t time,
    AliasedOffset* offset, bool add_to_pending) {
  // Check for existing required assignment at this time and make sure it is the
  // same as this if there is one.
  auto existing_required_assignment = RequiredMemoryAssignmentAt(value, time);
  if (existing_required_assignment) {
    CHECK(memory_space == existing_required_assignment->memory_space)
        << "inst = " << instruction->ToString() << " at " << time;
    CHECK((!offset && !existing_required_assignment->offset) ||
          offset == existing_required_assignment->offset);
    VLOG(3) << "Not adding required assignment because there is one already: "
            << value->ToShortString() << " at " << time << " at "
            << (memory_space == MemorySpace::kDefault ? "def" : "alt");
  } else {
    VLOG(3) << "Adding required assignment: " << value->ToShortString()
            << " at " << time << " at "
            << (memory_space == MemorySpace::kDefault ? "def" : "alt");
    RequiredMemoryAssignment required_assignment{memory_space, time, offset};
    required_assignments_[value].push_back(required_assignment);
    if (add_to_pending) {
      pending_required_assignments_.push_back({value, required_assignment});
    }
  }
}

void AlternateMemoryBestFitHeap::AddRequiredAssignment(
    const HloInstruction* instruction, ShapeIndex index,
    MemorySpace memory_space, AliasedOffset* offset, bool add_to_pending) {
  const HloValue* value =
      &alias_analysis_.dataflow_analysis().GetUniqueValueAt(instruction, index);
  int64_t instruction_time =
      hlo_live_range_.instruction_schedule().at(instruction);
  AddRequiredAssignment(value, instruction, memory_space, instruction_time,
                        offset, add_to_pending);
}

void AlternateMemoryBestFitHeap::AddRequiredAssignment(
    const HloPosition& position, MemorySpace memory_space,
    AliasedOffset* offset, bool add_to_pending) {
  AddRequiredAssignment(position.instruction, position.index, memory_space,
                        offset, add_to_pending);
}

void AlternateMemoryBestFitHeap::AddRequiredAssignment(const HloUse& use,
                                                       MemorySpace memory_space,
                                                       AliasedOffset* offset,
                                                       bool add_to_pending) {
  const HloValue* value = &alias_analysis_.dataflow_analysis().GetUniqueValueAt(
      use.instruction->operand(use.operand_number), use.operand_index);
  int64_t instruction_time =
      hlo_live_range_.instruction_schedule().at(use.instruction);
  AddRequiredAssignment(value, use.instruction, memory_space, instruction_time,
                        offset, add_to_pending);
}

void AlternateMemoryBestFitHeap::AddInputAndOutputRequiredAssignments() {
  // Go through the parameters, outputs, and constants and pin them to the
  // corresponding memory by adding a required assignment.
  const HloModule& module = alias_analysis_.dataflow_analysis().module();
  const auto& instruction_schedule = hlo_live_range_.instruction_schedule();
  HloComputation* entry_computation = module.entry_computation();
  for (HloInstruction* parameter_instruction :
       entry_computation->parameter_instructions()) {
    int64_t parameter_instruction_time =
        instruction_schedule.at(parameter_instruction);
    ShapeUtil::ForEachSubshape(
        parameter_instruction->shape(),
        [&](const Shape& subshape, const ShapeIndex& index) {
          MemorySpace memory_space = MemorySpace::kDefault;
          if (subshape.has_layout() && subshape.layout().memory_space() ==
                                           options_.alternate_memory_space) {
            memory_space = MemorySpace::kAlternate;
          }
          for (const HloBuffer* buffer :
               alias_analysis_.ComputeBuffersAt(parameter_instruction, index)) {
            for (const HloValue* value : buffer->values()) {
              VLOG(3) << "Adding required assignment for parameter value = "
                      << value->ToShortString()
                      << " time = " << parameter_instruction_time << " space = "
                      << (memory_space == MemorySpace::kDefault ? "def"
                                                                : "alt");
              AddRequiredAssignment(value, parameter_instruction, memory_space,
                                    parameter_instruction_time,
                                    /*offset=*/nullptr,
                                    /*add_to_pending=*/false);
            }
          }
        });
  }
  HloInstruction* root_instruction = entry_computation->root_instruction();
  int64_t root_instruction_time = instruction_schedule.at(root_instruction);
  ShapeUtil::ForEachSubshape(
      root_instruction->shape(),
      [&](const Shape& subshape, const ShapeIndex& index) {
        MemorySpace memory_space = MemorySpace::kDefault;
        if (subshape.has_layout() && subshape.layout().memory_space() ==
                                         options_.alternate_memory_space) {
          memory_space = MemorySpace::kAlternate;
        }
        for (const HloBuffer* buffer :
             alias_analysis_.ComputeBuffersAt(root_instruction, index)) {
          for (const HloValue* value : buffer->values()) {
            VLOG(3) << "Adding required assignment for output value = "
                    << value->ToShortString()
                    << " time = " << root_instruction_time << " space = "
                    << (memory_space == MemorySpace::kDefault ? "def" : "alt");
            AddRequiredAssignment(value, root_instruction, memory_space,
                                  root_instruction_time,
                                  /*offset=*/nullptr, /*add_to_pending=*/false);
          }
        }
      });

  for (const HloComputation* computation : module.MakeNonfusionComputations()) {
    for (HloInstruction* instruction : computation->instructions()) {
      if (instruction->opcode() == HloOpcode::kConstant) {
        auto constant_instruction_it = instruction_schedule.find(instruction);
        if (constant_instruction_it == instruction_schedule.end()) {
          continue;
        }
        int64_t constant_instruction_time = constant_instruction_it->second;
        for (const auto& indexed_shape :
             ShapeUtil::GetLeafShapes(instruction->shape())) {
          const ShapeIndex& index = indexed_shape.index;
          for (const HloBuffer* buffer :
               alias_analysis_.ComputeBuffersAt(instruction, index)) {
            for (const HloValue* value : buffer->values()) {
              VLOG(3) << "Adding required assignment for constant value = "
                      << value->ToShortString()
                      << " time = " << constant_instruction_time
                      << " space = def";
              AddRequiredAssignment(value, instruction, MemorySpace::kDefault,
                                    constant_instruction_time,
                                    /*offset=*/nullptr,
                                    /*add_to_pending=*/false);
            }
          }
        }
      }
    }
  }

  // Go through all of the values and pin them to the default memory if they are
  // not allowed on the alternate memory.
  for (const HloValue* value : alias_analysis_.dataflow_analysis().values()) {
    if (!options_.is_allowed_in_alternate_mem_fn(*value)) {
      // We won't find the instruction in the schedule if it's inside a fusion.
      // If so, just skip.
      auto instruction_time_it =
          instruction_schedule.find(value->instruction());
      if (instruction_time_it == instruction_schedule.end()) {
        continue;
      }
      int64_t instruction_time = instruction_time_it->second;
      auto& required_assignments = required_assignments_[value];
      // Check if there is an existing matching required assignment (e.g.
      // inserted by the logic above) and if so ensure it requires a default
      // memory allocation.
      auto matching_assignment = absl::c_find_if(
          required_assignments,
          [&](const RequiredMemoryAssignment& required_assignment) {
            return required_assignment.time == instruction_time;
          });
      if (matching_assignment != required_assignments.end()) {
        CHECK(matching_assignment->memory_space == MemorySpace::kDefault)
            << "Mismatch in required assignments at time " << instruction_time
            << " value: " << value->ToString();
      } else {
        required_assignments.push_back(
            {MemorySpace::kDefault, instruction_time});
      }
    }
  }
}

bool AlternateMemoryBestFitHeap::AreIntervalsReservedInAlternateMemory(
    absl::Span<const BufferInterval* const> colocated_intervals) const {
  auto is_position_in_alternate_memory = [&](const HloPosition& position) {
    const Shape& shape = position.shape();
    return shape.has_layout() &&
           shape.layout().memory_space() == options_.alternate_memory_space;
  };

  const HloModule& module = alias_analysis_.dataflow_analysis().module();
  const HloComputation* entry_computation = module.entry_computation();
  const HloInstruction* root_instruction =
      entry_computation->root_instruction();
  for (const BufferInterval* colocated_interval : colocated_intervals) {
    const HloValue* value = colocated_interval->buffer;
    if (value->defining_instruction()->opcode() == HloOpcode::kParameter &&
        value->defining_instruction()->parent() == entry_computation &&
        is_position_in_alternate_memory(value->defining_position())) {
      return true;
    }

    for (const HloPosition& position : value->positions()) {
      if (position.instruction == root_instruction &&
          is_position_in_alternate_memory(position)) {
        return true;
      }
    }
  }
  return false;
}

const std::vector<const HloInstruction*>*
AlternateMemoryBestFitHeap::GetRepeatedInstructionList(
    const HloInstruction* instruction) const {
  const auto fingerprint_it = fingerprint_map_.find(instruction);
  if (fingerprint_it == fingerprint_map_.end()) {
    return nullptr;
  }
  const auto repeated_insts_it =
      repeated_inst_map_.find(fingerprint_it->second);
  CHECK(repeated_insts_it != repeated_inst_map_.end());
  return &repeated_insts_it->second;
}

void AlternateMemoryBestFitHeap::UpdateReservedScopedAllocationSize() {
  // Check all instructions, if their operands/outputs have been placed in
  // alternate memory, update their scoped allocation size.
  VLOG(2) << "Update scoped allocation size before repacking.";
  const auto& instruction_sequence =
      hlo_live_range_.flattened_instruction_sequence().instructions();
  absl::flat_hash_map<int64_t, int64_t> reserved_scoped_memory_map;
  for (int i = 0; i < instruction_sequence.size(); ++i) {
    const HloInstruction* instruction = instruction_sequence[i];
    reserved_scoped_memory_map[i] = options_.reserved_scoped_memory_fn(
        instruction, operands_in_alternate_memory_map_[instruction],
        outputs_in_alternate_memory_map_[instruction]);
  }
  // Update scoped allocation sizes.
  for (RepackAllocationBlock& allocation_block : repack_allocation_blocks_) {
    MemorySpaceAssignment::Allocation* allocation = allocation_block.allocation;
    if (allocation->is_scoped_allocation()) {
      allocation_block.size =
          reserved_scoped_memory_map[allocation->start_time()];
      allocation->mutable_chunk()->size =
          reserved_scoped_memory_map[allocation->start_time()];
    }
  }
}

void AlternateMemoryBestFitHeap::ExportAllocationsForRepacking(
    std::vector<MemorySpaceAssignmentRepacker::AllocationBlock*>& allocations) {
  using SlicedCopyAllocation = MemorySpaceAssignment::SlicedCopyAllocation;
  using SliceDetail = SlicedCopyAllocation::SliceDetail;

  if (options_.reduce_scoped_memory_limit) {
    UpdateReservedScopedAllocationSize();
  }

  for (RepackAllocationBlock& allocation_block : repack_allocation_blocks_) {
    allocation_block.original_slice_data = std::nullopt;
    allocation_block.repacked_slice_data = std::nullopt;

    if (!allocation_block.allocation->is_sliced_copy_allocation()) {
      allocations.push_back(&allocation_block);
      continue;
    }

    SlicedCopyAllocation* allocation =
        dynamic_cast<SlicedCopyAllocation*>(allocation_block.allocation);
    std::vector<const SliceDetail*> slice_details_sorted_by_offset;
    slice_details_sorted_by_offset.reserve(
        allocation->slice_details_sorted_by_start_time().size());
    for (const SliceDetail& slice_detail :
         allocation->slice_details_sorted_by_start_time()) {
      slice_details_sorted_by_offset.push_back(&slice_detail);
    }
    absl::c_stable_sort(slice_details_sorted_by_offset,
                        [](const SliceDetail* lhs, const SliceDetail* rhs) {
                          return lhs->slice_decision.chunk.offset <
                                 rhs->slice_decision.chunk.offset;
                        });

    // Since this is a sliced allocation, construct SlicedAllocationData to
    // attach to the AllocationBlock.
    MemorySpaceAssignmentRepacker::SlicedAllocationData original_slice_data;
    for (const SliceDetail* slice_detail : slice_details_sorted_by_offset) {
      CHECK_EQ(slice_detail->copy_start_after_time,
               slice_detail->slice_decision.exclusive_start_time);
      original_slice_data.slices_sorted_by_offset.push_back(
          MemorySpaceAssignmentRepacker::Slice{
              slice_detail->slice_decision.chunk.size,
              slice_detail->slice_decision.chunk.offset,
              /*inclusive_start_time=*/
              ExclusiveToInclusiveStartTime(
                  slice_detail->slice_decision.exclusive_start_time)});
    }

    allocation_block.original_slice_data = std::move(original_slice_data);
    allocations.push_back(&allocation_block);
  }
}

void AlternateMemoryBestFitHeap::ImportRepackedAllocations() {
  interval_tree_ = {};
  for (RepackAllocationBlock& allocation_block : repack_allocation_blocks_) {
    if (allocation_block.allocation->is_sliced_copy_allocation()) {
      ImportRepackedSlicedAllocation(allocation_block);
      continue;
    }
    ImportRepackedNonSlicedAllocation(allocation_block);
  }
}

void AlternateMemoryBestFitHeap::ImportRepackedNonSlicedAllocation(
    RepackAllocationBlock& block) {
  MemorySpaceAssignment::Allocation* allocation = block.allocation;
  int64_t original_offset = block.initial_offset;
  int64_t repacked_offset = block.offset;

  // Update the Allocation, AllocationBlock, and interval_tree_.
  allocation->set_offset(repacked_offset);
  block.initial_offset = repacked_offset;
  block.offset = -1;
  interval_tree_.Add(
      block.inclusive_start_time, block.end_time,
      HeapSimulator::Chunk::FromOffsetSize(repacked_offset, block.size));

  VLOG(3) << "Repacking move. offset: " << original_offset << " -> "
          << repacked_offset << "; size: " << block.size
          << "; Allocation: " << allocation->ToString();
}

void AlternateMemoryBestFitHeap::ImportRepackedSlicedAllocation(
    RepackAllocationBlock& block) {
  using SlicedCopyAllocation = MemorySpaceAssignment::SlicedCopyAllocation;
  using SliceDetail = SlicedCopyAllocation::SliceDetail;

  SlicedCopyAllocation* allocation =
      dynamic_cast<SlicedCopyAllocation*>(block.allocation);
  CHECK(block.allocation->is_sliced_copy_allocation());
  int64_t original_offset = block.initial_offset;
  int64_t repacked_offset = block.offset;
  std::vector<int64_t> original_slice_offsets =
      allocation->SliceOffsetsSortedByStartTime();

  // Update the Allocation, AllocationBlock, and interval_tree_.
  allocation->set_offset(repacked_offset);
  if (block.repacked_slice_data.has_value()) {
    CHECK(block.original_slice_data.has_value());
    CHECK_EQ(allocation->slice_details_sorted_by_start_time().size(),
             block.repacked_slice_data->slices_sorted_by_offset.size());
    allocation->ImportRepackedSliceData(*block.repacked_slice_data);
  } else {
    allocation->AddDiffToAllSliceOffsets(repacked_offset - original_offset);
  }
  block.initial_offset = repacked_offset;
  block.offset = -1;
  // Note, in a non-repacking setting, we would have reworked the chunks as
  // described in AlternateMemoryBestFitHeap::PrefetchContext::SlicedSolution::
  // slices_for_pending_chunks. Doing so was for the benefit of
  // AlternateMemoryBestFitHeap::pending_chunks_. However, pending_chunks_
  // are cleared before repacking, when UncommitPendingChunks() is called. Thus,
  // we don't need to worry about modifying the chunks here.
  for (const SliceDetail& slice_detail :
       allocation->slice_details_sorted_by_start_time()) {
    interval_tree_.Add(
        /*start=*/
        ExclusiveToInclusiveStartTime(slice_detail.copy_start_after_time),
        block.end_time, slice_detail.slice_decision.chunk);
  }

  VLOG(3) << "Repacking move. offset: " << original_offset << " -> "
          << repacked_offset << "; size: " << block.size << "; " <<
      [&]() {
        std::vector<int64_t> new_slice_offsets =
            allocation->SliceOffsetsSortedByStartTime();
        CHECK_EQ(original_slice_offsets.size(), new_slice_offsets.size());
        std::vector<std::string> offset_moves;
        offset_moves.reserve(original_slice_offsets.size());
        for (int i = 0; i < original_slice_offsets.size(); ++i) {
          offset_moves.push_back(absl::StrCat(original_slice_offsets[i], " -> ",
                                              new_slice_offsets[i]));
        }
        return absl::StrCat("slice_offsets: [",
                            absl::StrJoin(offset_moves, ", "), "]");
      }()
          << "; Allocation: " << allocation->ToString();
}

void AlternateMemoryBestFitHeap::UncommitPendingChunks(
    absl::Span<AllocationValue> allocation_values) {
  // Clear the allocation sequence of the allocation values so that in case we
  // retry allocation after uncommitting.
  for (AllocationValue& allocation_value : allocation_values) {
    allocation_value.mutable_allocation_sequence()->clear();
  }
  for (const auto& interval_and_chunk : pending_chunks_) {
    const BufferInterval& interval = interval_and_chunk.first;
    const Chunk& chunk = interval_and_chunk.second;
    VLOG(3) << "Uncommitting: (" << interval.start << ", " << interval.end
            << ") off = " << chunk.offset << " size = " << chunk.size;
    for (int i = interval.start; i <= interval.end; ++i) {
      peak_memory_usage_[i] -= chunk.size;
      CHECK_GE(peak_memory_usage_[i], 0)
          << "Peak memory usage at " << i
          << " is below zero after uncommitting. " << interval.start << "-"
          << interval.end << " : [" << chunk.offset << ", " << chunk.size
          << "]";
    }
    interval_tree_.Remove(interval.start, interval.end, chunk);
  }
  for (const AsynchronousCopy& async_copy : pending_async_copies_) {
    if (async_copy.destination == MemorySpace::kAlternate) {
      prefetch_interval_tree_.Remove(
          /*start=*/
          ExclusiveToInclusiveStartTime(async_copy.exclusive_start_time),
          async_copy.end_time, kDummyChunk);
      prefetch_async_copy_resource_.RemoveCopy(async_copy);
      if (options_.enforce_prefetch_fifo_order) {
        async_copy_ordering_.RemoveCopy(async_copy);
      }
    } else {
      eviction_interval_tree_.Remove(
          /*start=*/
          ExclusiveToInclusiveStartTime(async_copy.exclusive_start_time),
          async_copy.end_time, kDummyChunk);
      eviction_async_copy_resource_.RemoveCopy(async_copy);
    }
  }
  for (const auto& value_and_required_assignment :
       pending_required_assignments_) {
    auto& required_assignment_vector =
        required_assignments_[value_and_required_assignment.first];
    const RequiredMemoryAssignment& required_assignment =
        value_and_required_assignment.second;
    VLOG(3) << "Removing required assignment: "
            << (required_assignment.memory_space == MemorySpace::kDefault
                    ? "def"
                    : "alt")
            << " time = " << required_assignment.time << " off = "
            << (required_assignment.offset ? required_assignment.offset->offset
                                           : -1);
    for (auto it = required_assignment_vector.begin();
         it != required_assignment_vector.end(); ++it) {
      if (*it == value_and_required_assignment.second) {
        required_assignment_vector.erase(it);
        break;
      }
    }
  }
  ClearPendingChunks();
}

void AlternateMemoryBestFitHeap::FinalizeAllocations(
    absl::Span<AllocationValue> allocation_values) {
  absl::flat_hash_map<const AliasedOffset*,
                      std::vector<MemorySpaceAssignment::Allocation*>>
      colocation_map;
  for (AllocationValue& allocation_value : allocation_values) {
    for (auto& allocation : *allocation_value.mutable_allocation_sequence()) {
      if ((allocation->memory_space() == MemorySpace::kAlternate) &&
          (!allocation->is_scoped_allocation())) {
        for (const HloUse& use : allocation->uses()) {
          operands_in_alternate_memory_map_[use.instruction].insert(
              std::make_pair(use.operand_number, use.operand_index));
        }
        if (!allocation->is_copy_like_allocation()) {
          outputs_in_alternate_memory_map_[allocation->defining_position()
                                               .instruction]
              .insert(allocation->defining_position().index);
        }
      }
      allocations_->push_back(std::move(allocation));
      MemorySpaceAssignment::Allocation* inserted_allocation =
          allocations_->back().get();
      if (inserted_allocation->memory_space() == MemorySpace::kAlternate) {
        colocation_map[GetAliasedOffset(*inserted_allocation)].push_back(
            inserted_allocation);
      }
    }
  }
  // The allocations that have the same AliasedOffset need to be colocated.
  // Export these to repack_allocation_blocks_ so that we can repack them to
  // reduce fragmentation.
  for (auto& colocation : colocation_map) {
    std::vector<MemorySpaceAssignmentRepacker::AllocationBlock*> colocations;
    for (MemorySpaceAssignment::Allocation* colocated_allocation :
         colocation.second) {
      repack_allocation_blocks_.push_back(MakeRepackAllocationBlock(
          colocated_allocation->start_time(), colocated_allocation->end_time(),
          colocated_allocation->chunk().size,
          colocated_allocation->chunk().offset,
          static_cast<int64_t>(repack_allocation_blocks_.size()),
          colocated_allocation));
      colocations.push_back(&repack_allocation_blocks_.back());
    }
    for (MemorySpaceAssignmentRepacker::AllocationBlock* repack_block :
         colocations) {
      repack_block->colocations = colocations;
    }
  }
  ClearPendingChunks();
}

void AlternateMemoryBestFitHeap::ClearPendingChunks() {
  pending_chunks_.clear();
  pending_async_copies_.clear();
  pending_required_assignments_.clear();
  aliased_offset_map_.clear();
  aliased_offsets_.clear();
}

void AlternateMemoryBestFitHeap::AddToPendingChunks(
    const BufferInterval& buffer_interval, const Chunk& chunk_candidate) {
  VLOG(3) << "Committing chunk: " << buffer_interval.start << "-"
          << buffer_interval.end << " : [" << chunk_candidate.offset << ", "
          << chunk_candidate.size << "]";
  pending_chunks_.emplace_back(buffer_interval, chunk_candidate);
  for (int i = buffer_interval.start; i <= buffer_interval.end; ++i) {
    peak_memory_usage_[i] += chunk_candidate.size;
    CHECK_LE(peak_memory_usage_[i], options_.max_size_in_bytes)
        << "Peak memory usage at " << i
        << " exceeds the max size of alternate memory. "
        << buffer_interval.start << "-" << buffer_interval.end << " : ["
        << chunk_candidate.offset << ", " << chunk_candidate.size << "]";
  }
  CommitChunk(buffer_interval, chunk_candidate);
}

std::optional<int>
AlternateMemoryBestFitHeap::FindEarliestExclusiveTimeToSatisfyPeakMemory(
    int exclusive_start_time, int end_time, int64_t size) const {
  std::optional<int> earliest_time_exclusive = std::nullopt;
  for (int time_inclusive = ExclusiveToInclusiveEndTime(end_time);
       time_inclusive > exclusive_start_time; --time_inclusive) {
    if (peak_memory_usage_[time_inclusive] + size <=
        options_.max_size_in_bytes) {
      earliest_time_exclusive = InclusiveToExclusiveStartTime(time_inclusive);
    } else {
      break;
    }
  }

  return earliest_time_exclusive;
}

AlternateMemoryBestFitHeap::Result AlternateMemoryBestFitHeap::AllocateSegment(
    const AllocationRequest& request) {
  auto allocation_sequence =
      request.allocation_value->mutable_allocation_sequence();
  // inclusive_start_time == end_time is a special case where the value is
  // consumed multiple times by the same instruction. We can just find the
  // previous allocation and use that allocation.
  if (request.inclusive_start_time == request.end_time) {
    MemorySpaceAssignment::Allocation* allocation =
        GetLiveAllocationAt(*allocation_sequence, request.end_time);
    CHECK_NE(allocation, nullptr);
    allocation->AddUse(request.use->hlo_use);
    return Result::kSuccess;
  }

  const HloPosition& defining_position =
      request.allocation_value->defining_position();
  VLOG(2) << "Finding allocation for "
          << request.allocation_value->ToShortString() << " ["
          << request.inclusive_start_time << ", " << request.end_time
          << ") latest prefetch = " << request.latest_prefetch_time
          << " last use = " << request.allocation_value->uses().back().time
          << " use = " << request.use->hlo_use.ToString()
          << ". Size = " << request.size
          << ", def pos = " << defining_position.ToString();
  CHECK_LE(request.inclusive_start_time, request.end_time);
  if (VLOG_IS_ON(3) && options_.cost_analysis) {
    const HloPosition& defining_position =
        request.allocation_value->defining_position();
    const HloUse& use = request.use->hlo_use;
    VLOG(3) << "Definition benefit = "
            << options_.cost_analysis->GetAlternateMemoryBenefit(
                   request.allocation_value->defining_position())
            << " use benefit = "
            << options_.cost_analysis->GetAlternateMemoryBenefit(
                   request.use->hlo_use);
    VLOG(3) << "Definition bytes accessed = "
            << options_.cost_analysis->cost_analysis().output_bytes_accessed(
                   *defining_position.instruction, defining_position.index)
            << ", use bytes accessed = "
            << options_.cost_analysis->cost_analysis().operand_bytes_accessed(
                   *use.instruction, use.operand_number, use.operand_index);
  }

  // There could be a requirement to pin this buffer to default memory either
  // because it is a parameter or an output.  If the buffer is a parameter, then
  // we're allowed to prefetch. If the use expects the output to be in default
  // memory, we cannot prefetch it because if we did, it would be in alternate
  // memory instead.
  auto required_assignment_at_start = RequiredMemoryAssignmentAt(
      request.allocation_value->value(), request.inclusive_start_time);
  std::optional<MemorySpace> required_memory_space_at_start;
  if (required_assignment_at_start) {
    required_memory_space_at_start = required_assignment_at_start->memory_space;
  }
  // Find required assignment both for the use and its aliases. If they are both
  // non-nullopt, then make sure they require the same assignment.
  auto required_assignment_at_end = RequiredMemoryAssignmentAt(
      request.allocation_value->value(), request.end_time);
  auto aliased_required_assignment_at_end =
      AliasedRequiredAssignmentForUse(*request.use);
  if (required_assignment_at_end != aliased_required_assignment_at_end) {
    if (required_assignment_at_end == std::nullopt) {
      required_assignment_at_end = aliased_required_assignment_at_end;
    } else {
      CHECK(aliased_required_assignment_at_end == std::nullopt ||
            aliased_required_assignment_at_end->equals_ignoring_time(
                *required_assignment_at_end));
    }
  }
  std::optional<MemorySpace> required_memory_space_at_end;
  if (required_assignment_at_end) {
    required_memory_space_at_end = required_assignment_at_end->memory_space;
  }

  if (required_assignment_at_start) {
    bool needs_required_allocation = true;
    if (!allocation_sequence->empty()) {
      auto prev_allocation_it = std::find_if(
          allocation_sequence->rbegin(), allocation_sequence->rend(),
          [&](const auto& allocation) {
            return allocation->memory_space() == required_memory_space_at_start;
          });
      if (prev_allocation_it != allocation_sequence->rend()) {
        (*prev_allocation_it)->set_end_time(request.inclusive_start_time);
        needs_required_allocation = false;
      }
    }
    if (needs_required_allocation) {
      std::optional<Chunk> aliased_chunk = std::nullopt;
      if (required_assignment_at_start->memory_space ==
          MemorySpace::kAlternate) {
        aliased_chunk = Chunk::FromOffsetSize(
            required_assignment_at_start->offset->offset, request.size);
      }
      allocation_sequence->push_back(
          std::make_unique<MemorySpaceAssignment::Allocation>(
              defining_position, required_assignment_at_start->memory_space,
              aliased_chunk, request.inclusive_start_time,
              request.inclusive_start_time,
              /*is_scoped_allocation=*/false));
      if (required_assignment_at_start->memory_space ==
          MemorySpace::kAlternate) {
        CreateOrAddToAliasedOffset(*allocation_sequence->back(),
                                   required_assignment_at_start->offset);
      }
    }
  }

  Result allocation_result = Result::kSuccess;
  // First try keeping the allocation entirely in the alternate memory.
  if (required_memory_space_at_start != MemorySpace::kDefault &&
      required_memory_space_at_end != MemorySpace::kDefault &&
      request.allow_no_copy_alternate_mem_allocation) {
    allocation_result = AllocateInAlternateMemoryNoCopy(request);
    if (allocation_result == Result::kSuccess) {
      return Result::kSuccess;
    }
  }

  auto prev_allocation_it = allocation_sequence->rbegin();
  // Find a previous allocation that is in the default memory space (not
  // necessarily the very last allocation).
  auto prev_allocation_in_default_mem_it =
      std::find_if(allocation_sequence->rbegin(), allocation_sequence->rend(),
                   [&](const auto& allocation) {
                     return allocation->memory_space() == MemorySpace::kDefault;
                   });

  if (prev_allocation_in_default_mem_it == allocation_sequence->rend() &&
      prev_allocation_it != allocation_sequence->rend() &&
      (*prev_allocation_it)->memory_space() == MemorySpace::kAlternate &&
      (*prev_allocation_it)->defining_position() == defining_position &&
      !request.allocation_value->requires_contiguous_allocation()) {
    // If there was an allocation for this HloValue that was in the alternate
    // memory space, we also need to perform an eviction.
    Result eviction_result = Evict(request);
    if (eviction_result != Result::kSuccess) {
      // A non-success eviction requires us to uncommit previous allocations.
      return result_mark(Result::kFailRequiresUncommit, eviction_result);
    }
    prev_allocation_in_default_mem_it = allocation_sequence->rbegin();
  } else if (prev_allocation_in_default_mem_it == allocation_sequence->rend()) {
    allocation_sequence->push_back(
        std::make_unique<MemorySpaceAssignment::Allocation>(
            defining_position, MemorySpace::kDefault,
            /*chunk=*/std::nullopt, request.inclusive_start_time,
            request.end_time,
            /*is_scoped_allocation=*/false));
    prev_allocation_in_default_mem_it = allocation_sequence->rbegin();
  }

  CHECK(prev_allocation_in_default_mem_it != allocation_sequence->rend());
  CHECK((*prev_allocation_in_default_mem_it)->memory_space() ==
        MemorySpace::kDefault);

  // If the allocation value requires a contiguous allocation but has a memory
  // space mismatch between the start and end required assignments, then we need
  // to uncommit.
  if (request.allocation_value->requires_contiguous_allocation() &&
      required_memory_space_at_start.has_value() &&
      required_memory_space_at_end.has_value() &&
      required_memory_space_at_start != required_memory_space_at_end) {
    VLOG(3) << "Allocation requires contiguous allocation but has memory space "
               "mismatch.";
    return result_mark(Result::kFailRequiresUncommit, allocation_result);
  }

  // If the buffer must be in default memory at the end_time, don't prefetch.
  if (required_memory_space_at_end == MemorySpace::kDefault) {
    VLOG(3)
        << "Not trying to prefetch because use requires buffer in default mem.";
    (*prev_allocation_in_default_mem_it)->set_end_time(request.end_time);
    (*prev_allocation_in_default_mem_it)->AddUse(request.use->hlo_use);
    return Result::kSuccess;
  }

  // Finally, try to prefetch the buffer into alternate memory.
  if (request.allow_prefetch &&
      !request.allocation_value->requires_contiguous_allocation()) {
    Result prefetch_result =
        Prefetch(request, **prev_allocation_in_default_mem_it);
    if (prefetch_result == Result::kSuccess) {
      if (request.preferred_prefetch_time) {
        // Warn if the prefetch time picked doesn't match the preferred prefetch
        // time.
        CHECK(!request.allocation_value->allocation_sequence()->empty());
        const MemorySpaceAssignment::Allocation* allocation =
            request.allocation_value->allocation_sequence()->back().get();
        int64_t prefetch_time = 0;
        if (allocation->is_copy_allocation()) {
          prefetch_time =
              static_cast<const MemorySpaceAssignment::CopyAllocation*>(
                  allocation)
                  ->copy_start_schedule_after();
        } else if (allocation->is_sliced_copy_allocation()) {
          prefetch_time =
              static_cast<const MemorySpaceAssignment::SlicedCopyAllocation*>(
                  allocation)
                  ->slice_details_sorted_by_start_time()
                  .front()
                  .copy_start_after_time;
        } else {
          LOG(FATAL) << "Prefetch allocation are expected to be "
                        "CopyAllocations or SlicedCopyAllocations.";
        }
        if (prefetch_time != *request.preferred_prefetch_time) {
          LOG(WARNING) << "Scheduled prefetch time (" << prefetch_time
                       << ") doesn't match the preferred prefetch time ("
                       << *request.preferred_prefetch_time
                       << "): " << request.use->hlo_use.ToString();
        }
      }
      return Result::kSuccess;
    }
    // Warn if there was a preferred prefetch time but we couldn't actually
    // prefetch.
    if (request.preferred_prefetch_time) {
      LOG(WARNING) << "The request has a preferred prefetch time ("
                   << *request.preferred_prefetch_time
                   << ") which could not be satisfied: "
                   << request.use->hlo_use.ToString();
    }
    result_mark(prefetch_result, allocation_result);
  }

  // If the end assignment was required to be in alternate memory but that
  // wasn't possible, then this allocation is invalid.
  if (required_memory_space_at_end == MemorySpace::kAlternate) {
    return result_mark(Result::kFailRequiresUncommit, allocation_result);
  }

  // If the start assignment was required to be in alternate memory and the
  // buffer needs a contiguous assignment, we couldn't satisfy this requirement
  // and must abort.
  if (required_memory_space_at_start == MemorySpace::kAlternate &&
      request.allocation_value->requires_contiguous_allocation()) {
    return result_mark(Result::kFailRequiresUncommit, allocation_result);
  }

  // If a copy wasn't inserted, then add this use to the latest allocation in
  // default memory.
  (*prev_allocation_in_default_mem_it)->set_end_time(request.end_time);
  (*prev_allocation_in_default_mem_it)->AddUse(request.use->hlo_use);
  return allocation_result;
}

void AlternateMemoryBestFitHeap::AddAsyncCopy(
    MemorySpaceAssignment::Allocation& prev_allocation,
    MemorySpace memory_space, std::optional<Chunk> chunk,
    int64_t exclusive_start_time, int64_t end_time,
    int64_t copy_done_schedule_before_time,
    MemorySpaceAssignment::AllocationSequence* allocations,
    AliasedOffset* aliased_offset, float resource,
    std::optional<int> cross_program_prefetch_index) {
  VLOG(3) << "Copy to "
          << (memory_space == MemorySpaceAssignment::MemorySpace::kDefault
                  ? "default"
                  : "alternate")
          << " memory in (" << exclusive_start_time << ", "
          << copy_done_schedule_before_time << "), keeping until " << end_time
          << ", estimated copy resource is " << resource;
  CHECK_LT(exclusive_start_time, copy_done_schedule_before_time);

  allocations->push_back(
      std::make_unique<MemorySpaceAssignment::CopyAllocation>(
          prev_allocation, memory_space, chunk, exclusive_start_time, end_time,
          copy_done_schedule_before_time, cross_program_prefetch_index));

  // Register the additional async copy with the interval tree to keep track of
  // the limit at any given time.
  pending_async_copies_.push_back({exclusive_start_time,
                                   copy_done_schedule_before_time, resource,
                                   memory_space, next_async_copy_id_++});
  if (memory_space == MemorySpaceAssignment::MemorySpace::kAlternate) {
    prefetch_interval_tree_.Add(
        /*start=*/
        ExclusiveToInclusiveStartTime(exclusive_start_time),
        copy_done_schedule_before_time, kDummyChunk);
    prefetch_async_copy_resource_.AddCopy(pending_async_copies_.back());
    if (options_.enforce_prefetch_fifo_order) {
      async_copy_ordering_.AddCopy(pending_async_copies_.back());
    }
    CreateOrAddToAliasedOffset(*allocations->back(), aliased_offset);
  } else {
    eviction_interval_tree_.Add(
        /*start=*/
        ExclusiveToInclusiveStartTime(exclusive_start_time),
        copy_done_schedule_before_time, kDummyChunk);
    eviction_async_copy_resource_.AddCopy(pending_async_copies_.back());
  }
}

namespace {

// Computes a string that can be used for logging/debugging. For each slice, the
// string includes:
// - When the slice starts
// - When the slice copy must complete
// - When the allocation for the slice ends
// - An estimation of how much copy resource the slice consumes
std::string SliceTimesAndCopyResourcesToString(
    const std::vector<MemorySpaceAssignment::SliceDecision>& slice_decisions,
    int64_t prefetch_end, int64_t allocation_end) {
  std::vector<std::string> slice_strings;
  slice_strings.reserve(slice_decisions.size());

  for (const auto& slice_decision : slice_decisions) {
    std::vector<std::string> details;
    details.push_back(absl::StrCat(slice_decision.exclusive_start_time));
    details.push_back(absl::StrCat(prefetch_end));
    details.push_back(absl::StrCat(allocation_end));
    details.push_back(absl::StrCat(slice_decision.copy_resource_consumed));

    slice_strings.push_back(
        absl::StrCat("(", absl::StrJoin(details, ", "), ")"));
  }

  return absl::StrCat(
      "Slices(copy_start_time, copy_done_by_time, allocation_end, "
      "estimated_copy_resource) = [",
      absl::StrJoin(slice_strings, ", "), "]");
}

}  // namespace

void AlternateMemoryBestFitHeap::AddAsyncSlicesForPrefetch(
    const MemorySpaceAssignment::Allocation& prev_allocation,
    MemorySpaceAssignment::AllocationSequence* allocations,
    AliasedOffset* aliased_offset,
    const std::vector<MemorySpaceAssignment::SliceDecision>&
        slice_decisions_sorted_by_start_time,
    int64_t prefetch_end_time, int64_t allocation_end_time) {
  VLOG(3) << "Sliced copy to alternate memory. "
          << SliceTimesAndCopyResourcesToString(
                 slice_decisions_sorted_by_start_time, prefetch_end_time,
                 allocation_end_time);
  CHECK(absl::c_all_of(
      slice_decisions_sorted_by_start_time, [&](const auto& slice_decision) {
        return slice_decision.exclusive_start_time < prefetch_end_time;
      }));

  allocations->push_back(
      std::make_unique<MemorySpaceAssignment::SlicedCopyAllocation>(
          prev_allocation, MemorySpaceAssignment::MemorySpace::kAlternate,
          slice_decisions_sorted_by_start_time, allocation_end_time,
          prefetch_end_time, options_.update_layout_fn));

  // Register the additional async copy with the interval tree to keep track of
  // the limit at any given time.
  for (const auto& slice_decision : slice_decisions_sorted_by_start_time) {
    pending_async_copies_.push_back(
        {slice_decision.exclusive_start_time, prefetch_end_time,
         slice_decision.copy_resource_consumed,
         MemorySpaceAssignment::MemorySpace::kAlternate,
         next_async_copy_id_++});
    prefetch_interval_tree_.Add(slice_decision.exclusive_start_time,
                                prefetch_end_time, kDummyChunk);
    prefetch_async_copy_resource_.AddCopy(pending_async_copies_.back());
    if (options_.enforce_prefetch_fifo_order) {
      async_copy_ordering_.AddCopy(pending_async_copies_.back());
    }
  }
  CreateOrAddToAliasedOffset(*allocations->back(), aliased_offset);
}

bool AlternateMemoryBestFitHeap::ViolatesMaximumOutstandingAsyncCopies(
    int64_t inclusive_start_time, int64_t end_time, bool is_prefetch,
    int64_t extra_async_copy_limit, int64_t num_additional_copies) const {
  if (options_.max_outstanding_prefetches < 0 && is_prefetch) {
    return false;
  }
  if (options_.max_outstanding_evictions < 0 && !is_prefetch) {
    return false;
  }

  // Count the prefetches/evictions in the interval tree for the given interval.
  if (is_prefetch) {
    int64_t num_prefetches =
        prefetch_interval_tree_
            .ChunksOverlappingInTime(inclusive_start_time, end_time)
            .size() +
        num_additional_copies;
    return num_prefetches >=
           options_.max_outstanding_prefetches + extra_async_copy_limit;
  } else {
    int64_t num_evictions =
        eviction_interval_tree_
            .ChunksOverlappingInTime(inclusive_start_time, end_time)
            .size() +
        num_additional_copies;
    return num_evictions >=
           options_.max_outstanding_evictions + extra_async_copy_limit;
  }
}

AlternateMemoryBestFitHeap::Result
AlternateMemoryBestFitHeap::AllocateInAlternateMemoryNoCopy(
    const AllocationRequest& request) {
  MemorySpaceAssignment::Allocation* prev_allocation = nullptr;
  bool can_eliminate_copy = false;
  if (request.allocation_value->allocation_sequence()->empty()) {
    // There hasn't been any allocations for this interval so far. We can
    // eliminate copy if the value can be placed in the alternate memory.
    can_eliminate_copy = options_.is_allowed_in_alternate_mem_fn(
        *request.allocation_value->value());
  } else {
    // If there has been a previous allocation, we can eliminate the copy if the
    // previous allocation was also in the alternate memory.
    prev_allocation =
        request.allocation_value->allocation_sequence()->back().get();
    can_eliminate_copy =
        (prev_allocation->memory_space() == MemorySpace::kAlternate);
  }

  if (!can_eliminate_copy) {
    VLOG(3) << "Can't eliminate copy.";
    return Result::kFailPrevAllocationNotInAlternateMem;
  }

  const HloPosition& defining_position =
      request.allocation_value->defining_position();
  // If prefer_no_copy_alternate_mem_allocation is true, bypass the live range
  // duration checks.
  if (!request.prefer_no_copy_alternate_mem_allocation &&
      !options_.prefetch_interval_picker->CanAllocateInAlternateMemoryNoCopy(
          defining_position.shape(), request.inclusive_start_time,
          request.end_time)) {
    VLOG(3) << "Live range is too long.";
    return Result::kFailLiveRangeTooLong;
  }

  BufferInterval alternate_mem_interval;
  alternate_mem_interval.buffer = request.allocation_value->value();
  alternate_mem_interval.size = request.size;
  alternate_mem_interval.end = request.end_time;
  alternate_mem_interval.start = request.inclusive_start_time;

  // Prefer the offset that was previously used for the previous allocation.
  AliasedOffset* preferred_offset = nullptr;
  if (prev_allocation != nullptr) {
    preferred_offset = GetAliasedOffset(*prev_allocation);
    // If there is a previous allocation, set the start time one after the end
    // of the previous allocation's end.
    alternate_mem_interval.start = prev_allocation->end_time() + 1;
  }

  if (request.preferred_offset) {
    // Sanity check that if there is a preferred offset provided in the request,
    // it matches with the previous allocation.
    CHECK(!preferred_offset || request.preferred_offset == preferred_offset)
        << "preferred_offset = " << preferred_offset->offset
        << ", request.preferred_offset = " << request.preferred_offset->offset;
    preferred_offset = request.preferred_offset;
  }

  VLOG(3) << "We can eliminate copy to alternate memory. Preferred offset = "
          << (preferred_offset ? preferred_offset->offset : -1);
  // In case there are additional uses after this use, we rely on the last use
  // time to try to reserve a chunk in the heap simulator. This is to prevent
  // the following scenario:
  //
  //                            +-------+
  //                           /         \
  //                   Producer--->Use1   +-->Use2
  //                       +---------+---------+
  // New buffer:           |         |         |
  //                       +---------+---------+
  //
  //                                     +-----------+
  // Current heap:                       | offset: 0 |
  //           --------------------------+-----------+------
  //
  // Because we allocate buffers greedily, Producer to Use1 segment first, and
  // then Use1 to Use2 segment, it is possible to allocate the first segment at
  // an offset that is available for the first segment (e.g. offset 0) but not
  // for the entire live range. This can result in unnecessary copies. By using
  // the last use time, we try to find an allocation that is available for the
  // entire Producer to Use2 range.
  std::optional<Chunk> chunk_candidate = FindBestChunkCandidate(
      request, preferred_offset, &alternate_mem_interval);
  // Check if the new heap size fits within limits. Also ensure if a
  // preferred offset was provided, that offset was used.
  if (chunk_candidate) {
    VLOG(3) << "Keep the buffer in alternate memory. Offset = "
            << chunk_candidate->offset << ", size = " << chunk_candidate->size
            << ", heap_size = " << result_.UpdatedHeapSize(*chunk_candidate)
            << ", prefetch picker = "
            << options_.prefetch_interval_picker->ToNoCopyDebugString(
                   defining_position.shape(),
                   /*start_time=*/
                   InclusiveToExclusiveStartTime(request.inclusive_start_time),
                   request.end_time);
    AddToPendingChunks(alternate_mem_interval, *chunk_candidate);

    // If there was a previous allocation, the buffer location is the
    // same as the previous. Otherwise, it is the operand.
    if (prev_allocation != nullptr &&
        (prev_allocation->is_copy_like_allocation() ||
         prev_allocation->defining_position() == defining_position)) {
      prev_allocation->set_end_time(request.end_time);
    } else {
      request.allocation_value->mutable_allocation_sequence()->push_back(
          std::make_unique<MemorySpaceAssignment::Allocation>(
              defining_position, MemorySpace::kAlternate, chunk_candidate,
              request.inclusive_start_time, request.end_time,
              /*is_scoped_allocation=*/false));
      CreateOrAddToAliasedOffset(
          *request.allocation_value->allocation_sequence()->back(),
          preferred_offset);
    }
    request.allocation_value->allocation_sequence()->back()->AddUse(
        request.use->hlo_use);
    return Result::kSuccess;
  }
  if (request.prefer_no_copy_alternate_mem_allocation) {
    LOG(WARNING) << "Preferred no-copy allocation, but this was not possible: "
                 << request.use->hlo_use.ToString();
  }
  return Result::kFailOutOfMemory;
}

AlternateMemoryBestFitHeap::Result AlternateMemoryBestFitHeap::Evict(
    const AllocationRequest& request) {
  CHECK_GT(request.allocation_value->allocation_sequence()->size(), 0);
  MemorySpaceAssignment::Allocation* prev_allocation =
      request.allocation_value->allocation_sequence()->back().get();
  // TODO(b/306478911): prev_allocation can never be a prefetch, or we would be
  // using an incorrect start time (we would need to wait until the copies
  // finish)

  // The previous allocation's inclusive start time is the eviction's exclusive
  // start time to ensure that the value is created before we start copying
  // back to default memory.
  int64_t eviction_exclusive_start_time = prev_allocation->start_time();
  int64_t eviction_end_time = prev_allocation->end_time();
  CHECK(eviction_exclusive_start_time <= eviction_end_time);

  int64_t preferred_eviction_end_time =
      std::max(options_.prefetch_interval_picker->PreferredEvictionEndTime(
                   request.allocation_value->defining_position().shape(),
                   eviction_exclusive_start_time, request.end_time),
               eviction_end_time);
  // Evictions must complete by the time of this use.
  preferred_eviction_end_time =
      std::min(preferred_eviction_end_time, request.latest_prefetch_time);

  BufferInterval eviction_mem_interval;
  eviction_mem_interval.buffer = request.allocation_value->value();
  eviction_mem_interval.size = request.size;
  // Try to reserve a buffer from the end of the previous allocation to the
  // preferred eviction end time.
  eviction_mem_interval.start = eviction_end_time + 1;
  eviction_mem_interval.end = preferred_eviction_end_time;
  int64_t preferred_offset = prev_allocation->chunk().offset;
  VLOG(3) << "Eviction (" << eviction_exclusive_start_time << ", "
          << eviction_end_time
          << ") preferred end time = " << eviction_mem_interval.end;

  for (; eviction_mem_interval.end > eviction_end_time;
       --eviction_mem_interval.end) {
    Chunk chunk_candidate =
        FindChunkCandidate(eviction_mem_interval, preferred_offset);
    if (chunk_candidate.offset == preferred_offset) {
      AddToPendingChunks(eviction_mem_interval, chunk_candidate);
      break;
    }
  }
  eviction_end_time = eviction_mem_interval.end;

  VLOG(3) << "Evicting buffer at " << prev_allocation->chunk().offset << " ("
          << eviction_exclusive_start_time << ", " << eviction_end_time << ")";

  float eviction_resource =
      options_.cost_analysis
          ? options_.cost_analysis->GetAsyncCopyElapsed(
                request.allocation_value->defining_position().shape())
          : 0.1;

  bool eviction_interval_too_short =
      (eviction_exclusive_start_time == eviction_end_time);
  bool eviction_violates_resource =
      !eviction_async_copy_resource_.HasEnoughResource(
          eviction_exclusive_start_time, eviction_end_time, eviction_resource);
  if (eviction_violates_resource) {
    // If we're in the last retry, set resource to 0.
    if (options_.prefetch_interval_picker->retry_number() ==
        options_.max_retries - 1) {
      VLOG(3) << "Violates resource in last retry, setting resource = 0";
      eviction_resource = 0;
    }
    eviction_violates_resource =
        !eviction_async_copy_resource_.HasEnoughResource(
            eviction_exclusive_start_time, eviction_end_time,
            eviction_resource);
  }
  bool eviction_violates_outstanding_copies =
      ViolatesMaximumOutstandingAsyncCopies(
          /*inclusive_start_time=*/ExclusiveToInclusiveStartTime(
              eviction_exclusive_start_time),
          eviction_end_time,
          /*is_prefetch=*/false);

  // See if this interval would violate the asynchronous copy limit.
  if (!eviction_interval_too_short && !eviction_violates_outstanding_copies &&
      !eviction_violates_resource) {
    prev_allocation->set_end_time(eviction_end_time);
    AddAsyncCopy(*prev_allocation, MemorySpace::kDefault,
                 /*chunk=*/std::nullopt, eviction_exclusive_start_time,
                 prev_allocation->end_time(), eviction_end_time,
                 request.allocation_value->mutable_allocation_sequence(),
                 /*aliased_offset=*/nullptr, eviction_resource);
  } else {
    if (eviction_violates_outstanding_copies) {
      VLOG(3) << "This violates the maximum async copies.";
    } else if (eviction_violates_resource) {
      VLOG(3) << "This violates resource.";
    } else {
      VLOG(3) << "Eviction interval is too short ("
              << eviction_exclusive_start_time << ", " << eviction_end_time
              << ").";
    }
    // If the original interval violated the limit, try sub-intervals within
    // this interval.
    bool eviction_scheduled = false;

    if (!eviction_scheduled) {
      // If the eviction couldn't be scheduled, then fail. This buffer will be
      // kept in the default memory.
      VLOG(3) << "Bailing: Could not evict " << request.use->hlo_use.ToString()
              << " because we hit the limit of maximum asynchronous copies "
              << "between ("
              << hlo_live_range_.flattened_instruction_sequence()
                     .instructions()[eviction_exclusive_start_time]
              << ", "
              << hlo_live_range_.flattened_instruction_sequence()
                     .instructions()[eviction_end_time]
              << ")";
      return Result::kFailOutOfAsyncCopies;
    }
  }
  return Result::kSuccess;
}

int64_t AlternateMemoryBestFitHeap::FindPrefetchEndTime(
    const AllocationRequest& request, int64_t earliest_prefetch_time) const {
  return request.latest_prefetch_time;
}

namespace {

// A debugging/logging method for describing a sliced solution.
std::string DescribeSlicedBufferMove(
    const std::vector<MemorySpaceAssignment::SliceDecision>& slice_decisions,
    const AlternateMemoryBestFitHeap::HeapResult& heap_result,
    const AlternateMemoryBestFitHeap::Chunk& full_chunk,
    absl::string_view prefetch_picker_debug_string) {
  std::vector<std::string> slice_strings;
  slice_strings.reserve(slice_decisions.size());

  for (const auto& slice_decision : slice_decisions) {
    slice_strings.push_back(absl::StrCat(
        "(", slice_decision.exclusive_start_time, ", ",
        slice_decision.chunk.offset, ", ", slice_decision.chunk.size, ")"));
  }

  return absl::StrCat(
      "Moving buffer to alternate memory in slices. Slices(start_time, offset, "
      "size) = [",
      absl::StrJoin(slice_strings, ", "),
      "]. Heap size = ", heap_result.UpdatedHeapSize(full_chunk),
      ". Prefetch picker = ", prefetch_picker_debug_string);
}

}  // namespace

AlternateMemoryBestFitHeap::Result AlternateMemoryBestFitHeap::Prefetch(
    const AllocationRequest& request,
    MemorySpaceAssignment::Allocation& prev_allocation_in_default_mem) {
  // Try partially placing the buffer in the alternate space. The time that is
  // overlapped will be used to asynchronously copy the buffer from the
  // default memory to the alternate memory.
  //
  //                      start                 end
  //                      time                  time
  //                      X---------------------X
  // Alternate:                          +------+
  // Default:             +---------------------+
  //                                     ^      ^
  //                                   Copy    Copy
  //                                   Start   Done

  VLOG(5) << "Considering prefetch of "
          << request.allocation_value->defining_instruction()->ToString()
          << (request.preferred_offset
                  ? absl::StrCat(", with a preferred offset of ",
                                 request.preferred_offset->offset, ".")
                  : "");
  PrefetchContext context;
  context.request = &request;
  context.prev_allocation_in_default_mem = &prev_allocation_in_default_mem;

  // Create a SliceProposal and WorkingIntervals.
  SetupPrefetchWorkingIntervalsAndSliceProposal(context);

  // Compute some additional preliminaries
  Result init_result = InitializePrefetchIntervalPicker(context);
  if (init_result != Result::kSuccess) {
    return init_result;
  }
  Result check_result = EnsureSomeSpatialPrefetchFitExists(context);
  if (check_result != Result::kSuccess) {
    return check_result;
  }
  const HloUse& use = request.use->hlo_use;
  context.full_shape = &ShapeUtil::GetSubshape(
      use.instruction->operand(use.operand_number)->shape(), use.operand_index);
  // While uses might be allowed to have additional outstanding prefetches.
  context.extra_async_copy_limit =
      use.instruction->opcode() == HloOpcode::kWhile
          ? options_.while_use_extra_outstanding_prefetch_limit
          : 0;

  // Loop over potential prefetch starting times. At the selected start time, we
  // check if we have enough resources and memory for a sliced version of the
  // request and a non-sliced version of the request. We return the first sliced
  // solution that we find. We fallback to the first unsliced solution we find,
  // if we are unable to find a sliced solution.
  Result result = Result::kSuccess;
  while (!options_.prefetch_interval_picker->Done()) {
    // Get the prefetch start time from the interval picker.
    context.exclusive_prefetch_start_time =
        options_.prefetch_interval_picker->Next();
    CHECK_LT(context.exclusive_prefetch_start_time, context.prefetch_end_time);
    if (context.exclusive_out_of_mem_start.has_value() &&
        context.exclusive_prefetch_start_time <=
            *context.exclusive_out_of_mem_start) {
      VLOG(4) << "This would OOM (cached).";
      return Result::kFailOutOfMemory;
    }

    if (context.slice_proposal_collection) {
      VLOG(5) << "Trying sliced solution.";
      // Check if a sliced solution fits.
      Result sliced_result =
          CheckPrefetchFit(/*for_sliced_solution=*/true, context);
      if (sliced_result == Result::kSuccess) {
        // Break out of the loop and use the sliced solution.
        CHECK(context.sliced_solution);
        break;
      } else if (sliced_result != Result::kAllSlicesHaveTheSameStartTime) {
        result_mark(sliced_result, result);
      }
    }

    // If we don't already have an unsliced solution, check the current fit.
    if (!context.unsliced_solution) {
      VLOG(5) << "Trying unsliced solution.";
      Result unsliced_result =
          CheckPrefetchFit(/*for_sliced_solution=*/false, context);
      if (unsliced_result != Result::kSuccess) {
        result_mark(unsliced_result, result);
      } else if (!context.slice_proposal_collection) {
        // We found an unsliced solution and there is no slice proposal, so
        // break out of the loop and use the unsliced solution.
        CHECK(context.unsliced_solution);
        break;
      }
    }
  }

  // Check if we found any solutions.
  if (context.sliced_solution) {
    CHECK(!context.sliced_solution->slices_for_pending_chunks.empty());
    VLOG(3) << DescribeSlicedBufferMove(
        context.sliced_solution->slice_decisions_sorted_by_start_time, result_,
        context.sliced_solution->slices_for_pending_chunks.back().second,
        context.sliced_solution->prefetch_picker_debug_string);

    for (const auto& interval_chunk_pair :
         context.sliced_solution->slices_for_pending_chunks) {
      AddToPendingChunks(interval_chunk_pair.first, interval_chunk_pair.second);
    }
    AddAsyncSlicesForPrefetch(
        *context.prev_allocation_in_default_mem,
        context.request->allocation_value->mutable_allocation_sequence(),
        context.request->preferred_offset,
        context.sliced_solution->slice_decisions_sorted_by_start_time,
        context.prefetch_end_time, context.request->end_time);
    context.request->allocation_value->allocation_sequence()->back()->AddUse(
        context.request->use->hlo_use);
    return Result::kSuccess;
  }
  if (context.unsliced_solution) {
    VLOG(3) << "Move the buffer to alternate memory after time "
            << InclusiveToExclusiveStartTime(
                   context.unsliced_solution_intervals.full.start)
            << ". Offset = "
            << context.unsliced_solution->chunk_candidate.offset
            << ", size = " << context.unsliced_solution->chunk_candidate.size
            << ", heap_size = "
            << result_.UpdatedHeapSize(
                   context.unsliced_solution->chunk_candidate)
            << ", prefetch picker = "
            << context.unsliced_solution->prefetch_picker_debug_string;
    AddToPendingChunks(context.unsliced_solution_intervals.full,
                       context.unsliced_solution->chunk_candidate);
    AddAsyncCopy(
        *context.prev_allocation_in_default_mem, MemorySpace::kAlternate,
        context.unsliced_solution->chunk_candidate,
        context.unsliced_solution_intervals.full.start - 1,
        context.request->end_time, context.prefetch_end_time,
        context.request->allocation_value->mutable_allocation_sequence(),
        context.request->preferred_offset,
        context.unsliced_solution->prefetch_resource);

    request.allocation_value->allocation_sequence()->back()->AddUse(
        request.use->hlo_use);
    return Result::kSuccess;
  }

  // If we didn't consider any prefetch intervals, then the live range was too
  // short.
  return (result == Result::kSuccess ? Result::kFailLiveRangeTooShort : result);
}

void AlternateMemoryBestFitHeap::GenerateSliceProposal(
    PrefetchContext& context) const {
  if (options_.sliced_prefetch_options.max_slices() < 2) {
    return;
  }
  auto log_prefix = [&]() {
    return absl::StrCat(
        "Slice request(options = ",
        options_.sliced_prefetch_options.ShortDebugString(), "; shape = ",
        context.prev_allocation_in_default_mem->defining_position()
            .shape()
            .ToString(),
        ")");
  };

  if (context.request->size < options_.sliced_prefetch_options.min_bytes()) {
    VLOG(5) << "Not slicing " << log_prefix() << " because the request size "
            << context.request->size
            << " is smaller than the min configured size of "
            << options_.sliced_prefetch_options.min_bytes();
    return;
  }

  auto status_or_proposal = options_.propose_slice_fn(
      context.prev_allocation_in_default_mem->defining_position().shape(),
      options_.sliced_prefetch_options);
  if (!status_or_proposal.ok()) {
    VLOG(2) << log_prefix() << " failed: " << status_or_proposal.status();
    return;
  }

  if (status_or_proposal.value().size() < 2) {
    VLOG(2) << log_prefix() << ". No slices proposed.";
    return;
  }

  VLOG(6) << log_prefix() << ". Slice proposal = ["
          << absl::StrJoin(
                 status_or_proposal.value(), ", ",
                 [](std::string* out,
                    const MemorySpaceAssignment::SliceProposal& proposal) {
                   absl::StrAppend(out, proposal.ToString());
                 })
          << "]";

  context.slice_proposal_collection = std::move(status_or_proposal.value());
}

void AlternateMemoryBestFitHeap::SetupPrefetchWorkingIntervalsAndSliceProposal(
    PrefetchContext& context) const {
  // Setup the full WorkingIntervals for the sliced and unsliced solutions.
  // Future code will adjust the start and end times.
  context.sliced_solution_intervals.full = BufferInterval{
      context.request->allocation_value->value(),
      /*size=*/context.request->size,
      /*start=*/-1,
      /*end=*/context.request->end_time,
      /*colocations=*/{},
      /*need_allocation=*/true,
  };
  context.unsliced_solution_intervals.full =
      context.sliced_solution_intervals.full;

  // Attempt to generate a slice proposal.
  GenerateSliceProposal(context);

  // Setup the full SlicedBufferIntervals for the sliced and unsliced solutions.
  // If there is no slice proposal, we will not try a sliced solution. In such a
  // case, we do not populate context.sliced_solution_intervals.
  if (context.slice_proposal_collection) {
    context.sliced_solution_intervals.sliced =
        std::make_unique<SlicedBufferInterval>(
            SlicedBufferInterval::CreateMutableInterval(
                context.sliced_solution_intervals.full));
    std::vector<int64_t> sizes;
    sizes.reserve(context.slice_proposal_collection->size());
    for (const MemorySpaceAssignment::SliceProposal& single_slice_proposal :
         *context.slice_proposal_collection) {
      sizes.push_back(single_slice_proposal.slice_size);
    }
    context.sliced_solution_intervals.sliced->Slice(sizes);
  }
  context.unsliced_solution_intervals.sliced =
      std::make_unique<SlicedBufferInterval>(
          SlicedBufferInterval::CreateMutableInterval(
              context.unsliced_solution_intervals.full));
}

AlternateMemoryBestFitHeap::Result
AlternateMemoryBestFitHeap::InitializePrefetchIntervalPicker(
    PrefetchContext& context) {
  int64_t earliest_exclusive_prefetch_time =
      context.prev_allocation_in_default_mem->earliest_available_time();
  if (context.request->earliest_prefetch_time) {
    earliest_exclusive_prefetch_time =
        std::max(earliest_exclusive_prefetch_time,
                 *context.request->earliest_prefetch_time);
  }
  context.prefetch_end_time =
      FindPrefetchEndTime(*context.request, earliest_exclusive_prefetch_time);

  // As a compile time optimization, use the peak memory usage to filter out
  // allocation times that would push us to OOM.
  std::optional<int> earliest_exclusive_non_oom_prefetch_time =
      FindEarliestExclusiveTimeToSatisfyPeakMemory(
          earliest_exclusive_prefetch_time, context.prefetch_end_time,
          context.request->size);
  if (!earliest_exclusive_non_oom_prefetch_time) {
    VLOG(3) << "Any prefetch in range (" << earliest_exclusive_prefetch_time
            << ", " << context.prefetch_end_time << ") for size "
            << context.request->size << " would go out of memory.";
    return Result::kFailOutOfMemory;
  }
  if (!context.slice_proposal_collection) {
    // We can only perform this optimization if we are not slicing.
    // earliest_non_oom_prefetch_time lets us know the first time the entire
    // buffer will fit, but we may be able to start slices before that time. So,
    // we leave earliest_prefetch_time at its initial value.
    VLOG(4) << "After peak memory check, prefetch range is ("
            << *earliest_exclusive_non_oom_prefetch_time << ", "
            << context.prefetch_end_time
            << "). Original earliest prefetch time is "
            << earliest_exclusive_prefetch_time;
    earliest_exclusive_prefetch_time =
        *earliest_exclusive_non_oom_prefetch_time;
  }
  std::optional<int64_t> preferred_prefetch_time =
      context.request->preferred_prefetch_time;
  if (preferred_prefetch_time) {
    preferred_prefetch_time =
        std::max(*preferred_prefetch_time, earliest_exclusive_prefetch_time);
  }
  options_.prefetch_interval_picker->Begin(
      context.request->use->hlo_use, earliest_exclusive_prefetch_time,
      context.prefetch_end_time, preferred_prefetch_time);
  VLOG(3) << "Trying prefetch picker = "
          << options_.prefetch_interval_picker->ToDebugString();

  return Result::kSuccess;
}

AlternateMemoryBestFitHeap::Result
AlternateMemoryBestFitHeap::EnsureSomeSpatialPrefetchFitExists(
    PrefetchContext& context) const {
  SlicedBufferInterval* interval =
      (context.slice_proposal_collection
           ? context.sliced_solution_intervals.sliced.get()
           : context.unsliced_solution_intervals.sliced.get());

  // Note, UpdateInclusiveSliceStartTimes() will correctly update start times
  // for both sliced and unsliced solutions.
  interval->UpdateExclusiveSliceStartTimes(
      std::vector<int64_t>(interval->num_slices(),
                           options_.prefetch_interval_picker->latest_time()));
  std::vector<Chunk> chunk_candidates = FindBestChunkCandidates(
      *context.request, context.request->preferred_offset, interval);
  if (chunk_candidates.empty()) {
    VLOG(3) << "The latest prefetch (" << interval->full_buffer_interval().start
            << ", " << context.request->end_time
            << ") cannot find valid chunks. Giving up.";
    return Result::kFailOutOfMemory;
  }

  return Result::kSuccess;
}

namespace {

// GetAsyncCopyElapsed with a default value.
float CopyResourceForShape(const Options& options, const Shape& shape) {
  return options.cost_analysis
             ? options.cost_analysis->GetAsyncCopyElapsed(shape)
             : 0.1;
}

// Returns the copy resources needed for the specified slice proposal
// collection, in descending order.
std::vector<float> GetCopyResourcesSortedDescending(
    const Options& options,
    const MemorySpaceAssignment::SliceProposalCollection&
        slice_proposal_collection) {
  std::vector<float> copy_resources;
  copy_resources.reserve(slice_proposal_collection.size());
  for (const MemorySpaceAssignment::SliceProposal& proposal :
       slice_proposal_collection) {
    copy_resources.push_back(
        CopyResourceForShape(options, proposal.slice_shape));
  }
  absl::c_sort(copy_resources);
  return copy_resources;
}

// Returns true if we would have enough async copy resources to copy each
// specified slice.
bool DoWeHaveEnoughCopyResource(
    const std::vector<int64_t>& slice_start_times, int64_t prefetch_end_time,
    const std::vector<float>& copy_resource_per_slice,
    AsynchronousCopyResource& async_copy_resource) {
  CHECK_EQ(slice_start_times.size(), copy_resource_per_slice.size());

  std::vector<AsynchronousCopyResource::ResourceSpec> specs;
  specs.reserve(slice_start_times.size());

  // Note, the HasEnoughResourceMultiCheck() below is sensitive to this order.
  // The specs must be in slice start time order because that's the order
  // they'll be added to prefetch_async_copy_resource_ in
  // AddAsyncSlicesForPrefetch(), if the solution is selected.
  static const float kSlicedCopyResourceInflation = 1.8;
  for (int i = 0; i < slice_start_times.size(); ++i) {
    float original_copy_resource = copy_resource_per_slice[i];
    float new_copy_resource = original_copy_resource;
    if (slice_start_times.size() > 1) {
      // This is a hack that makes us more conservative about using sliced
      // prefetching vs unsliced prefetching.
      new_copy_resource = original_copy_resource * kSlicedCopyResourceInflation;
      VLOG(5)
          << "Inflating required copy resources DoWeHaveEnoughCopyResource() "
             "slice check from "
          << original_copy_resource << " to " << new_copy_resource;
    }
    specs.push_back(
        {slice_start_times[i], prefetch_end_time, new_copy_resource});
  }

  auto specs_to_string = [&specs]() {
    return absl::StrCat(
        "[ ",
        absl::StrJoin(specs, ", ",
                      [](std::string* out,
                         const AsynchronousCopyResource::ResourceSpec& spec) {
                        absl::StrAppend(out, "{exclusive start: ",
                                        spec.exclusive_start_time,
                                        ", end: ", spec.end_time,
                                        ", resource: ", spec.resource, "}");
                      }),
        " ]");
  };

  VLOG(5) << "Checking for enough copy resources for: " << specs_to_string();
  if (!async_copy_resource.HasEnoughResourceMultiCheck(specs)) {
    VLOG(4) << "Not enough copy resources for " << specs_to_string();
    return false;
  }
  return true;
}

// We compute a map from indices in chunk_candidates to indices in a
// SliceProposalCollection. Since the indices of chunk_candidates correspond to
// slice start times order, and SliceProposalCollections are always sorted in
// offset order, the mapping allows us to get the sizing details of a slice at a
// specific slice time.
absl::flat_hash_map<int64_t, int64_t> GetCandidateToProposalIndexMap(
    const std::vector<AlternateMemoryBestFitHeap::Chunk>& chunk_candidates) {
  std::vector<std::pair<int64_t, int64_t>> sorted_offset_candidate_index_pairs;
  sorted_offset_candidate_index_pairs.reserve(chunk_candidates.size());
  for (int64_t chunk_candidate_index = 0;
       chunk_candidate_index < chunk_candidates.size();
       ++chunk_candidate_index) {
    sorted_offset_candidate_index_pairs.push_back(std::make_pair(
        chunk_candidates[chunk_candidate_index].offset, chunk_candidate_index));
  }
  absl::c_sort(sorted_offset_candidate_index_pairs);

  absl::flat_hash_map<int64_t, int64_t> candidate_to_proposal_index_map;
  for (int64_t offset_index = 0;
       offset_index < sorted_offset_candidate_index_pairs.size();
       ++offset_index) {
    int64_t chunk_candidate_index =
        sorted_offset_candidate_index_pairs[offset_index].second;
    candidate_to_proposal_index_map[chunk_candidate_index] = offset_index;
  }

  return candidate_to_proposal_index_map;
}

}  // namespace

AlternateMemoryBestFitHeap::Result AlternateMemoryBestFitHeap::CheckPrefetchFit(
    bool for_sliced_solution, PrefetchContext& context) {
  SlicedBufferInterval* sliced_buffer_interval =
      context.GetMutableWorkingIntervals(for_sliced_solution).sliced.get();

  if (for_sliced_solution) {
    CHECK(context.slice_proposal_collection);
    CHECK_EQ(context.slice_proposal_collection->size(),
             sliced_buffer_interval->num_slices());
  }

  // Update the prefetch start time in our working solution.
  std::vector<int64_t> exclusive_slice_start_times = PickSliceStartTimes(
      sliced_buffer_interval->num_slices(),
      context.exclusive_prefetch_start_time, context.prefetch_end_time);
  CHECK_EQ(sliced_buffer_interval->num_slices(),
           exclusive_slice_start_times.size());
  sliced_buffer_interval->UpdateExclusiveSliceStartTimes(
      exclusive_slice_start_times);
  VLOG(4) << AlternateMemoryAllocationAttemptToString(for_sliced_solution,
                                                      context);

  // Check if all slices have the same start time. If so, we might as well
  // resort to a full copy.
  if (for_sliced_solution &&
      absl::c_all_of(
          exclusive_slice_start_times, [&](int64_t slice_start_time) {
            return slice_start_time == exclusive_slice_start_times.front();
          })) {
    return Result::kAllSlicesHaveTheSameStartTime;
  }

  // Check that we have enough copy resource for the prefetching.
  std::vector<float> copy_resource_per_slice_sorted_by_start_time;
  // If there is a preferred prefetch time due to a loop optimized allocation,
  // we already keep track of the prefetch resources there, so skip tracking
  // resources here.
  if (context.request->preferred_prefetch_time) {
    copy_resource_per_slice_sorted_by_start_time =
        std::vector<float>(exclusive_slice_start_times.size(), 0.0);
  } else if (for_sliced_solution) {
    // In a sliced setting, we don't yet know when each slice will be
    // prefetched. Given the proposed slice times, the most conservative copy
    // resource check we can make is to assume that larger slices are started
    // at earlier times, i.e., they have more time to complete. That is the
    // check we will make here. Once, we've decided when each slice will be
    // prefetched, we can do an exact check below.
    //
    // We start by computing the amount of copy resources needed for each slice,
    // if larger slices are started at earlier times.
    copy_resource_per_slice_sorted_by_start_time =
        GetCopyResourcesSortedDescending(options_,
                                         *context.slice_proposal_collection);
  } else {
    copy_resource_per_slice_sorted_by_start_time.push_back(
        CopyResourceForShape(options_, *context.full_shape));
  }
  CHECK_EQ(sliced_buffer_interval->num_slices(),
           copy_resource_per_slice_sorted_by_start_time.size());

  if (!DoWeHaveEnoughCopyResource(exclusive_slice_start_times,
                                  context.prefetch_end_time,
                                  copy_resource_per_slice_sorted_by_start_time,
                                  prefetch_async_copy_resource_)) {
    return Result::kFailViolatesAsyncCopyResource;
  }

  // Check if the copies we would add for the prefetch would violate copy
  // ordering.
  if (options_.enforce_prefetch_fifo_order &&
      absl::c_any_of(exclusive_slice_start_times,
                     [&](int64_t slice_start_time) {
                       return async_copy_ordering_.ViolatesOrdering(
                           slice_start_time, context.prefetch_end_time);
                     })) {
    VLOG(4) << "This would violate asynchronous copy ordering.";
    return Result::kFailViolatesAsyncCopyResource;
  }

  // Check if the copies we would add for the prefetch violate the maximum
  // number of outstanding async copies.
  for (int i = 0; i < exclusive_slice_start_times.size(); ++i) {
    if (ViolatesMaximumOutstandingAsyncCopies(
            exclusive_slice_start_times[i], context.prefetch_end_time,
            /*is_prefetch=*/true, context.extra_async_copy_limit, i)) {
      VLOG(4) << "This would violate the outstanding async copy limit.";
      return Result::kFailOutOfAsyncCopies;
    }
  }

  // Check if we can find a place in alternate memory for the prefetch.
  std::vector<Chunk> chunk_candidates = FindBestChunkCandidates(
      *context.request, context.request->preferred_offset,
      sliced_buffer_interval);
  CHECK(chunk_candidates.empty() ||
        chunk_candidates.size() == sliced_buffer_interval->num_slices());
  std::string prefetch_picker_debug_string;
  if (VLOG_IS_ON(4)) {
    prefetch_picker_debug_string =
        options_.prefetch_interval_picker->ToDebugString();
  }
  if (for_sliced_solution && !chunk_candidates.empty()) {
    // We're trying a sliced solution. So, if FindBestChunkCandidates() found a
    // solution, each slice should have its own chunk candidate.
    CHECK_EQ(chunk_candidates.size(), sliced_buffer_interval->num_slices());
    // We need a mapping from chunks in chunk_candidates to slice proposals in
    // context.slice_proposal_context.
    absl::flat_hash_map<int64_t, int64_t> candidate_to_proposal_index_map =
        GetCandidateToProposalIndexMap(chunk_candidates);

    // Create slice decisions, sorted by time.
    std::vector<MemorySpaceAssignment::SliceDecision>
        slice_decisions_sorted_by_start_time;
    for (int64_t slice_time = 0;
         slice_time < sliced_buffer_interval->num_slices(); ++slice_time) {
      const MemorySpaceAssignment::SliceProposal& proposal =
          context.slice_proposal_collection->at(
              candidate_to_proposal_index_map[slice_time]);
      copy_resource_per_slice_sorted_by_start_time[slice_time] =
          CopyResourceForShape(options_, proposal.slice_shape);
      slice_decisions_sorted_by_start_time.push_back(
          MemorySpaceAssignment::SliceDecision{
              chunk_candidates[slice_time],
              exclusive_slice_start_times[slice_time], proposal,
              copy_resource_per_slice_sorted_by_start_time[slice_time]});
    }

    // Check that we have enough copy resources for all the slice decisions.
    if (!DoWeHaveEnoughCopyResource(
            exclusive_slice_start_times, context.prefetch_end_time,
            copy_resource_per_slice_sorted_by_start_time,
            prefetch_async_copy_resource_)) {
      return Result::kFailViolatesAsyncCopyResource;
    }

    // Construct BufferInterval-Chunk pairs that are appropriate for pending
    // chunks, as described in PrefetchContext::SlicedSolution.
    std::vector<std::pair<BufferInterval, Chunk>> slices_for_pending_chunks;
    slices_for_pending_chunks.reserve(sliced_buffer_interval->num_slices());
    Chunk final_chunk = Chunk::FromOffsetSize(
        absl::c_min_element(
            chunk_candidates,
            [](const Chunk& a, const Chunk& b) { return a.offset < b.offset; })
            ->offset,
        absl::c_accumulate(
            chunk_candidates, 0,
            [](int64_t sum, const Chunk& chunk) { return sum + chunk.size; }));
    BufferInterval final_buffer_interval{
        context.request->allocation_value->value(),
        /*size=*/final_chunk.size,
        /*start=*/
        ExclusiveToInclusiveStartTime(exclusive_slice_start_times.back()),
        /*end=*/context.request->end_time,
        /*colocations=*/
        sliced_buffer_interval->full_buffer_interval().colocations,
        /*need_allocation=*/true};
    for (int64_t slice_time = 0;
         slice_time < sliced_buffer_interval->num_slices(); ++slice_time) {
      const Chunk& chunk = chunk_candidates[slice_time];
      int64_t inclusive_start_time = ExclusiveToInclusiveStartTime(
          exclusive_slice_start_times[slice_time]);
      if (inclusive_start_time ==
          ExclusiveToInclusiveStartTime(exclusive_slice_start_times.back())) {
        // This and the following chunks will be merged into the final chunk.
        // Note, it's possible for more than one slice to start at the same
        // time.
        break;
      }
      CHECK_LT(inclusive_start_time, ExclusiveToInclusiveStartTime(
                                         exclusive_slice_start_times.back()));
      slices_for_pending_chunks.push_back(std::make_pair(
          BufferInterval{
              context.request->allocation_value->value(),
              /*size=*/chunk.size,
              /*start=*/inclusive_start_time,
              /*end=*/exclusive_slice_start_times.back(),
              // We only use the final_buffer_interval for colocations because
              // slices start at different offsets, and the colocation
              // infrastructure expects all colocated buffers to start at the
              // same offset.
              /*colocations=*/{},
              /*need_allocation=*/true,
          },
          chunk));
    }
    slices_for_pending_chunks.push_back(
        std::make_pair(final_buffer_interval, final_chunk));

    context.sliced_solution = PrefetchContext::SlicedSolution{
        std::move(slice_decisions_sorted_by_start_time),
        std::move(slices_for_pending_chunks),
        prefetch_picker_debug_string,
    };
    return Result::kSuccess;
  } else if (!chunk_candidates.empty()) {
    // We're trying an unsliced solution. So, if FindBestChunkCandidates() found
    // a solution, there must be only 1 chunk for it.
    CHECK_EQ(chunk_candidates.size(), 1);
    CHECK_EQ(copy_resource_per_slice_sorted_by_start_time.size(), 1);
    context.unsliced_solution = PrefetchContext::UnslicedSolution{
        chunk_candidates.front(),
        copy_resource_per_slice_sorted_by_start_time.front(),
        prefetch_picker_debug_string,
    };
    return Result::kSuccess;
  }

  // Mark the out of memory start with the prefetch start time so that we don't
  // explore prefetch start times earlier than this point. If a sliced prefetch
  // doesn't fit at a given time, an unsliced prefetch will not fit either.
  // Thus, if we are considering a sliced prefetch for the current request,
  // we can only update out_of_mem_start when we check with slices.
  if (for_sliced_solution || !context.slice_proposal_collection) {
    CHECK_GT(exclusive_slice_start_times.size(), 0);
    context.exclusive_out_of_mem_start = std::max(
        context.exclusive_out_of_mem_start ? *context.exclusive_out_of_mem_start
                                           : -1,
        exclusive_slice_start_times.front());
  }

  VLOG(4) << "Out of memory.";
  return Result::kFailOutOfMemory;
}

std::vector<int64_t> AlternateMemoryBestFitHeap::PickSliceStartTimes(
    int64_t num_slices, int64_t prefetch_start_time,
    int64_t prefetch_end_time) const {
  CHECK_LE(prefetch_start_time, prefetch_end_time);
  VLOG(5) << "Picking slice start times. num_slices = " << num_slices
          << "; prefetch_start_time = " << prefetch_start_time
          << "; prefetch_end_time = " << prefetch_end_time;

  // Prefetching starts after the selected start instruction and ends
  // before the selected end instruction. Thus, we have (end - (start + 1)) HLO
  // instructions worth of time to perform all of the sliced copies. So, the
  // only choices for start times that give us time to copy are <=
  // prefetch_end_time - 2.
  if (prefetch_start_time >= prefetch_end_time - 2 || num_slices == 1) {
    return std::vector<int64_t>(num_slices, prefetch_start_time);
  }

  float total_elapsed =
      options_.prefetch_interval_picker->GetLogicalIntervalElapsed(
          prefetch_start_time, prefetch_end_time);
  if (total_elapsed <= 0.0) {
    return std::vector<int64_t>(num_slices, prefetch_start_time);
  }

  CHECK_LE(prefetch_start_time, prefetch_end_time - 2);
  std::vector<int64_t> reverse_start_times;
  reverse_start_times.reserve(num_slices);
  for (int64_t candidate_start_time = prefetch_end_time - 2;
       reverse_start_times.size() < num_slices &&
       candidate_start_time >= prefetch_start_time;
       --candidate_start_time) {
    if (candidate_start_time == prefetch_start_time) {
      while (reverse_start_times.size() < num_slices) {
        // This is the last good start time, so use it for all remaining slices.
        reverse_start_times.push_back(candidate_start_time);
      }
      break;
    }
    float used = options_.prefetch_interval_picker->GetLogicalIntervalElapsed(
        candidate_start_time, prefetch_end_time);
    CHECK_GE(used, 0.0) << used << " real time elapses in logical interval ("
                        << candidate_start_time << ", " << prefetch_end_time
                        << "). Expected something >= 0.0.";
    CHECK_LE(used, total_elapsed);
    auto compute_target_fraction =
        [num_slices](const std::vector<int64_t>& reverse_start_times) -> float {
      return (static_cast<float>(reverse_start_times.size()) + 1.0f) /
             static_cast<float>(num_slices);
    };
    while (used >=
           compute_target_fraction(reverse_start_times) * total_elapsed) {
      CHECK_LE(reverse_start_times.size(), num_slices)
          << "Num slices = " << num_slices
          << "; Prefetch start = " << prefetch_start_time
          << "; Slice candidate time = " << candidate_start_time
          << "; Prefetch end = " << prefetch_end_time
          << "; Total elapsed = " << total_elapsed << "; Used = " << used
          << "; Target fraction = "
          << compute_target_fraction(reverse_start_times);
      reverse_start_times.push_back(candidate_start_time);
    }
  }

  CHECK_EQ(reverse_start_times.size(), num_slices);
  absl::c_reverse(reverse_start_times);
  return reverse_start_times;
}

std::string
AlternateMemoryBestFitHeap::AlternateMemoryAllocationAttemptToString(
    bool for_sliced_solution, const PrefetchContext& context) const {
  const SlicedBufferInterval* sliced_buffer_interval =
      context.GetWorkingIntervals(for_sliced_solution).sliced.get();

  std::vector<std::string> slice_times;
  std::vector<int64_t> estimated_slice_prefetch_end_times;

  for (int i = 0; i < sliced_buffer_interval->num_slices(); ++i) {
    slice_times.push_back(absl::StrCat(
        "(", sliced_buffer_interval->IntervalForMakeFreeChunks(i).start, ", ",
        sliced_buffer_interval->full_buffer_interval().end, ")"));
    if (context.slice_proposal_collection) {
      estimated_slice_prefetch_end_times.push_back(
          options_.prefetch_interval_picker->EstimatedPrefetchEndTime(
              context.slice_proposal_collection->at(i).slice_shape,
              sliced_buffer_interval->IntervalForMakeFreeChunks(i).start,
              context.prefetch_end_time));
    } else {
      estimated_slice_prefetch_end_times.push_back(
          options_.prefetch_interval_picker->EstimatedPrefetchEndTime(
              *context.full_shape,
              sliced_buffer_interval->IntervalForMakeFreeChunks(i).start,
              context.prefetch_end_time));
    }
  }

  return absl::StrCat(
      "Trying alternate memory allocation. Slice times = { ",
      absl::StrJoin(slice_times, ", "), " }. Estimated prefetch end times = { ",
      absl::StrJoin(estimated_slice_prefetch_end_times, ", "), " }");
}

std::optional<AlternateMemoryBestFitHeap::Chunk>
AlternateMemoryBestFitHeap::FindBestChunkCandidate(
    const AllocationRequest& request, const AliasedOffset* preferred_offset,
    BufferInterval* alternate_mem_interval) const {
  SlicedBufferInterval sliced_buffer_interval =
      SlicedBufferInterval::CreateMutableInterval(*alternate_mem_interval);
  std::vector<Chunk> chunks = FindBestChunkCandidates(request, preferred_offset,
                                                      &sliced_buffer_interval);
  CHECK_LE(chunks.size(), 1);
  if (chunks.empty()) {
    return std::nullopt;
  }
  return chunks[0];
}

std::vector<AlternateMemoryBestFitHeap::Chunk>
AlternateMemoryBestFitHeap::FindBestChunkCandidates(
    const AllocationRequest& request, const AliasedOffset* preferred_offset,
    SlicedBufferInterval* alternate_mem_interval) const {
  int64_t end_time = request.end_time;
  if (!preferred_offset) {
    // First find the earliest use that is the same or later than the end time.
    const auto& use_times = request.all_use_times;
    auto use_time_it = absl::c_lower_bound(use_times, end_time);
    CHECK(use_time_it != use_times.end());
    int64_t earliest_use = *use_time_it;
    auto earliest_use_it = use_time_it;

    // Then find the latest use that can be allocated contiguously without
    // copies.
    const Shape& shape = request.allocation_value->defining_position().shape();
    for (;
         (use_time_it + 1) != use_times.end() &&
         options_.prefetch_interval_picker->CanAllocateInAlternateMemoryNoCopy(
             shape, *use_time_it, *(use_time_it + 1));
         ++use_time_it) {
    }
    CHECK(use_time_it != use_times.end());
    int64_t latest_contiguous_use_time = *use_time_it;

    // Find chunks that are as long living as possible.
    std::vector<Chunk> last_chunk_candidates;
    int64_t latest_matching_use = std::numeric_limits<int64_t>::min();
    (void)std::lower_bound(
        earliest_use_it, std::next(use_time_it), -1, [&](int64_t use, int64_t) {
          alternate_mem_interval->UpdateEndTime(use);
          std::vector<Chunk> chunk_candidates =
              FindChunkCandidates(*alternate_mem_interval);
          int64_t candidates_end =
              absl::c_max_element(chunk_candidates, [](const Chunk& c1,
                                                       const Chunk& c2) {
                return c1.chunk_end() < c2.chunk_end();
              })->chunk_end();
          if (candidates_end <= available_heap_size()) {
            if (use > latest_matching_use) {
              last_chunk_candidates = std::move(chunk_candidates);
              latest_matching_use = use;
            }
            return true;
          }
          return false;
        });
    if (!last_chunk_candidates.empty()) {
      VLOG(3) << "FindBestChunkCandidates earliest use = " << earliest_use
              << ", latest contiguous use = " << latest_contiguous_use_time
              << ", use with available mem = " << latest_matching_use
              << ", offsets = { "
              << absl::StrJoin(last_chunk_candidates, ", ",
                               [](std::string* out, const Chunk& c) {
                                 absl::StrAppend(out, c.offset);
                               })
              << " }";
    }
    alternate_mem_interval->UpdateEndTime(end_time);
    return last_chunk_candidates;
  }
  // If a preferred offset is given, try to find an allocation at that offset
  // only.
  alternate_mem_interval->UpdateEndTime(end_time);
  std::vector<Chunk> chunk_candidates =
      FindChunkCandidates(*alternate_mem_interval, preferred_offset->offset);
  int64_t candidates_start =
      absl::c_min_element(chunk_candidates, [](const Chunk& c1,
                                               const Chunk& c2) {
        return c1.offset < c2.offset;
      })->offset;

  if (candidates_start == preferred_offset->offset) {
    return chunk_candidates;
  }

  return {};
}

StatusOr<MemorySpaceAssignment::AsyncCopyStats>
MemorySpaceAssignment::CalculateAsyncCopyStats() const {
  AsyncCopyStats stats;
  int64_t current_copies = 0;
  TF_ASSIGN_OR_RETURN(std::unique_ptr<HloDataflowAnalysis> dataflow_analysis,
                      HloDataflowAnalysis::Run(*module_));
  for (const HloComputation* computation :
       module_->MakeNonfusionComputations()) {
    for (HloInstruction* instruction : computation->instructions()) {
      if (instruction->opcode() == HloOpcode::kCopyStart ||
          (instruction->opcode() == HloOpcode::kAsyncStart &&
           instruction->async_wrapped_instruction()->opcode() ==
               HloOpcode::kSlice)) {
        current_copies++;
      } else if (instruction->opcode() == HloOpcode::kCopyDone ||
                 (instruction->opcode() == HloOpcode::kAsyncDone &&
                  instruction->async_wrapped_instruction()->opcode() ==
                      HloOpcode::kSlice)) {
        current_copies--;
        int64_t size =
            options_.size_fn(dataflow_analysis->GetUniqueValueAt(instruction));
        if (instruction->shape().layout().memory_space() ==
            options_.alternate_memory_space) {
          ++stats.num_prefetches;
          stats.prefetch_bytes += size;
          if (instruction->opcode() == HloOpcode::kAsyncDone &&
              instruction->async_wrapped_instruction()->opcode() ==
                  HloOpcode::kSlice) {
            ++stats.num_sliced_prefetch_slices;
          }
        } else {
          ++stats.num_evictions;
          stats.eviction_bytes += size;
        }
      } else if (instruction->IsCustomCall(kConcatBitcastCustomCall)) {
        ++stats.num_sliced_prefetches;
      }
      stats.max_outstanding_async_copies =
          std::max(stats.max_outstanding_async_copies, current_copies);
    }
  }
  return stats;
}

/*static*/ MemorySpaceAssignment::BufferIntervalCompare
MemorySpaceAssignment::GetMemoryBoundednessBufferIntervalCompare(
    const MemorySpaceAssignmentCostAnalysis& cost_analysis,
    MemorySpaceAssignmentCostAnalysis::Cache* cache) {
  return [&cost_analysis, cache](const BufferInterval& x,
                                 const BufferInterval& y) {
    float x_memory_boundedness = cost_analysis.GetMemoryBoundedness(x, cache);
    float y_memory_boundedness = cost_analysis.GetMemoryBoundedness(y, cache);
    if (x_memory_boundedness != y_memory_boundedness) {
      return x_memory_boundedness > y_memory_boundedness;
    }
    // Tie-break if the memory boundedness is the same.
    return GlobalDecreasingSizeBestFitHeap<
        HloValue>::GetSpatialBufferIntervalCompare()(x, y);
  };
}

/*static*/ StatusOr<std::unique_ptr<PresetAssignments>>
MemorySpaceAssignment::Run(HloModule* module,
                           const HloLiveRange& hlo_live_range,
                           const HloAliasAnalysis& alias_analysis,
                           const Options& options) {
  CHECK(module->has_schedule());
  VLOG(3) << "Module before memory space assignment: ";
  XLA_VLOG_LINES(3, module->ToString());
  VLOG(3) << "Schedule: " << module->schedule().ToString();
  MemorySpaceAssignment memory_space_assignment(module, options,
                                                hlo_live_range);

  return memory_space_assignment.RunMemorySpaceAssignment(hlo_live_range,
                                                          alias_analysis);
}

StatusOr<std::unique_ptr<PresetAssignments>>
MemorySpaceAssignment::RunMemorySpaceAssignment(
    const HloLiveRange& hlo_live_range,
    const HloAliasAnalysis& alias_analysis) {
  TF_RETURN_IF_ERROR(FindAllocationSequence(hlo_live_range, alias_analysis));

  if (options_.cost_analysis) {
    float estimated_time =
        ComputeEstimatedElapsedTime(hlo_live_range, allocations_);
    VLOG(1) << "Estimated elapsed time (sec): " << estimated_time;
  }

  TF_RETURN_IF_ERROR(Process(hlo_live_range));
  ScheduleAsynchronousCopies();
  TF_RETURN_IF_ERROR(SimplifyGraph());
  TF_RETURN_IF_ERROR(FixSchedule());
  TF_RETURN_IF_ERROR(ExportAndColorBuffers());

  VLOG(3) << "Module after memory space assignment: ";
  XLA_VLOG_LINES(3, module_->ToString());
  TF_CHECK_OK(module_->schedule().Verify());
  TF_ASSIGN_OR_RETURN(AsyncCopyStats stats, CalculateAsyncCopyStats());
  VLOG(1) << "Maximum number of outstanding async copies/slices: "
          << stats.max_outstanding_async_copies;
  VLOG(1) << "Number of prefetches: " << stats.num_prefetches
          << ", in bytes: " << stats.prefetch_bytes;
  VLOG(1) << "Number of sliced prefetches: " << stats.num_sliced_prefetches
          << ", consuming number of slices: "
          << stats.num_sliced_prefetch_slices;
  VLOG(1) << "Number of evictions: " << stats.num_evictions
          << ", in bytes: " << stats.eviction_bytes;

  TF_RETURN_IF_ERROR(VerifyAndExportHeapSimulatorTrace());

  return std::move(preset_assignments_);
}

Status MemorySpaceAssignment::FindAllocationSequence(
    const HloLiveRange& hlo_live_range,
    const HloAliasAnalysis& alias_analysis) {
  auto algorithm = std::make_unique<AlternateMemoryBestFitHeap>(
      &allocations_, options_, alias_analysis, hlo_live_range);

  HeapSimulator::Options heap_simulator_options;
  heap_simulator_options.may_reuse_operand_buffers = false;
  heap_simulator_options.alloc_constants = true;
  TF_RETURN_IF_ERROR(HeapSimulator::Run(std::move(algorithm), *module_,
                                        module_->schedule(), alias_analysis,
                                        options_.size_fn,
                                        heap_simulator_options)
                         .status());
  return OkStatus();
}

bool MemorySpaceAssignment::Allocation::is_copy_like_allocation() const {
  return is_copy_allocation() || is_sliced_copy_allocation();
}

void MemorySpaceAssignment::Allocation::AddUse(HloUse use) {
  HloInstruction* operand =
      use.instruction->mutable_operand(use.operand_number);
  // If the use is a tuple, look inside the tuple to find the actual use.
  for (int64_t index : use.operand_index) {
    if (operand->opcode() != HloOpcode::kTuple) {
      break;
    }
    operand = operand->mutable_operand(index);
  }

  // Look beyond GetTupleElement(Tuple()) pattern for any bitcasts.
  std::function<HloInstruction*(HloInstruction*)> get_simplified_operand;
  get_simplified_operand = [&](HloInstruction* instruction) {
    while (instruction->opcode() == HloOpcode::kGetTupleElement) {
      HloInstruction* operand =
          get_simplified_operand(instruction->mutable_operand(0));
      if (operand->opcode() == HloOpcode::kTuple) {
        instruction = operand->mutable_operand(instruction->tuple_index());
      } else {
        return instruction;
      }
    }
    return instruction;
  };
  operand = get_simplified_operand(operand);

  uses_.push_back(use);
}

void MemorySpaceAssignment::Allocation::set_offset(int64_t offset) {
  CHECK(chunk_.has_value());
  *chunk_ = Chunk::FromOffsetSize(offset, chunk_->size);
}

float MemorySpaceAssignment::ComputeEstimatedElapsedTime(
    const HloLiveRange& hlo_live_range, const AllocationSequence& allocations) {
  absl::flat_hash_map<const HloInstruction*, std::vector<ShapeIndex>>
      outputs_in_alternate_memory_map;
  absl::flat_hash_map<const HloInstruction*,
                      std::vector<std::pair<int64_t, ShapeIndex>>>
      operands_in_alternate_memory_map;

  for (auto& allocation : allocations) {
    if (!allocation->is_copy_allocation()) {
      if (allocation->memory_space() == MemorySpace::kAlternate) {
        const HloInstruction* defining_instruction =
            allocation->defining_position().instruction;
        outputs_in_alternate_memory_map[defining_instruction].push_back(
            allocation->defining_position().index);
      }
    }
    for (auto& hlo_use : allocation->uses()) {
      const HloInstruction* use_instruction = hlo_use.instruction;
      operands_in_alternate_memory_map[use_instruction].push_back(
          std::make_pair(hlo_use.operand_number, hlo_use.operand_index));
    }
  }

  const auto& instruction_sequence =
      hlo_live_range.flattened_instruction_sequence().instructions();
  float total_elapsed = 0.0;
  for (const HloInstruction* instruction : instruction_sequence) {
    std::vector<ShapeIndex> outputs_in_alternate_memory;
    auto output_it = outputs_in_alternate_memory_map.find(instruction);
    if (output_it != outputs_in_alternate_memory_map.end()) {
      outputs_in_alternate_memory = output_it->second;
    }
    std::vector<std::pair<int64_t, ShapeIndex>> operands_in_alternate_memory;
    auto operand_it = operands_in_alternate_memory_map.find(instruction);
    if (operand_it != operands_in_alternate_memory_map.end()) {
      operands_in_alternate_memory = operand_it->second;
    }
    float instruction_elapsed =
        options_.cost_analysis->GetInstructionElapsedInAlternateMemory(
            *instruction, operands_in_alternate_memory,
            outputs_in_alternate_memory);
    float while_nest_multiplier = IPow<float>(
        options_.xla_tpu_memory_space_assignment_while_execution_count,
        options_.cost_analysis->CalculateComputationNestLevel(
            instruction,
            /*while_only=*/true));
    total_elapsed += while_nest_multiplier * instruction_elapsed;
  }
  return total_elapsed;
}

Status MemorySpaceAssignment::Allocation::Process() {
  if (is_scoped_allocation()) {
    // Nothing to do here for scoped allocations.
    return OkStatus();
  }
  HloInstruction* producing_instruction = AddGetTupleElements();
  HloComputation* computation = producing_instruction->parent();
  for (const HloUse& use : uses_) {
    Shape operand_shape = use.instruction->operand(use.operand_number)->shape();
    HloInstruction* replacement_instruction = producing_instruction;
    if (operand_shape.IsTuple()) {
      TF_ASSIGN_OR_RETURN(
          replacement_instruction,
          TupleUtil::ReplaceTupleWith(
              producing_instruction,
              use.instruction->mutable_operand(use.operand_number),
              use.operand_index));
    } else if (operand_shape != producing_instruction->shape()) {
      VLOG(4) << "Old shape = " << operand_shape.ToString()
              << ", new shape = " << producing_instruction->shape().ToString()
              << "; inserting a bitcast.";
      replacement_instruction = computation->AddInstruction(
          HloInstruction::CreateBitcast(operand_shape, producing_instruction));
    }
    TF_RETURN_IF_ERROR(use.instruction->ReplaceOperandWith(
        use.operand_number, replacement_instruction));
  }
  return OkStatus();
}

HloInstruction* MemorySpaceAssignment::Allocation::AddGetTupleElements() const {
  CHECK_NE(defining_position().instruction, nullptr);

  Shape shape = defining_position().shape();
  CHECK(shape.IsArray()) << "Allocation shape is not an array. Shape = "
                         << shape.ToString()
                         << " position = " << defining_position().shape();
  return TupleUtil::AddGetTupleElements(defining_position());
}

std::string MemorySpaceAssignment::Allocation::ToString() const {
  std::string memory_space_str =
      memory_space_ == MemorySpace::kDefault ? "def" : "alt";
  if (chunk_) {
    absl::StrAppend(&memory_space_str, " (off: ", chunk_->offset, ")");
  }
  return absl::StrCat((is_scoped_allocation() ? "Scoped " : ""),
                      "Allocation in ", memory_space_str, " defined at ",
                      defining_position_.ToString(),
                      ", start_time:", start_time(), ", end_time:", end_time(),
                      ", uses: ", UsesToString(uses()));
}

std::string MemorySpaceAssignment::CopyAllocation::ToString() const {
  std::string memory_space_str =
      memory_space_ == MemorySpace::kDefault ? "def" : "alt";
  if (chunk_) {
    absl::StrAppend(&memory_space_str, " (off: ", chunk_->offset, ")");
  }
  return absl::StrCat("Copy Allocation in ", memory_space_str,
                      ", start_time:", start_time(), ", end_time:", end_time(),
                      ", copy_start_after_time: ", copy_start_schedule_after(),
                      ", copy_done_before_time: ", copy_done_schedule_before(),
                      ", uses: ", UsesToString(uses()), ", from ",
                      prev_allocation_.ToString());
}

std::string MemorySpaceAssignment::SliceParam::ToString() const {
  return absl::StrCat("[", start_inclusive, ",", end_exclusive, ")");
}

bool MemorySpaceAssignment::SliceParam::operator==(
    const SliceParam& other) const {
  return start_inclusive == other.start_inclusive &&
         end_exclusive == other.end_exclusive;
}

std::string MemorySpaceAssignment::SliceProposal::ToString() const {
  return absl::StrCat(
      "{ slice_shape: ", slice_shape.ToString(true), ", slice_params: { ",
      absl::StrJoin(slice_params, ", ",
                    [](std::string* out, const SliceParam& param) {
                      absl::StrAppend(out, param.ToString());
                    }),
      " }, slice_size: ", slice_size, " }");
}

std::ostream& operator<<(std::ostream& os,
                         const MemorySpaceAssignment::SliceProposal& proposal) {
  os << proposal.ToString();
  return os;
}

std::tuple<const Shape&, const std::vector<MemorySpaceAssignment::SliceParam>&,
           int64_t>
MemorySpaceAssignment::SliceProposal::ToTuple() const {
  return std::make_tuple(std::ref(slice_shape), std::ref(slice_params),
                         slice_size);
}

bool MemorySpaceAssignment::SliceProposal::operator==(
    const SliceProposal& other) const {
  return ToTuple() == other.ToTuple();
}

std::string MemorySpaceAssignment::SliceDecision::ToString() const {
  return absl::StrCat("{ chunk: ", chunk.ToString(),
                      ", (exclusive) start_time: ", exclusive_start_time,
                      ", sizing: ", sizing.ToString(),
                      ", copy_resource_consumed: ", copy_resource_consumed,
                      " }");
}

namespace {

std::tuple<const MemorySpaceAssignment::Chunk&, int64_t,
           const MemorySpaceAssignment::SliceProposal&, float>
SliceDecisionToTuple(const MemorySpaceAssignment::SliceDecision& decision) {
  return std::make_tuple(
      std::ref(decision.chunk), decision.exclusive_start_time,
      std::ref(decision.sizing), decision.copy_resource_consumed);
}

}  // namespace

bool MemorySpaceAssignment::SliceDecision::operator==(
    const SliceDecision& other) const {
  return SliceDecisionToTuple(*this) == SliceDecisionToTuple(other);
}

std::string MemorySpaceAssignment::SlicedCopyAllocation::SliceDetail::ToString()
    const {
  return absl::StrCat("{ slice_decision: ", slice_decision.ToString(),
                      ", copy_start_after_time: ", copy_start_after_time,
                      ", copy_done_before_time: ", copy_done_before_time, " }");
}

namespace {

std::tuple<const MemorySpaceAssignment::SliceDecision&, int64_t, int64_t,
           const HloInstruction*, const HloInstruction*>
SliceDetailToTuple(
    const MemorySpaceAssignment::SlicedCopyAllocation::SliceDetail&
        slice_detail) {
  return std::make_tuple(std::ref(slice_detail.slice_decision),
                         slice_detail.copy_start_after_time,
                         slice_detail.copy_done_before_time,
                         slice_detail.copy_start, slice_detail.copy_done);
}

}  // namespace

bool MemorySpaceAssignment::SlicedCopyAllocation::SliceDetail::operator==(
    const SliceDetail& other) const {
  return SliceDetailToTuple(*this) == SliceDetailToTuple(other);
}

Status
MemorySpaceAssignment::SlicedCopyAllocation::SliceDetail::CreateAsyncSlice(
    const Shape& original_shape, HloInstruction& producer,
    HloComputation& parent, absl::FunctionRef<void(Shape*)> update_layout_fn) {
  if (original_shape.rank() != slice_decision.sizing.slice_params.size()) {
    return FailedPrecondition(
        "%s", absl::StrCat("The number of SlicedCopyAllocation parameters ",
                           slice_decision.sizing.slice_params.size(),
                           " does not match the rank ", original_shape.rank(),
                           " of the tensor we are slicing."));
  }

  std::vector<int64_t> start_indices;
  start_indices.reserve(slice_decision.sizing.slice_params.size());
  std::vector<int64_t> limit_indices;
  limit_indices.reserve(slice_decision.sizing.slice_params.size());
  std::vector<int64_t> strides;
  strides.reserve(slice_decision.sizing.slice_params.size());
  Shape new_shape(original_shape);

  for (int i = 0; i < slice_decision.sizing.slice_params.size(); ++i) {
    const SliceParam& slice_param = slice_decision.sizing.slice_params[i];
    start_indices.push_back(slice_param.start_inclusive);
    limit_indices.push_back(slice_param.end_exclusive);
    strides.push_back(1);
    int64_t new_value = slice_param.end_exclusive - slice_param.start_inclusive;
    if (new_value <= 0) {
      return FailedPrecondition(
          "%s", absl::StrCat("SlicedCopyAllocation new dimension size is ",
                             new_value, ", expected something > 0."));
    }
    if (new_shape.dimensions(i) < new_value) {
      return FailedPrecondition(
          "%s",
          absl::StrCat("SlicedCopyAllocation sliced dimension size ", new_value,
                       " is bigger than its original dimension size of ",
                       new_shape.dimensions(i), "."));
    }
    new_shape.set_dimensions(i, new_value);
  }
  update_layout_fn(&new_shape);
  if (!Shape::Equal().IgnoreMemorySpaceInLayout()(
          slice_decision.sizing.slice_shape, new_shape)) {
    return FailedPrecondition(
        "%s",
        absl::StrCat(
            "Slice was calculated to have shape ",
            slice_decision.sizing.slice_shape.ToString(true),
            ", but we are trying to create the slice instruction with shape ",
            new_shape.ToString(true), "."));
  }

  HloInstruction* slice = parent.AddInstruction(HloInstruction::CreateSlice(
      new_shape, &producer, start_indices, limit_indices, strides));
  TF_ASSIGN_OR_RETURN(copy_done, parent.CreateAsyncInstructions(
                                     slice, {ShapeUtil::MakeShape(S32, {})}));
  copy_start = copy_done->mutable_operand(0);

  return OkStatus();
}

namespace {

// Helper function to compute the underlying Allocation chunk for a
// SlicedCopyAllocation.
std::optional<MemorySpaceAssignment::Chunk> GetSlicedCopyAllocationChunk(
    const std::vector<MemorySpaceAssignment::SliceDecision>&
        slice_decisions_sorted_by_start_time) {
  if (slice_decisions_sorted_by_start_time.empty()) {
    return std::nullopt;
  }
  auto offset_cmp = [](const MemorySpaceAssignment::SliceDecision& lhs,
                       const MemorySpaceAssignment::SliceDecision& rhs) {
    return lhs.chunk.offset < rhs.chunk.offset;
  };
  auto end_cmp = [](const MemorySpaceAssignment::SliceDecision& lhs,
                    const MemorySpaceAssignment::SliceDecision& rhs) {
    return lhs.chunk.chunk_end() < rhs.chunk.chunk_end();
  };
  return MemorySpaceAssignment::Chunk::FromOffsetEnd(
      std::min_element(slice_decisions_sorted_by_start_time.begin(),
                       slice_decisions_sorted_by_start_time.end(), offset_cmp)
          ->chunk.offset,
      std::max_element(slice_decisions_sorted_by_start_time.begin(),
                       slice_decisions_sorted_by_start_time.end(), end_cmp)
          ->chunk.chunk_end());
}

// Helper function to compute the start time for a SlicedCopyAllocation.
int64_t GetSlicedCopyAllocationExclusiveStartTime(
    const std::vector<MemorySpaceAssignment::SliceDecision>&
        slice_decisions_sorted_by_exclusive_start_time) {
  if (slice_decisions_sorted_by_exclusive_start_time.empty()) {
    return -1;
  }

  return slice_decisions_sorted_by_exclusive_start_time.front()
      .exclusive_start_time;
}

}  // namespace

MemorySpaceAssignment::SlicedCopyAllocation::SlicedCopyAllocation(
    const Allocation& prev_allocation, MemorySpace memory_space,
    std::vector<SliceDecision> slice_decisions_sorted_by_exclusive_start_time,
    int64_t end_time, int64_t copy_done_schedule_before_time,
    absl::FunctionRef<void(Shape*)> update_layout_fn)
    : Allocation(
          /*defining_position=*/{nullptr, {}}, memory_space,
          GetSlicedCopyAllocationChunk(
              slice_decisions_sorted_by_exclusive_start_time),
          // Allocation uses an inclusive start time
          ExclusiveToInclusiveStartTime(
              GetSlicedCopyAllocationExclusiveStartTime(
                  slice_decisions_sorted_by_exclusive_start_time)),
          end_time,
          /*is_scoped_allocation=*/false),
      original_shape_to_slice_(prev_allocation.defining_position().shape()),
      prev_allocation_(prev_allocation),
      update_layout_fn_(update_layout_fn) {
  CHECK_GE(slice_decisions_sorted_by_exclusive_start_time.size(), 2);
  slice_details_sorted_by_start_time_.reserve(
      slice_decisions_sorted_by_exclusive_start_time.size());
  for (SliceDecision& decision :
       slice_decisions_sorted_by_exclusive_start_time) {
    int64_t copy_done_schedule_after_time = decision.exclusive_start_time;
    slice_details_sorted_by_start_time_.push_back(SliceDetail{
        std::move(decision),
        copy_done_schedule_after_time,
        copy_done_schedule_before_time,
        /*copy_start=*/nullptr,
        /*copy_done=*/nullptr,
    });
  }
}

namespace {

// Sets defining_position with the copy_complete instruction and replaces all
// uses of the allocation with the copy_complete instruction.
Status ProcessCopyLikeAllocationUses(HloPosition& defining_position,
                                     std::vector<HloUse>& uses,
                                     HloComputation* computation,
                                     HloInstruction* copy_complete) {
  // Update the allocation position with the copy complete instruction, so that
  // if there are further copies from it, they can find the correct position.
  defining_position = HloPosition{copy_complete, {}};

  // Replace all the uses of the copy-like allocation with the copy complete
  // instruction.
  for (HloUse use : uses) {
    // If the operand is a tuple, we need to descend to the actual instruction
    // we want to replace.
    HloInstruction* replacement_instruction = copy_complete;
    Shape operand_shape = use.instruction->operand(use.operand_number)->shape();
    if (operand_shape.IsTuple()) {
      TF_ASSIGN_OR_RETURN(
          replacement_instruction,
          TupleUtil::ReplaceTupleWith(
              copy_complete,
              use.instruction->mutable_operand(use.operand_number),
              use.operand_index));
    } else if (operand_shape != copy_complete->shape()) {
      // When processing allocations, we treat bitcasts as trivial positions and
      // do not create allocations for them. We insert bitcasts after copies, to
      // account for the fact that we don't have an allocation for the bitcast.
      VLOG(4) << "Old shape = " << operand_shape.ToString()
              << ", new shape = " << copy_complete->shape().ToString()
              << "; inserting a bitcast.";
      replacement_instruction = computation->AddInstruction(
          HloInstruction::CreateBitcast(operand_shape, copy_complete));
    }
    TF_RETURN_IF_ERROR(use.instruction->ReplaceOperandWith(
        use.operand_number, replacement_instruction));
  }

  return OkStatus();
}

}  // namespace

Status MemorySpaceAssignment::SlicedCopyAllocation::Process() {
  Shape shape = defining_position().shape();
  HloInstruction* producing_instruction = AddGetTupleElements();

  // Calling Process() over the previous allocation might have modified the
  // defining position, and hence the shape that was used when we computed
  // the slices. In cases where the shape has changed, we insert a bitcast, so
  // slice instructions operate on the originally sliced shape.
  //
  // Note, these bitcasts are being inserted in the same cases that
  // ProcessCopyLikeAllocationUses() is inserting bitcasts, except we are
  // inserting the bitcasts before the copy, instead of after the copy.
  if (!Shape::Equal().IgnoreMemorySpaceInLayout()(shape,
                                                  original_shape_to_slice_)) {
    int64_t new_memory_space = shape.layout().memory_space();
    shape = original_shape_to_slice_;
    shape.mutable_layout()->set_memory_space(new_memory_space);
    producing_instruction = producing_instruction->parent()->AddInstruction(
        HloInstruction::CreateBitcast(shape, producing_instruction));
  }

  HloComputation* computation = producing_instruction->parent();
  std::vector<HloInstruction*> slice_dones;
  slice_dones.reserve(slice_details_sorted_by_start_time_.size());

  // Sliced copy allocations need to insert asynchronous copy nodes.
  for (SliceDetail& slice_detail : slice_details_sorted_by_start_time_) {
    TF_RETURN_IF_ERROR(slice_detail.CreateAsyncSlice(
        shape, *producing_instruction, *computation, update_layout_fn_));
    VLOG(4) << "Created " << slice_detail.copy_start->name()
            << " for copy allocation: " << ToString();
    slice_dones.push_back(slice_detail.copy_done);
  }

  TF_RETURN_IF_ERROR(CreateBitcastConcat(shape, slice_dones));

  return ProcessCopyLikeAllocationUses(defining_position_, uses_, computation,
                                       concat_);
}

void MemorySpaceAssignment::SlicedCopyAllocation::MarkNeeded(
    absl::flat_hash_set<const Allocation*>& needed_allocations) const {
  needed_allocations.insert(this);
  prev_allocation_.MarkNeeded(needed_allocations);
}

HloPosition MemorySpaceAssignment::SlicedCopyAllocation::defining_position()
    const {
  // Unless explicitly set, the defining position of a sliced copy allocation is
  // retrieved from the previous allocation. This is because we don't create
  // new CopyStart/CopyDone instructions until later and the position should
  // point to the previous (copy or otherwise) allocation's position for the
  // original defining position.
  if (defining_position_.instruction == nullptr) {
    return prev_allocation_.defining_position();
  }
  return defining_position_;
}

int64_t MemorySpaceAssignment::SlicedCopyAllocation::earliest_available_time()
    const {
  return slice_details_sorted_by_start_time().back().copy_done_before_time;
}

std::vector<int64_t>
MemorySpaceAssignment::SlicedCopyAllocation::SliceOffsetsSortedByStartTime()
    const {
  std::vector<int64_t> offsets;
  offsets.reserve(slice_details_sorted_by_start_time_.size());

  for (const SliceDetail& slice_detail : slice_details_sorted_by_start_time_) {
    offsets.push_back(slice_detail.slice_decision.chunk.offset);
  }

  return offsets;
}

void MemorySpaceAssignment::SlicedCopyAllocation::AddDiffToAllSliceOffsets(
    int64_t diff) {
  for (SliceDetail& slice_detail : slice_details_sorted_by_start_time_) {
    Chunk& chunk = slice_detail.slice_decision.chunk;
    chunk = Chunk::FromOffsetSize(chunk.offset + diff, chunk.size);
  }
}

void MemorySpaceAssignment::SlicedCopyAllocation::ImportRepackedSliceData(
    const MemorySpaceAssignmentRepacker::SlicedAllocationData& data) {
  int num_slices = slice_details_sorted_by_start_time_.size();
  CHECK_EQ(data.slices_sorted_by_offset.size(), num_slices);

  std::vector<SliceDetail*> slice_details_sorted_by_offset;
  slice_details_sorted_by_offset.reserve(num_slices);
  for (SliceDetail& slice_detail : slice_details_sorted_by_start_time_) {
    slice_details_sorted_by_offset.push_back(&slice_detail);
  }
  absl::c_sort(slice_details_sorted_by_offset, [](const SliceDetail* lhs,
                                                  const SliceDetail* rhs) {
    return lhs->slice_decision.chunk.offset < rhs->slice_decision.chunk.offset;
  });

  for (int i = 0; i < num_slices; ++i) {
    SliceDetail* slice_detail = slice_details_sorted_by_offset[i];
    Chunk& chunk = slice_detail->slice_decision.chunk;
    const MemorySpaceAssignmentRepacker::Slice& repacked_slice_data =
        data.slices_sorted_by_offset[i];
    chunk = Chunk::FromOffsetSize(repacked_slice_data.offset, chunk.size);
    slice_detail->copy_start_after_time =
        repacked_slice_data.inclusive_start_time - 1;
    slice_detail->slice_decision.exclusive_start_time =
        InclusiveToExclusiveStartTime(repacked_slice_data.inclusive_start_time);
  }

  absl::c_sort(slice_details_sorted_by_start_time_,
               [](const SliceDetail& lhs, const SliceDetail& rhs) {
                 return std::make_tuple(lhs.copy_start_after_time,
                                        lhs.slice_decision.chunk.offset) <
                        std::make_tuple(rhs.copy_start_after_time,
                                        rhs.slice_decision.chunk.offset);
               });
}

const std::vector<MemorySpaceAssignment::SlicedCopyAllocation::SliceDetail>&
MemorySpaceAssignment::SlicedCopyAllocation::
    slice_details_sorted_by_start_time() const {
  return slice_details_sorted_by_start_time_;
}

std::vector<MemorySpaceAssignment::SlicedCopyAllocation::SliceDetail>&
MemorySpaceAssignment::SlicedCopyAllocation::
    mutable_slice_details_sorted_by_start_time() {
  return slice_details_sorted_by_start_time_;
}

std::tuple<const MemorySpaceAssignment::Allocation&,
           const std::vector<
               MemorySpaceAssignment::SlicedCopyAllocation::SliceDetail>&,
           const HloInstruction*>
MemorySpaceAssignment::SlicedCopyAllocation::ToTuple() const {
  return std::make_tuple(
      std::ref(*this), std::ref(slice_details_sorted_by_start_time_), concat_);
}

bool MemorySpaceAssignment::SlicedCopyAllocation::operator==(
    const SlicedCopyAllocation& other) const {
  return ToTuple() == other.ToTuple();
}

std::string MemorySpaceAssignment::SlicedCopyAllocation::ToString() const {
  std::string memory_space_str = "def";
  if (memory_space_ == MemorySpace::kAlternate) {
    memory_space_str = absl::StrCat("alt (off: ", chunk_->offset, ")");
  }
  return absl::StrCat(
      "Sliced Copy Allocation in ", memory_space_str,
      ", start_time:", start_time(), ", end_time:", end_time(),
      ", first_slice_copy_start_after_time: ",
      slice_details_sorted_by_start_time().front().copy_start_after_time,
      ", last_slice_copy_done_before_time: ",
      slice_details_sorted_by_start_time().back().copy_done_before_time,
      ", uses: ", UsesToString(uses()), ", from ", prev_allocation_.ToString());
}

Status MemorySpaceAssignment::SlicedCopyAllocation::CreateBitcastConcat(
    const Shape& shape, absl::Span<HloInstruction* const> slices) {
  CHECK(!slices.empty());
  concat_ =
      slices.front()->parent()->AddInstruction(HloInstruction::CreateCustomCall(
          shape, slices, kConcatBitcastCustomCall));
  return OkStatus();
}

std::string MemorySpaceAssignment::MirroredAllocation::ToString() const {
  return absl::StrCat("Mirrored Allocation for ",
                      original_allocation_.ToString());
}

std::string MemorySpaceAssignment::ParentAllocation::ToString() const {
  return absl::StrCat("Parent Allocation mirrored at ",
                      defining_position_.ToString(), ", originally ",
                      original_allocation_.ToString());
}

MemorySpaceAssignment::CopyAllocation::CopyAllocation(
    Allocation& prev_allocation, MemorySpace memory_space,
    std::optional<Chunk> chunk, int64_t copy_start_schedule_after_time,
    int64_t end_time, int64_t copy_done_schedule_before_time,
    std::optional<int64_t> cross_program_prefetch_index)
    : Allocation(/*defining_position=*/{nullptr, {}}, memory_space, chunk,
                 // Allocation uses an inclusive start time
                 ExclusiveToInclusiveStartTime(copy_start_schedule_after_time),
                 end_time,
                 /*is_scoped_allocation=*/false),
      prev_allocation_(prev_allocation),
      copy_start_schedule_after_(copy_start_schedule_after_time),
      copy_done_schedule_before_(copy_done_schedule_before_time),
      cross_program_prefetch_index_(cross_program_prefetch_index) {}

Status MemorySpaceAssignment::CopyAllocation::Process() {
  // Copy allocations need to insert asynchronous copy nodes.
  Shape shape = defining_position().shape();
  HloInstruction* producing_instruction = AddGetTupleElements();
  HloComputation* computation = producing_instruction->parent();
  copy_start_ = computation->AddInstruction(HloInstruction::CreateCopyStart(
      ShapeUtil::MakeTupleShape({shape, shape, ShapeUtil::MakeShape(U32, {})}),
      producing_instruction, cross_program_prefetch_index_));
  copy_done_ = computation->AddInstruction(
      HloInstruction::CreateUnary(shape, HloOpcode::kCopyDone, copy_start_));
  VLOG(4) << "Created " << copy_start_->name()
          << " for copy allocation: " << ToString();

  return ProcessCopyLikeAllocationUses(defining_position_, uses_, computation,
                                       copy_done_);
}

Status MemorySpaceAssignment::MirroredAllocation::Process() {
  defining_position_ = original_allocation_.defining_position();
  return Allocation::Process();
}

Status MemorySpaceAssignment::ParentAllocation::Process() {
  // Add an additional parameter to the while HLO with a reference to the buffer
  // in the default memory space.
  HloInstruction* producing_instruction =
      original_allocation_.AddGetTupleElements();
  int new_tuple_index = calling_instruction_->shape().tuple_shapes_size();

  TF_ASSIGN_OR_RETURN(
      HloInstruction * new_while_operand,
      TupleUtil::ReplaceTupleWith(producing_instruction,
                                  calling_instruction_->mutable_operand(0),
                                  {new_tuple_index}));
  TF_RETURN_IF_ERROR(calling_instruction_->ReplaceOperandWithDifferentShape(
      0, new_while_operand));
  *calling_instruction_->mutable_shape() = new_while_operand->shape();
  *calling_instruction_->while_condition()
       ->parameter_instruction(0)
       ->mutable_shape() = new_while_operand->shape();
  *calling_instruction_->while_body()
       ->parameter_instruction(0)
       ->mutable_shape() = new_while_operand->shape();
  defining_position_.index = {new_tuple_index};
  // Also replace the while op with a tuple that has the old shape. Note that we
  // need to first take a snapshot of the users before calling ExtractPrefix
  // since ExtractPrefix introduces additional gte users.
  std::vector<HloInstruction*> while_users = calling_instruction_->users();
  HloInstruction* tuple_with_old_shape =
      TupleUtil::ExtractPrefix(calling_instruction_, new_tuple_index);
  TF_RETURN_IF_ERROR(calling_instruction_->ReplaceAllUsesWithDifferentShape(
      while_users, tuple_with_old_shape));
  return Allocation::Process();
}

Status MemorySpaceAssignment::ParentAllocation::PostProcess() {
  // Update the root of the while body with the new parameter. The reason why we
  // need a separate post-process for this is because other allocations may have
  // while body root as a use, so they would update the old root instead of the
  // new root. Doing the post-process step later ensures the root has been
  // updated with other changes, and we can safely add the additional parameter.
  HloComputation* while_body = calling_instruction_->while_body();
  TF_ASSIGN_OR_RETURN(HloInstruction * new_while_body_root,
                      TupleUtil::ReplaceTupleWith(
                          AddGetTupleElements(), while_body->root_instruction(),
                          defining_position_.index));
  while_body->set_root_instruction(new_while_body_root,
                                   /*accept_different_shape=*/true);
  return OkStatus();
}

void MemorySpaceAssignment::Allocation::MarkIfNeeded(
    absl::flat_hash_set<const Allocation*>& needed_allocations) const {
  MarkNeeded(needed_allocations);
}

void MemorySpaceAssignment::Allocation::MarkNeeded(
    absl::flat_hash_set<const Allocation*>& needed_allocations) const {
  needed_allocations.insert(this);
}

void MemorySpaceAssignment::CopyAllocation::MarkNeeded(
    absl::flat_hash_set<const Allocation*>& needed_allocations) const {
  needed_allocations.insert(this);
  prev_allocation_.MarkNeeded(needed_allocations);
}

void MemorySpaceAssignment::ParentAllocation::MarkIfNeeded(
    absl::flat_hash_set<const Allocation*>& needed_allocations) const {
  // Parent allocations are only needed if they have any uses or if there is a
  // copy allocation that copies this value (in that case, the copy allocation
  // will call this allocation's MarkNeeded function).
  if (!uses_.empty()) {
    MarkNeeded(needed_allocations);
  }
}

void MemorySpaceAssignment::ParentAllocation::MarkNeeded(
    absl::flat_hash_set<const Allocation*>& needed_allocations) const {
  needed_allocations.insert(this);
  original_allocation_.MarkNeeded(needed_allocations);
}

void MemorySpaceAssignment::MirroredAllocation::MarkNeeded(
    absl::flat_hash_set<const Allocation*>& needed_allocations) const {
  needed_allocations.insert(this);
  original_allocation_.MarkNeeded(needed_allocations);
}

Status MemorySpaceAssignment::Process(const HloLiveRange& hlo_live_range) {
  VLOG(1) << "Processing assigned buffers...";
  // Since some parent allocations may not be needed (e.g. when they don't have
  // any uses and if there is no other (non-parent) allocation that depends on
  // it, before we process the allocations, mark all allocations that are
  // needed.
  absl::flat_hash_set<const Allocation*> needed_allocations;
  if (options_.always_spill_to_default_memory) {
    TransformAllocationSequenceToSpill(allocations_, hlo_live_range);
  }
  for (auto& allocation : allocations_) {
    allocation->MarkIfNeeded(needed_allocations);
  }
  // Insert CopyStart/CopyDone and SliceStart/SliceDone pairs.
  for (auto& allocation : allocations_) {
    VLOG(3) << "Processing: " << allocation->ToString();
    if (!needed_allocations.contains(allocation.get())) {
      VLOG(3) << "Allocation not needed.";
      continue;
    }
    TF_RETURN_IF_ERROR(allocation->Process());
    // Add the offset and size of the allocation in the alternate memory to
    // the output map.
    if (allocation->is_scoped_allocation()) {
      CHECK(allocation->memory_space() == MemorySpace::kAlternate);
      scoped_memory_assignments_.emplace_back(
          allocation->defining_position().instruction, allocation->chunk());
      alternate_memory_size_ =
          std::max(alternate_memory_size_, allocation->chunk().chunk_end());
    } else if (allocation->memory_space() == MemorySpace::kAlternate) {
      if (allocation->is_sliced_copy_allocation()) {
        // Add slices
        const SlicedCopyAllocation& sliced_copy_allocation =
            *static_cast<const SlicedCopyAllocation*>(allocation.get());
        for (const SlicedCopyAllocation::SliceDetail& details :
             sliced_copy_allocation.slice_details_sorted_by_start_time()) {
          alternate_memory_assignments_.push_back(
              {{details.copy_done, {}}, details.slice_decision.chunk});
          alternate_memory_size_ = std::max(
              alternate_memory_size_, details.slice_decision.chunk.chunk_end());
        }
        CHECK(
            !sliced_copy_allocation.cross_program_prefetch_index().has_value());
      }

      alternate_memory_assignments_.emplace_back(
          allocation->defining_position(), allocation->chunk());
      alternate_memory_size_ =
          std::max(alternate_memory_size_, allocation->chunk().chunk_end());

      if (allocation->cross_program_prefetch_index().has_value()) {
        TF_RETURN_IF_ERROR(module_->SetCrossProgramPrefetchOffset(
            *allocation->cross_program_prefetch_index(),
            allocation->chunk().offset));
      }
    }
  }

  // Post-process allocations. This is only used for parent allocations where we
  // update the body root with a reference to the buffer in default memory
  // space.
  for (auto& allocation : allocations_) {
    if (needed_allocations.contains(allocation.get())) {
      VLOG(3) << "Post-Processing: " << allocation->ToString();
      TF_RETURN_IF_ERROR(allocation->PostProcess());
    }
  }
  return OkStatus();
}

Status MemorySpaceAssignment::ExportAndColorBuffers() {
  VLOG(1) << "Exporting buffers...";
  TF_ASSIGN_OR_RETURN(auto alias_analysis, HloAliasAnalysis::Run(module_));
  absl::flat_hash_map<int64_t, int64_t> seen_buffer_offsets;
  VLOG(3) << "Exported alternate memory allocations:";
  for (const auto& position_and_chunk : alternate_memory_assignments_) {
    const HloPosition& defining_position = position_and_chunk.first;
    const Chunk& chunk = position_and_chunk.second;
    const HloBuffer& buffer = alias_analysis->GetUniqueBufferAt(
        defining_position.instruction, defining_position.index);
    auto seen_buffer_offset_it = seen_buffer_offsets.find(buffer.id());
    if (seen_buffer_offset_it != seen_buffer_offsets.end()) {
      CHECK_EQ(chunk.offset, seen_buffer_offset_it->second)
          << "Mismatch in offset for positions that map to the same value: "
          << buffer.ToString() << ", pos: " << defining_position.ToString();
    } else {
      VLOG(3) << " [" << chunk.offset << ", " << chunk.size
              << "] : " << defining_position.ToString() << " ("
              << buffer.ToString() << ")";
      preset_assignments_->add_chunk(defining_position, chunk);
      seen_buffer_offsets[buffer.id()] = chunk.offset;
    }
  }

  VLOG(3) << "Exported scoped allocations in alternate memory:";
  for (const auto& instruction_and_chunk : scoped_memory_assignments_) {
    HloInstruction* instruction = instruction_and_chunk.first;
    const Chunk& chunk = instruction_and_chunk.second;
    VLOG(3) << " [" << chunk.offset << ", " << chunk.size
            << "] : " << instruction->name();
    preset_assignments_->add_scoped_allocation_chunk(instruction, chunk);
  }

  if (!preset_assignments_->chunks().empty() ||
      !preset_assignments_->scoped_allocation_chunks().empty()) {
    preset_assignments_
        ->assignment_information_for_space(options_.alternate_memory_space)
        ->size = alternate_memory_size_;
  }

  VLOG(3) << "Exported alternate memory sizes:";
  for (auto& pair : preset_assignments_->assignment_informations()) {
    VLOG(3) << "  space: " << pair.first << ", size: " << pair.second.size;
  }

  VLOG(1) << "Coloring buffers...";
  // Color the pending positions and all of their aliased buffers.
  for (const auto& defining_position_and_chunk :
       preset_assignments_->chunks()) {
    const HloPosition& defining_position = defining_position_and_chunk.first;
    for (auto& buffer : alias_analysis->ComputeBuffersAt(
             defining_position.instruction, defining_position.index)) {
      for (auto& value : buffer->values()) {
        for (auto& position : value->positions()) {
          VLOG(4) << "Coloring " << position.ToString();
          Shape* shape = ShapeUtil::GetMutableSubshape(
              position.instruction->mutable_shape(), position.index);
          CHECK(shape->IsArray()) << "Coloring a shape that is not an array: "
                                  << position.ToString();
          shape->mutable_layout()->set_memory_space(
              options_.alternate_memory_space);
        }
      }
    }
  }
  return OkStatus();
}

void MemorySpaceAssignment::RemoveAssignmentForInstruction(
    const HloInstruction* instruction) {
  auto it = alternate_memory_assignments_.begin();
  auto end = alternate_memory_assignments_.end();
  while (it != end) {
    const HloPosition& position = it->first;
    if (position.instruction == instruction) {
      VLOG(3) << "Removing instruction from alternate memory assignments.";
      if (std::next(it) == end) {
        alternate_memory_assignments_.pop_back();
        break;
      } else {
        // Swap the removed position and chunk with the back and pop back.
        *it = alternate_memory_assignments_.back();
        alternate_memory_assignments_.pop_back();
        end = alternate_memory_assignments_.end();
      }
    } else {
      ++it;
    }
  }
}

Status MemorySpaceAssignment::SimplifyGraph() {
  VLOG(1) << "Simplifying graph...";
  for (HloComputation* computation : module_->MakeNonfusionComputations()) {
    // Parallel computations aren't in the schedule and don't need to be
    // modified.
    if (!computations_in_schedule_.contains(computation)) {
      VLOG(4) << "Not simplifying " << computation->name()
              << " because it's not in the schedule.";
      continue;
    }
    // Drop control dependencies. Since the computation is already scheduled, we
    // don't need control dependencies anymore, and having control
    // predecessors/successors prevents us from removing instructions without
    // users (HloComputation::IsSafelyRemovable returns false if there are
    // control dependencies).
    for (HloInstruction* instruction :
         computation->MakeInstructionPostOrder()) {
      TF_RETURN_IF_ERROR(instruction->DropAllControlDeps());
    }
    // We perform limited DCE and forward the tuple operand in patterns like
    // GetTupleElement(Tuple(a, b), 0). This is mostly because memory space
    // assignment is ran late in compilation (after DCE and arithmetic
    // simplification passes) and we don't want to generate redundant code.  Run
    // to fixed point.
    bool computation_modified = true;
    while (computation_modified) {
      computation_modified = false;
      VLOG(4) << "Running simplify graph loop over " << computation->name();
      for (HloInstruction* instruction :
           computation->MakeInstructionPostOrder()) {
        if (computation->IsSafelyRemovable(instruction) &&
            instruction->IsDead() && !instruction->HasSideEffect() &&
            instruction->opcode() != HloOpcode::kCopyStart &&
            instruction->opcode() != HloOpcode::kCopyDone) {
          VLOG(4) << "Instruction removed: " << instruction->ToString();
          // Ensure the alternate memory assignments don't contain a reference
          // to the removed instruction.
          RemoveAssignmentForInstruction(instruction);
          // Instead of deleting the instruction from the schedule, replace it
          // with a nullptr. This is needed because FixSchedule relies on the
          // logical time that is the index into flattened_instructions_ for
          // scheduling asynchronous copies.
          auto instruction_it =
              absl::c_find(flattened_instructions_, instruction);
          if (instruction_it != flattened_instructions_.end()) {
            *instruction_it = nullptr;
          }
          TF_RETURN_IF_ERROR(computation->RemoveInstruction(instruction));
          computation_modified = true;
        } else if (instruction->opcode() == HloOpcode::kGetTupleElement) {
          HloInstruction* operand = instruction->mutable_operand(0);
          if (operand->opcode() == HloOpcode::kTuple) {
            HloInstruction* forwarded_instruction =
                operand->mutable_operand(instruction->tuple_index());
            VLOG(4) << "Replacing uses of " << instruction->ToString()
                    << " with " << forwarded_instruction->ToString();
            TF_RETURN_IF_ERROR(
                instruction->ReplaceAllUsesWith(forwarded_instruction));
            computation_modified = true;
          }
        } else if (instruction->opcode() == HloOpcode::kTuple) {
          // Replace Tuple(GetTupleElement(x), ..., GetTupleElement(x)) pattern
          // with x.
          bool can_replace =
              instruction->operand_count() > 0 &&
              instruction->operand(0)->opcode() ==
                  HloOpcode::kGetTupleElement &&
              instruction->operand(0)
                      ->operand(0)
                      ->shape()
                      .tuple_shapes_size() == instruction->operand_count();
          for (int operand_number = 0;
               operand_number < instruction->operand_count();
               ++operand_number) {
            const HloInstruction* operand =
                instruction->operand(operand_number);
            if (operand->opcode() != HloOpcode::kGetTupleElement ||
                operand->tuple_index() != operand_number ||
                operand->operand(0) != instruction->operand(0)->operand(0)) {
              can_replace = false;
              break;
            }
          }
          if (can_replace) {
            HloInstruction* forwarded_instruction =
                instruction->mutable_operand(0)->mutable_operand(0);
            VLOG(4) << "Replacing uses of " << instruction->ToString()
                    << " with " << forwarded_instruction->ToString();
            TF_RETURN_IF_ERROR(
                instruction->ReplaceAllUsesWith(forwarded_instruction));
            computation_modified = true;
          }
        }
      }
    }
  }

  return OkStatus();
}

namespace {

// An interface that is used to wrap asynchronous copies, asynchronous slices,
// and asynchronous slice concat operations, for use in MSA's scheduling
// algorithm (ScheduleAsynchronousCopies).
//
// Each AsyncCopy step represents 1 copy, 1 slice, or 1 concat. Each step
// has an optional start phase (e.g., to start a copy or slice), and a required
// done phase (e.g., to finish a copy or slice, or to perform a concat).
class AsyncCopyStep {
 public:
  struct StartPhase {
    int64_t schedule_after_time;
    HloInstruction* instruction;
  };
  struct DonePhase {
    int64_t schedule_before_time;
    HloInstruction* instruction;
  };

  virtual ~AsyncCopyStep() = default;

  bool operator<(const AsyncCopyStep& rhs) const {
    std::optional<StartPhase> lhs_start_phase = start_phase();
    auto lhs_tuple = std::make_tuple(
        done_phase().schedule_before_time,
        (lhs_start_phase.has_value() ? lhs_start_phase->schedule_after_time
                                     : done_phase().schedule_before_time));
    std::optional<StartPhase> rhs_start_phase = rhs.start_phase();
    auto rhs_tuple = std::make_tuple(
        rhs.done_phase().schedule_before_time,
        (rhs_start_phase.has_value() ? rhs_start_phase->schedule_after_time
                                     : rhs.done_phase().schedule_before_time));

    return lhs_tuple < rhs_tuple;
  }

  virtual HloPosition defining_position() const = 0;

  virtual std::optional<StartPhase> start_phase() const = 0;
  virtual void set_start_phase_schedule_after_time(int64_t schedule_after) = 0;
  virtual DonePhase done_phase() const = 0;

 protected:
  AsyncCopyStep() = default;
};

class AsyncCopyStepForCopyAllocation : public AsyncCopyStep {
 public:
  explicit AsyncCopyStepForCopyAllocation(
      MemorySpaceAssignment::CopyAllocation* copy_allocation)
      : AsyncCopyStep(), copy_allocation_(copy_allocation) {}

  ~AsyncCopyStepForCopyAllocation() override = default;

  HloPosition defining_position() const override {
    return copy_allocation_->defining_position();
  }

  std::optional<StartPhase> start_phase() const override {
    StartPhase phase{copy_allocation_->copy_start_schedule_after(),
                     copy_allocation_->copy_start()};

    return phase;
  }

  void set_start_phase_schedule_after_time(int64_t schedule_after) override {
    copy_allocation_->set_copy_start_schedule_after(schedule_after);
  }

  DonePhase done_phase() const override {
    return {copy_allocation_->copy_done_schedule_before(),
            copy_allocation_->copy_done()};
  }

 private:
  MemorySpaceAssignment::CopyAllocation* copy_allocation_ = nullptr;
};

class AsyncCopyStepForSlice : public AsyncCopyStep {
 public:
  AsyncCopyStepForSlice(
      MemorySpaceAssignment::SlicedCopyAllocation* sliced_copy_allocation,
      size_t slice_index)
      : AsyncCopyStep(),
        sliced_copy_allocation_(sliced_copy_allocation),
        slice_index_(slice_index) {}

  ~AsyncCopyStepForSlice() override = default;

  HloPosition defining_position() const override {
    return sliced_copy_allocation_->defining_position();
  }

  std::optional<StartPhase> start_phase() const override {
    const MemorySpaceAssignment::SlicedCopyAllocation::SliceDetail&
        slice_details =
            sliced_copy_allocation_
                ->slice_details_sorted_by_start_time()[slice_index_];
    StartPhase phase{slice_details.copy_start_after_time,
                     slice_details.copy_start};

    return phase;
  }

  void set_start_phase_schedule_after_time(int64_t schedule_after) override {
    sliced_copy_allocation_
        ->mutable_slice_details_sorted_by_start_time()[slice_index_]
        .copy_start_after_time = schedule_after;
  }

  DonePhase done_phase() const override {
    const MemorySpaceAssignment::SlicedCopyAllocation::SliceDetail&
        slice_details =
            sliced_copy_allocation_
                ->slice_details_sorted_by_start_time()[slice_index_];
    DonePhase phase{slice_details.copy_done_before_time,
                    slice_details.copy_done};

    return phase;
  }

 private:
  MemorySpaceAssignment::SlicedCopyAllocation* sliced_copy_allocation_ =
      nullptr;
  size_t slice_index_;
};

class AsyncCopyStepForSliceConcat : public AsyncCopyStep {
 public:
  explicit AsyncCopyStepForSliceConcat(
      MemorySpaceAssignment::SlicedCopyAllocation* sliced_copy_allocation)
      : AsyncCopyStep(), sliced_copy_allocation_(sliced_copy_allocation) {}

  ~AsyncCopyStepForSliceConcat() override = default;

  HloPosition defining_position() const override {
    return sliced_copy_allocation_->defining_position();
  }

  std::optional<StartPhase> start_phase() const override {
    return std::nullopt;
  }

  void set_start_phase_schedule_after_time(int64_t schedule_after) override {}

  DonePhase done_phase() const override {
    return {sliced_copy_allocation_->earliest_available_time(),
            sliced_copy_allocation_->concat()};
  }

 private:
  MemorySpaceAssignment::SlicedCopyAllocation* sliced_copy_allocation_ =
      nullptr;
};

}  // namespace

void MemorySpaceAssignment::ScheduleAsynchronousCopies() {
  VLOG(1) << "Scheduling asynchronous copies...";
  for (MemorySpace memory_space :
       {MemorySpace::kDefault, MemorySpace::kAlternate}) {
    std::vector<std::unique_ptr<AsyncCopyStep>> async_copy_steps;
    for (auto& allocation : allocations_) {
      if (allocation->memory_space() != memory_space) {
        continue;
      }

      if (allocation->is_copy_allocation()) {
        auto copy_allocation = static_cast<CopyAllocation*>(allocation.get());
        async_copy_steps.push_back(
            std::make_unique<AsyncCopyStepForCopyAllocation>(copy_allocation));
      } else if (allocation->is_sliced_copy_allocation()) {
        auto sliced_copy_allocation =
            static_cast<SlicedCopyAllocation*>(allocation.get());
        for (int i = 0; i < sliced_copy_allocation
                                ->mutable_slice_details_sorted_by_start_time()
                                .size();
             ++i) {
          async_copy_steps.push_back(std::make_unique<AsyncCopyStepForSlice>(
              sliced_copy_allocation, i));
        }
        async_copy_steps.push_back(
            std::make_unique<AsyncCopyStepForSliceConcat>(
                sliced_copy_allocation));
      }
    }

    absl::c_stable_sort(
        async_copy_steps,
        [](const std::unique_ptr<AsyncCopyStep>& lhs,
           const std::unique_ptr<AsyncCopyStep>& rhs) { return *lhs < *rhs; });
    for (std::unique_ptr<AsyncCopyStep>& async_copy_step : async_copy_steps) {
      std::optional<AsyncCopyStep::StartPhase> start_phase =
          async_copy_step->start_phase();
      if (start_phase.has_value()) {
        // If the copy start doesn't happen to be scheduled at the correct
        // computation, delay it until the correct computation starts.
        int64_t copy_start_schedule_after = start_phase->schedule_after_time;

        // Accessing flattened_instructions_ here without checking if it is
        // nullptr is safe because this method is called before SimplifyGraph.
        while (
            async_copy_step->defining_position().instruction->parent() !=
            flattened_instructions_[
                // We can't use -1 to index into flatten_instructions_. However,
                // if we want to place the copy as first instruction, i.e.,
                // after the -1 scheduling position, its parent will be the same
                // as the first instruction, i.e., the one at the 0th position.
                std::max<int64_t>(0, copy_start_schedule_after)]
                ->parent()) {
          VLOG(4) << "Delaying CopyStart (" << copy_start_schedule_after
                  << " to " << (copy_start_schedule_after + 1) << ") for "
                  << start_phase->instruction->ToString()
                  << " because it is not in the correct computation.";
          async_copy_step->set_start_phase_schedule_after_time(
              ++copy_start_schedule_after);
        }
        start_phase = async_copy_step->start_phase();
        schedule_after_[start_phase->schedule_after_time].push_back(
            start_phase->instruction);
      }

      AsyncCopyStep::DonePhase done_phase = async_copy_step->done_phase();
      schedule_before_[done_phase.schedule_before_time].push_back(
          done_phase.instruction);
    }
  }
}

Status MemorySpaceAssignment::FixSchedule() {
  VLOG(1) << "Fixing schedule...";
  TF_RET_CHECK(module_->has_schedule());
  HloSchedule& schedule = module_->schedule();
  for (const HloComputation* computation :
       module_->MakeNonfusionComputations()) {
    // Parallel computations aren't in the schedule and don't need to be
    // modified.
    if (!computations_in_schedule_.contains(computation)) {
      if (computation->IsAsyncComputation()) {
        VLOG(4) << "Created a dummy schedule for async computation "
                << computation->name();
        schedule.GetOrCreateSequence(computation);
        continue;
      }
      VLOG(4) << "Not scheduling " << computation->name()
              << " because it's not in the schedule.";
      continue;
    }
    TF_RET_CHECK(schedule.is_computation_scheduled(computation));
    HloInstructionSequence new_sequence;

    absl::flat_hash_set<HloInstruction*> inserted_instructions;

    VLOG(4) << "Scheduling: " << computation->ToString();

    for (int64_t instruction_index = 0;; ++instruction_index) {
      auto insts_before_iter = schedule_before_.find(instruction_index);
      if (insts_before_iter != schedule_before_.end()) {
        for (HloInstruction* new_instruction : insts_before_iter->second) {
          if (new_instruction->parent() == computation) {
            VLOG(4) << "before " << instruction_index << ": "
                    << new_instruction->name();
            TF_RETURN_IF_ERROR(InsertInstructionAndEnsureOperandsInserted(
                new_instruction, &new_sequence, &inserted_instructions));
          }
        }
      }
      // We allow scheduling copy dones past the root instruction (for
      // end-of-program cross-program prefetch). So the loop exit condition is
      // actually here.
      if (instruction_index >= flattened_instructions_.size()) {
        break;
      }
      HloInstruction* instruction = flattened_instructions_[instruction_index];
      // Insert only if it is not deleted (SimplifyGraph sets it to nullptr if
      // it was deleted) and not previously inserted. Also bitcasts and tuples
      // are treated specially and only inserted as a result of operand
      // dependencies.
      if (instruction != nullptr && instruction->parent() == computation &&
          instruction->opcode() != HloOpcode::kBitcast &&
          instruction->opcode() != HloOpcode::kTuple &&
          !inserted_instructions.contains(instruction)) {
        VLOG(4) << "inst " << instruction_index << ": " << instruction->name();
        TF_RETURN_IF_ERROR(InsertInstructionAndEnsureOperandsInserted(
            instruction, &new_sequence, &inserted_instructions));
      }
      auto insts_after_iter = schedule_after_.find(instruction_index);
      if (insts_after_iter != schedule_after_.end()) {
        for (HloInstruction* new_instruction : insts_after_iter->second) {
          if (new_instruction->parent() == computation) {
            VLOG(4) << "after " << instruction_index << ": "
                    << new_instruction->name();
            TF_RETURN_IF_ERROR(InsertInstructionAndEnsureOperandsInserted(
                new_instruction, &new_sequence, &inserted_instructions));
          }
        }
      }
    }
    // For rare cases where the original sequence is empty, ensure the root
    // instruction and its dependencies are scheduled.
    TF_RETURN_IF_ERROR(EnsureInstructionAndOperandsInserted(
        computation->root_instruction(), &new_sequence,
        &inserted_instructions));
    CHECK_EQ(new_sequence.size(), computation->instruction_count())
        << "New sequence for computation " << computation->name() << " has "
        << new_sequence.size() << " instructions, expects "
        << computation->instruction_count() << ".";
    schedule.set_sequence(computation, new_sequence);
  }

  TF_RETURN_IF_ERROR(schedule.Update());

  return OkStatus();
}

Status MemorySpaceAssignment::VerifyAndExportHeapSimulatorTrace() {
  VLOG(1) << "Verifying...";
  TF_ASSIGN_OR_RETURN(std::unique_ptr<HloAliasAnalysis> alias_analysis,
                      HloAliasAnalysis::Run(module_));
  TF_ASSIGN_OR_RETURN(std::unique_ptr<HloLiveRange> hlo_live_range,
                      HloLiveRange::Run(module_->schedule(), *alias_analysis,
                                        module_->entry_computation()));

  BufferIntervalTree interval_tree;
  absl::flat_hash_set<int64_t> seen_buffers;
  // The key for events is: time, is_free, value_id. This is so that the events
  // are sorted first by time, then within the same time, allocations are sorted
  // earlier than frees, and finally the value id as a tie breaker.
  std::map<std::tuple<int64_t, bool, int64_t>,
           std::tuple<const HloValue*, Chunk, HeapSimulatorTrace::Event::Kind>>
      events;

  auto add_allocation_and_verify = [&](int64_t start_time, int64_t end_time,
                                       const Chunk& chunk,
                                       const HloValue* value) {
    events[std::make_tuple(start_time, /*is_free=*/false, value->id())] =
        std::make_tuple(value, chunk, HeapSimulatorTrace::Event::ALLOC);
    events[std::make_tuple(end_time, /*is_free=*/true, value->id())] =
        std::make_tuple(value, chunk, HeapSimulatorTrace::Event::FREE);

    // Get the chunks overlapping in time and search if they overlap in space
    // as well.
    // TODO(berkin): For now checking against end_time - 1 (exclusive), but we
    // really should check against end_time (inclusive) for cases where the
    // operand can't share buffer with user (see
    // HloDataflowAnalysis::CanShareOperandBufferWithUser).
    for (const Chunk& overlapping_chunk :
         interval_tree.ChunksOverlappingInTime(start_time, end_time - 1)) {
      if (chunk.OverlapsWith(overlapping_chunk)) {
        return InternalError(
            ("Value %s (%d, %d) off: %d size: %d overlaps with another chunk"
             " off: %d size: %d"),
            value->ToShortString(), start_time, end_time, chunk.offset,
            chunk.size, overlapping_chunk.offset, overlapping_chunk.size);
      }
    }
    interval_tree.Add(start_time, end_time - 1, chunk);
    return OkStatus();
  };

  // Go through all instructions in the module to ensure CopyStart/CopyDone
  // instructions copy between alternate memory and default memory.
  for (const HloComputation* computation :
       module_->MakeNonfusionComputations()) {
    for (const HloInstruction* instruction : computation->instructions()) {
      if (instruction->opcode() == HloOpcode::kCopyStart) {
        int64_t from_memory_space =
            ShapeUtil::GetSubshape(instruction->shape(), {1})
                .layout()
                .memory_space();
        int64_t to_memory_space =
            ShapeUtil::GetSubshape(instruction->shape(), {0})
                .layout()
                .memory_space();
        CHECK_NE(from_memory_space, to_memory_space)
            << "Asynchronous copy to the same memory space: "
            << instruction->ToString();
      }
    }
  }

  for (const auto& position_and_chunk : preset_assignments_->chunks()) {
    const HloPosition& position = position_and_chunk.first;
    const Chunk& chunk = position_and_chunk.second;
    const HloBuffer& buffer =
        alias_analysis->GetUniqueBufferAt(position.instruction, position.index);
    CHECK(!seen_buffers.contains(buffer.id()))
        << "Multiple preset assignments for the same buffer: "
        << buffer.ToString() << ", pos: " << position.ToString()
        << ", off: " << chunk.offset << ", size: " << chunk.size;
    seen_buffers.insert(buffer.id());

    for (const HloValue* value : buffer.values()) {
      const HloLiveRange::TimeBound& time_bound =
          hlo_live_range->buffer_live_ranges().at(value);
      const HloInstruction* last_use_instruction = nullptr;
      int64_t last_use_time = time_bound.start;
      for (const HloUse& use : value->GetUses()) {
        int64_t use_time =
            hlo_live_range->instruction_schedule().at(use.instruction);
        if (use_time > last_use_time) {
          last_use_time = use_time;
          last_use_instruction = use.instruction;
        }
      }

      std::function<Status(const HloInstruction*, int64_t, int64_t,
                           absl::string_view)>
          split_conditional_buffer;
      split_conditional_buffer = [&](const HloInstruction* use_instruction,
                                     int64_t start_time, int64_t end_time,
                                     absl::string_view indent_string) {
        // Special case when verifying conditional: we internally split the use
        // of alternate memory in conditionals, so fish them out from the
        // conditionals.
        VLOG(3) << indent_string
                << "Splitting conditional buffer: " << buffer.ToString()
                << " value: " << value->ToShortString() << ": (" << start_time
                << ", " << end_time << ") off: " << chunk.offset
                << ", size: " << chunk.size;
        int64_t earliest_computation_start_time = end_time;
        for (const HloComputation* called_computation :
             use_instruction->called_computations()) {
          int64_t computation_start_time =
              hlo_live_range->computation_span_times()
                  .at(called_computation)
                  .start;
          earliest_computation_start_time =
              std::min(earliest_computation_start_time, computation_start_time);
          int64_t last_use_time = -1;
          const HloInstruction* last_use_instruction = nullptr;
          for (const HloUse& use : value->GetUses()) {
            int64_t use_time =
                hlo_live_range->instruction_schedule().at(use.instruction);
            if (use.instruction->parent() == called_computation &&
                use_time > last_use_time) {
              last_use_time = use_time;
              last_use_instruction = use.instruction;
            }
          }
          if (last_use_time != -1) {
            VLOG(3) << indent_string
                    << " computation: " << called_computation->name() << ": ("
                    << computation_start_time << ", " << last_use_time << ")";
            CHECK(last_use_instruction);
            last_use_time = std::min(last_use_time, end_time);
            if (last_use_instruction->opcode() == HloOpcode::kConditional) {
              // The last use is another (nested) conditional. Call this
              // function recursively.
              TF_RETURN_IF_ERROR(split_conditional_buffer(
                  last_use_instruction, computation_start_time, last_use_time,
                  absl::StrCat(indent_string, "  ")));
            } else {
              TF_RETURN_IF_ERROR(add_allocation_and_verify(
                  computation_start_time, last_use_time, chunk, value));
            }
          }
        }
        VLOG(3) << indent_string << " from beginning until first computation: ("
                << start_time << ", " << (earliest_computation_start_time - 1)
                << ")";
        TF_RETURN_IF_ERROR(add_allocation_and_verify(
            start_time, earliest_computation_start_time - 1, chunk, value));
        return OkStatus();
      };

      if (last_use_instruction &&
          last_use_instruction->opcode() == HloOpcode::kConditional) {
        TF_RETURN_IF_ERROR(split_conditional_buffer(
            last_use_instruction, time_bound.start, time_bound.end, " "));
      } else if (!value->GetUses().empty()) {
        last_use_time = std::min(last_use_time, time_bound.end);
        VLOG(3) << " buffer: " << buffer.ToString()
                << " value: " << value->ToShortString() << ": ("
                << time_bound.start << ", " << last_use_time
                << ") off: " << chunk.offset << ", size: " << chunk.size;
        TF_RETURN_IF_ERROR(add_allocation_and_verify(
            time_bound.start, last_use_time, chunk, value));
      }
    }
  }

  HeapSimulatorTrace* heap_trace =
      &preset_assignments_
           ->assignment_information_for_space(options_.alternate_memory_space)
           ->heap_simulator_trace;
  int64_t memory_usage = 0;
  int64_t max_memory_usage = 0;
  int64_t prev_time = 0;
  int64_t prev_memory_usage = 0;
  for (const auto& event : events) {
    int64_t time;
    bool is_free;
    int64_t buffer_id;
    std::tie(time, is_free, buffer_id) = event.first;
    const HloValue* value;
    Chunk chunk;
    HeapSimulatorTrace::Event::Kind kind;
    std::tie(value, chunk, kind) = event.second;
    HeapSimulatorTrace::Event* heap_trace_event = heap_trace->add_events();
    heap_trace_event->set_kind(kind);
    heap_trace_event->set_buffer_id(buffer_id);
    *heap_trace_event->mutable_instruction_name() =
        std::string(value->instruction()->name());
    *heap_trace_event->mutable_computation_name() =
        std::string(value->instruction()->parent()->name());

    if (prev_time != time) {
      VLOG(2) << "Memory usage: " << std::max(memory_usage, prev_memory_usage)
              << " at time: " << prev_time << " ("
              << hlo_live_range->flattened_instruction_sequence()
                     .instructions()
                     .at(prev_time)
                     ->name()
              << ")";
      prev_time = time;
      prev_memory_usage = memory_usage;
    }
    if (kind == HeapSimulatorTrace::Event::ALLOC) {
      memory_usage += chunk.size;
    } else {
      CHECK_EQ(kind, HeapSimulatorTrace::Event::FREE);
      memory_usage -= chunk.size;
    }
    prev_memory_usage = std::max(prev_memory_usage, memory_usage);
    max_memory_usage = std::max(max_memory_usage, memory_usage);
    VLOG(4) << "Memory usage: " << memory_usage << " at time: " << time;
  }
  VLOG(1) << "Max memory usage ignoring fragmentation: " << max_memory_usage;

  return OkStatus();
}
}  // namespace memory_space_assignment
}  // namespace xla
