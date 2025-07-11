// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_COMPILER_BACKEND_INSTRUCTION_SELECTOR_H_
#define V8_COMPILER_BACKEND_INSTRUCTION_SELECTOR_H_

#include <map>
#include <optional>

#include "src/codegen/cpu-features.h"
#include "src/codegen/machine-type.h"
#include "src/compiler/backend/instruction-scheduler.h"
#include "src/compiler/backend/instruction.h"
#include "src/compiler/feedback-source.h"
#include "src/compiler/linkage.h"
#include "src/compiler/node-matchers.h"
#include "src/compiler/turboshaft/graph.h"
#include "src/compiler/turboshaft/operation-matcher.h"
#include "src/compiler/turboshaft/operations.h"
#include "src/compiler/turboshaft/representations.h"
#include "src/compiler/turboshaft/use-map.h"
#include "src/compiler/turboshaft/utils.h"
#include "src/utils/bit-vector.h"
#include "src/zone/zone-containers.h"

#if V8_ENABLE_WEBASSEMBLY
#include "src/wasm/simd-shuffle.h"
#endif  // V8_ENABLE_WEBASSEMBLY

namespace v8 {
namespace internal {

class TickCounter;

namespace compiler {

// Forward declarations.
class BasicBlock;
struct CallBuffer;  // TODO(bmeurer): Remove this.
class Linkage;
class OperandGenerator;
class SwitchInfo;
struct CaseInfo;
class TurbofanStateObjectDeduplicator;
class TurboshaftStateObjectDeduplicator;

// The flags continuation is a way to combine a branch or a materialization
// of a boolean value with an instruction that sets the flags register.
// The whole instruction is treated as a unit by the register allocator, and
// thus no spills or moves can be introduced between the flags-setting
// instruction and the branch or set it should be combined with.
class FlagsContinuation final {
 public:
  struct ConditionalCompare {
    InstructionCode code;
    FlagsCondition compare_condition;
    FlagsCondition default_flags;
    turboshaft::OpIndex lhs;
    turboshaft::OpIndex rhs;
  };
  // This limit covered almost all the opportunities when compiling the debug
  // builtins.
  static constexpr size_t kMaxCompareChainSize = 4;
  using compare_chain_t = std::array<ConditionalCompare, kMaxCompareChainSize>;

  FlagsContinuation() : mode_(kFlags_none) {}

  // Creates a new flags continuation from the given condition and true/false
  // blocks.
  static FlagsContinuation ForBranch(FlagsCondition condition,
                                     turboshaft::Block* true_block,
                                     turboshaft::Block* false_block) {
    return FlagsContinuation(kFlags_branch, condition, true_block, false_block);
  }

  static FlagsContinuation ForHintedBranch(FlagsCondition condition,
                                           turboshaft::Block* true_block,
                                           turboshaft::Block* false_block,
                                           BranchHint hint) {
    return FlagsContinuation(kFlags_branch, condition, true_block, false_block,
                             hint);
  }

  // Creates a new flags continuation from the given conditional compare chain
  // and true/false blocks.
  static FlagsContinuation ForConditionalBranch(
      compare_chain_t& compares, uint32_t num_conditional_compares,
      FlagsCondition branch_condition, turboshaft::Block* true_block,
      turboshaft::Block* false_block) {
    return FlagsContinuation(compares, num_conditional_compares,
                             branch_condition, true_block, false_block);
  }

  // Creates a new flags continuation for an eager deoptimization exit.
  static FlagsContinuation ForDeoptimize(
      FlagsCondition condition, DeoptimizeReason reason, uint32_t node_id,
      FeedbackSource const& feedback,
      turboshaft::V<turboshaft::FrameState> frame_state) {
    DCHECK(frame_state.valid());
    return FlagsContinuation(kFlags_deoptimize, condition, reason, node_id,
                             feedback, frame_state);
  }
  static FlagsContinuation ForDeoptimizeForTesting(
      FlagsCondition condition, DeoptimizeReason reason, uint32_t node_id,
      FeedbackSource const& feedback,
      turboshaft::OptionalV<turboshaft::FrameState> frame_state = {}) {
    // Tests (e.g. test-instruction-scheduler.cc) may not pass a valid
    // frame_state as that doesn't matter for the test.
    return FlagsContinuation(kFlags_deoptimize, condition, reason, node_id,
                             feedback, frame_state.value_or_invalid());
  }

  // Creates a new flags continuation for a boolean value.
  static FlagsContinuation ForSet(FlagsCondition condition,
                                  turboshaft::OpIndex result) {
    return FlagsContinuation(condition, result);
  }

  // Creates a new flags continuation for a conditional wasm trap.
  static FlagsContinuation ForConditionalTrap(compare_chain_t& compares,
                                              uint32_t num_conditional_compares,
                                              FlagsCondition condition,
                                              TrapId trap_id) {
    return FlagsContinuation(compares, num_conditional_compares, condition,
                             trap_id);
  }

  // Creates a new flags continuation for a wasm trap.
  static FlagsContinuation ForTrap(FlagsCondition condition, TrapId trap_id) {
    return FlagsContinuation(condition, trap_id);
  }

  static FlagsContinuation ForSelect(FlagsCondition condition,
                                     turboshaft::OpIndex result,
                                     turboshaft::OpIndex true_value,
                                     turboshaft::OpIndex false_value) {
    return FlagsContinuation(condition, result, true_value, false_value);
  }

  bool IsNone() const { return mode_ == kFlags_none; }
  bool IsBranch() const { return mode_ == kFlags_branch; }
  bool IsConditionalBranch() const {
    return mode_ == kFlags_conditional_branch;
  }
  bool IsDeoptimize() const { return mode_ == kFlags_deoptimize; }
  bool IsSet() const { return mode_ == kFlags_set; }
  bool IsTrap() const { return mode_ == kFlags_trap; }
  bool IsConditionalTrap() const { return mode_ == kFlags_conditional_trap; }
  bool IsSelect() const { return mode_ == kFlags_select; }
  FlagsCondition condition() const {
    DCHECK(!IsNone());
    return condition_;
  }
  FlagsCondition final_condition() const {
    DCHECK(IsConditionalTrap() || IsConditionalBranch());
    return final_condition_;
  }
  DeoptimizeReason reason() const {
    DCHECK(IsDeoptimize());
    return reason_;
  }
  uint32_t node_id() const {
    DCHECK(IsDeoptimize());
    return node_id_;
  }
  FeedbackSource const& feedback() const {
    DCHECK(IsDeoptimize());
    return feedback_;
  }
  turboshaft::OpIndex frame_state() const {
    DCHECK(IsDeoptimize());
    return frame_state_or_result_;
  }
  turboshaft::OpIndex result() const {
    DCHECK(IsSet() || IsSelect());
    return frame_state_or_result_;
  }
  TrapId trap_id() const {
    DCHECK(IsTrap() || IsConditionalTrap());
    return trap_id_;
  }
  turboshaft::Block* true_block() const {
    DCHECK(IsBranch() || IsConditionalBranch());
    return true_block_;
  }
  turboshaft::Block* false_block() const {
    DCHECK(IsBranch() || IsConditionalBranch());
    return false_block_;
  }
  BranchHint hint() const {
    DCHECK(IsBranch());
    return hint_;
  }
  turboshaft::OpIndex true_value() const {
    DCHECK(IsSelect());
    return true_value_;
  }
  turboshaft::OpIndex false_value() const {
    DCHECK(IsSelect());
    return false_value_;
  }
  const compare_chain_t& compares() const {
    DCHECK(IsConditionalTrap() || IsConditionalBranch());
    return compares_;
  }
  uint32_t num_conditional_compares() const {
    DCHECK(IsConditionalTrap() || IsConditionalBranch());
    return num_conditional_compares_;
  }

  void Negate() {
    DCHECK(!IsNone());
    DCHECK(!IsConditionalTrap() && !IsConditionalBranch());
    condition_ = NegateFlagsCondition(condition_);
  }

  void Commute() {
    DCHECK(!IsNone());
    DCHECK(!IsConditionalTrap() && !IsConditionalBranch());
    condition_ = CommuteFlagsCondition(condition_);
  }

  void Overwrite(FlagsCondition condition) {
    DCHECK(!IsConditionalTrap() && !IsConditionalBranch());
    condition_ = condition;
  }

  void OverwriteAndNegateIfEqual(FlagsCondition condition) {
    DCHECK(condition_ == kEqual || condition_ == kNotEqual);
    DCHECK(!IsConditionalTrap() && !IsConditionalBranch());
    bool negate = condition_ == kEqual;
    condition_ = condition;
    if (negate) Negate();
  }

  void OverwriteUnsignedIfSigned() {
    DCHECK(!IsConditionalTrap() && !IsConditionalBranch());
    switch (condition_) {
      case kSignedLessThan:
        condition_ = kUnsignedLessThan;
        break;
      case kSignedLessThanOrEqual:
        condition_ = kUnsignedLessThanOrEqual;
        break;
      case kSignedGreaterThan:
        condition_ = kUnsignedGreaterThan;
        break;
      case kSignedGreaterThanOrEqual:
        condition_ = kUnsignedGreaterThanOrEqual;
        break;
      default:
        break;
    }
  }

  // Encodes this flags continuation into the given opcode.
  InstructionCode Encode(InstructionCode opcode) {
    opcode |= FlagsModeField::encode(mode_);
    if (mode_ != kFlags_none) {
      opcode |= FlagsConditionField::encode(condition_);
    }
    return opcode;
  }

 private:
  FlagsContinuation(FlagsMode mode, FlagsCondition condition,
                    turboshaft::Block* true_block,
                    turboshaft::Block* false_block)
      : mode_(mode),
        condition_(condition),
        true_block_(true_block),
        false_block_(false_block) {
    DCHECK(mode == kFlags_branch);
    DCHECK_NOT_NULL(true_block);
    DCHECK_NOT_NULL(false_block);
  }

  FlagsContinuation(FlagsMode mode, FlagsCondition condition,
                    turboshaft::Block* true_block,
                    turboshaft::Block* false_block, BranchHint hint)
      : mode_(mode),
        condition_(condition),
        true_block_(true_block),
        false_block_(false_block),
        hint_(hint) {
    DCHECK_EQ(mode, kFlags_branch);
    DCHECK_NOT_NULL(true_block);
    DCHECK_NOT_NULL(false_block);
  }

  FlagsContinuation(compare_chain_t& compares,
                    uint32_t num_conditional_compares,
                    FlagsCondition branch_condition,
                    turboshaft::Block* true_block,
                    turboshaft::Block* false_block)
      : mode_(kFlags_conditional_branch),
        condition_(compares.front().compare_condition),
        final_condition_(branch_condition),
        num_conditional_compares_(num_conditional_compares),
        compares_(compares),
        true_block_(true_block),
        false_block_(false_block) {
    DCHECK_NOT_NULL(true_block);
    DCHECK_NOT_NULL(false_block);
  }

  FlagsContinuation(FlagsMode mode, FlagsCondition condition,
                    DeoptimizeReason reason, uint32_t node_id,
                    FeedbackSource const& feedback,
                    turboshaft::OpIndex frame_state)
      : mode_(mode),
        condition_(condition),
        reason_(reason),
        node_id_(node_id),
        feedback_(feedback),
        frame_state_or_result_(frame_state) {
    DCHECK(mode == kFlags_deoptimize);
    // NOTE: Tests might use this constructor with an invalid frame_state, which
    // is okay because it's never actually accessed.
  }

  FlagsContinuation(FlagsCondition condition, turboshaft::OpIndex result)
      : mode_(kFlags_set),
        condition_(condition),
        frame_state_or_result_(result) {
    DCHECK(result.valid());
  }

  FlagsContinuation(FlagsCondition condition, TrapId trap_id)
      : mode_(kFlags_trap), condition_(condition), trap_id_(trap_id) {}

  FlagsContinuation(compare_chain_t& compares,
                    uint32_t num_conditional_compares, FlagsCondition condition,
                    TrapId trap_id)
      : mode_(kFlags_conditional_trap),
        condition_(compares.front().compare_condition),
        final_condition_(condition),
        num_conditional_compares_(num_conditional_compares),
        compares_(compares),
        trap_id_(trap_id) {}

  FlagsContinuation(FlagsCondition condition, turboshaft::OpIndex result,
                    turboshaft::OpIndex true_value,
                    turboshaft::OpIndex false_value)
      : mode_(kFlags_select),
        condition_(condition),
        frame_state_or_result_(result),
        true_value_(true_value),
        false_value_(false_value) {
    DCHECK(result.valid());
    DCHECK(true_value.valid());
    DCHECK(false_value.valid());
  }

  FlagsMode const mode_;
  FlagsCondition condition_;
  FlagsCondition final_condition_;  // Only valid if mode_ ==
                                    // kFlags_conditional_*

  uint32_t num_conditional_compares_;  // Only valid if mode_ ==
                                       // kFlags_conditional_*.
  compare_chain_t compares_;  // Only valid if mode_ == kFlags_conditional_*.
  DeoptimizeReason reason_;         // Only valid if mode_ == kFlags_deoptimize*
  uint32_t node_id_;                // Only valid if mode_ == kFlags_deoptimize*
  FeedbackSource feedback_;         // Only valid if mode_ == kFlags_deoptimize*
  turboshaft::OpIndex
      frame_state_or_result_;       // Only valid if mode_ == kFlags_deoptimize*
                                    // or mode_ == kFlags_set.
  turboshaft::Block* true_block_;   // Only valid if mode_ == kFlags_branch*.
  turboshaft::Block* false_block_;  // Only valid if mode_ == kFlags_branch*.
  TrapId trap_id_;                  // Only valid if mode_ == kFlags_trap or
                                    // mode_ == kFlags_conditional_trap.
  turboshaft::OpIndex true_value_;  // Only valid if mode_ == kFlags_select.
  turboshaft::OpIndex false_value_;  // Only valid if mode_ == kFlags_select.
  BranchHint hint_ = BranchHint::kNone;
};

// This struct connects nodes of parameters which are going to be pushed on the
// call stack with their parameter index in the call descriptor of the callee.
struct PushParameter {
  PushParameter(turboshaft::OpIndex n = {},
                LinkageLocation l = LinkageLocation::ForAnyRegister())
      : node(n), location(l) {}

  turboshaft::OpIndex node;
  LinkageLocation location;
};

enum class FrameStateInputKind { kAny, kStackSlot };

// Instruction selection generates an InstructionSequence for a given Schedule.
class V8_EXPORT_PRIVATE InstructionSelector final
    : public turboshaft::OperationMatcher {
 public:
  using source_position_table_t =
      turboshaft::GrowingOpIndexSidetable<SourcePosition>;

  enum SourcePositionMode { kCallSourcePositions, kAllSourcePositions };
  enum EnableScheduling : bool {
    kDisableScheduling = false,
    kEnableScheduling = true
  };
  enum EnableRootsRelativeAddressing : bool {
    kDisableRootsRelativeAddressing = false,
    kEnableRootsRelativeAddressing = true
  };
  enum EnableSwitchJumpTable : bool {
    kDisableSwitchJumpTable = false,
    kEnableSwitchJumpTable = true
  };
  enum EnableTraceTurboJson : bool {
    kDisableTraceTurboJson = false,
    kEnableTraceTurboJson = true
  };
  enum EnsureDeterministicNan : bool {
    kNoDeterministicNan = false,
    kEnsureDeterministicNan = true
  };

  class Features final {
   public:
    Features() : bits_(0) {}
    explicit Features(unsigned bits) : bits_(bits) {}
    explicit Features(CpuFeature f) : bits_(1u << f) {}
    Features(CpuFeature f1, CpuFeature f2) : bits_((1u << f1) | (1u << f2)) {}

    bool Contains(CpuFeature f) const { return (bits_ & (1u << f)); }

   private:
    unsigned bits_;
  };

  static MachineOperatorBuilder::Flags SupportedMachineOperatorFlags();
  static MachineOperatorBuilder::AlignmentRequirements AlignmentRequirements();

  static InstructionSelector ForTurboshaft(
      Zone* zone, size_t node_count, Linkage* linkage,
      InstructionSequence* sequence, turboshaft::Graph* schedule, Frame* frame,
      EnableSwitchJumpTable enable_switch_jump_table, TickCounter* tick_counter,
      JSHeapBroker* broker, size_t* max_unoptimized_frame_height,
      size_t* max_pushed_argument_count,
      SourcePositionMode source_position_mode, Features features,
      EnableScheduling enable_scheduling,
      EnableRootsRelativeAddressing enable_roots_relative_addressing,
      EnableTraceTurboJson trace_turbo,
      EnsureDeterministicNan ensure_deterministic_nan);

  InstructionSelector(
      Zone* zone, size_t node_count, Linkage* linkage,
      InstructionSequence* sequence, turboshaft::Graph* schedule,
      source_position_table_t* source_positions, Frame* frame,
      EnableSwitchJumpTable enable_switch_jump_table, TickCounter* tick_counter,
      JSHeapBroker* broker, size_t* max_unoptimized_frame_height,
      size_t* max_pushed_argument_count,
      SourcePositionMode source_position_mode, Features features,
      EnableScheduling enable_scheduling,
      EnableRootsRelativeAddressing enable_roots_relative_addressing,
      EnableTraceTurboJson trace_turbo,
      EnsureDeterministicNan ensure_deterministic_nan);

  // Visit code for the entire graph with the included schedule.
  std::optional<BailoutReason> SelectInstructions();

  void StartBlock(RpoNumber rpo);
  void EndBlock(RpoNumber rpo);
  void AddInstruction(Instruction* instr);
  void AddTerminator(Instruction* instr);

  // ===========================================================================
  // ============= Architecture-independent code emission methods. =============
  // ===========================================================================

  Instruction* Emit(InstructionCode opcode, InstructionOperand output,
                    size_t temp_count = 0, InstructionOperand* temps = nullptr);
  Instruction* Emit(InstructionCode opcode, InstructionOperand output,
                    InstructionOperand a, size_t temp_count = 0,
                    InstructionOperand* temps = nullptr);
  Instruction* Emit(InstructionCode opcode, InstructionOperand output,
                    InstructionOperand a, InstructionOperand b,
                    size_t temp_count = 0, InstructionOperand* temps = nullptr);
  Instruction* Emit(InstructionCode opcode, InstructionOperand output,
                    InstructionOperand a, InstructionOperand b,
                    InstructionOperand c, size_t temp_count = 0,
                    InstructionOperand* temps = nullptr);
  Instruction* Emit(InstructionCode opcode, InstructionOperand output,
                    InstructionOperand a, InstructionOperand b,
                    InstructionOperand c, InstructionOperand d,
                    size_t temp_count = 0, InstructionOperand* temps = nullptr);
  Instruction* Emit(InstructionCode opcode, InstructionOperand output,
                    InstructionOperand a, InstructionOperand b,
                    InstructionOperand c, InstructionOperand d,
                    InstructionOperand e, size_t temp_count = 0,
                    InstructionOperand* temps = nullptr);
  Instruction* Emit(InstructionCode opcode, InstructionOperand output,
                    InstructionOperand a, InstructionOperand b,
                    InstructionOperand c, InstructionOperand d,
                    InstructionOperand e, InstructionOperand f,
                    size_t temp_count = 0, InstructionOperand* temps = nullptr);
  Instruction* Emit(InstructionCode opcode, InstructionOperand output,
                    InstructionOperand a, InstructionOperand b,
                    InstructionOperand c, InstructionOperand d,
                    InstructionOperand e, InstructionOperand f,
                    InstructionOperand g, InstructionOperand h,
                    size_t temp_count = 0, InstructionOperand* temps = nullptr);
  Instruction* Emit(InstructionCode opcode, size_t output_count,
                    InstructionOperand* outputs, size_t input_count,
                    InstructionOperand* inputs, size_t temp_count = 0,
                    InstructionOperand* temps = nullptr);
  Instruction* Emit(Instruction* instr);

  // [0-3] operand instructions with no output, uses labels for true and false
  // blocks of the continuation.
  Instruction* EmitWithContinuation(InstructionCode opcode,
                                    FlagsContinuation* cont);
  Instruction* EmitWithContinuation(InstructionCode opcode,
                                    InstructionOperand a,
                                    FlagsContinuation* cont);
  Instruction* EmitWithContinuation(InstructionCode opcode,
                                    InstructionOperand a, InstructionOperand b,
                                    FlagsContinuation* cont);
  Instruction* EmitWithContinuation(InstructionCode opcode,
                                    InstructionOperand a, InstructionOperand b,
                                    InstructionOperand c,
                                    FlagsContinuation* cont);
  Instruction* EmitWithContinuation(InstructionCode opcode, size_t output_count,
                                    InstructionOperand* outputs,
                                    size_t input_count,
                                    InstructionOperand* inputs,
                                    FlagsContinuation* cont);
  Instruction* EmitWithContinuation(
      InstructionCode opcode, size_t output_count, InstructionOperand* outputs,
      size_t input_count, InstructionOperand* inputs, size_t temp_count,
      InstructionOperand* temps, FlagsContinuation* cont);

  void EmitIdentity(turboshaft::OpIndex node);

  // ===========================================================================
  // ============== Architecture-independent CPU feature methods. ==============
  // ===========================================================================

  bool IsSupported(CpuFeature feature) const {
    return features_.Contains(feature);
  }

  // Returns the features supported on the target platform.
  static Features SupportedFeatures() {
    return Features(CpuFeatures::SupportedFeatures());
  }

  // ===========================================================================
  // ============ Architecture-independent graph covering methods. =============
  // ===========================================================================

  // Used in pattern matching during code generation.
  // Check if {node} can be covered while generating code for the current
  // instruction. A node can be covered if the {user} of the node has the only
  // edge, the two are in the same basic block, and there are no side-effects
  // in-between. The last check is crucial for soundness.
  // For pure nodes, CanCover(a,b) is checked to avoid duplicated execution:
  // If this is not the case, code for b must still be generated for other
  // users, and fusing is unlikely to improve performance.
  bool CanCover(turboshaft::OpIndex user, turboshaft::OpIndex node) const;

  bool CanCoverProtectedLoad(turboshaft::OpIndex user,
                             turboshaft::OpIndex node) const;

  // Used in pattern matching during code generation.
  // This function checks that {node} and {user} are in the same basic block,
  // and that {user} is the only user of {node} in this basic block.  This
  // check guarantees that there are no users of {node} scheduled between
  // {node} and {user}, and thus we can select a single instruction for both
  // nodes, if such an instruction exists. This check can be used for example
  // when selecting instructions for:
  //   n = Int32Add(a, b)
  //   c = Word32Compare(n, 0, cond)
  //   Branch(c, true_label, false_label)
  // Here we can generate a flag-setting add instruction, even if the add has
  // uses in other basic blocks, since the flag-setting add instruction will
  // still generate the result of the addition and not just set the flags.
  // However, if we had uses of the add in the same basic block, we could have:
  //   n = Int32Add(a, b)
  //   o = OtherOp(n, ...)
  //   c = Word32Compare(n, 0, cond)
  //   Branch(c, true_label, false_label)
  // where we cannot select the add and the compare together.  If we were to
  // select a flag-setting add instruction for Word32Compare and Int32Add while
  // visiting Word32Compare, we would then have to select an instruction for
  // OtherOp *afterwards*, which means we would attempt to use the result of
  // the add before we have defined it.
  bool IsOnlyUserOfNodeInSameBlock(turboshaft::OpIndex user,
                                   turboshaft::OpIndex node) const;

  // Checks if {node} was already defined, and therefore code was already
  // generated for it.
  bool IsDefined(turboshaft::OpIndex node) const;

  // Checks if {node} has any uses, and therefore code has to be generated for
  // it. Always returns {true} if the node has effect IsRequiredWhenUnused.
  bool IsUsed(turboshaft::OpIndex node) const;
  // Checks if {node} has any uses, and therefore code has to be generated for
  // it. Ignores the IsRequiredWhenUnused effect.
  bool IsReallyUsed(turboshaft::OpIndex node) const;

  // Checks if {node} is currently live.
  bool IsLive(turboshaft::OpIndex node) const {
    return !IsDefined(node) && IsUsed(node);
  }
  // Checks if {node} is currently live, ignoring the IsRequiredWhenUnused
  // effect.
  bool IsReallyLive(turboshaft::OpIndex node) const {
    return !IsDefined(node) && IsReallyUsed(node);
  }

  // Gets the effect level of {node}.
  int GetEffectLevel(turboshaft::OpIndex node) const;

  // Gets the effect level of {node}, appropriately adjusted based on
  // continuation flags if the node is a branch.
  int GetEffectLevel(turboshaft::OpIndex node, FlagsContinuation* cont) const;

  int GetVirtualRegister(turboshaft::OpIndex node);
  const std::map<uint32_t, int> GetVirtualRegistersForTesting() const;

  // Check if we can generate loads and stores of ExternalConstants relative
  // to the roots register.
  bool CanAddressRelativeToRootsRegister(
      const ExternalReference& reference) const;
  // Check if we can use the roots register to access GC roots.
  bool CanUseRootsRegister() const;

  Isolate* isolate() const { return sequence()->isolate(); }

  const ZoneVector<std::pair<int, int>>& instr_origins() const {
    return instr_origins_;
  }

  turboshaft::OptionalOpIndex FindProjection(turboshaft::OpIndex node,
                                             size_t projection_index);
  template <typename Op>
  auto Inputs(turboshaft::OpIndex node) {
    const Op& op = Cast<Op>(node);
    return InputsImpl(op, std::make_index_sequence<Op::input_count>());
  }
  template <typename Op, std::size_t... Is>
  auto InputsImpl(const Op& op, std::index_sequence<Is...>) {
    return std::make_tuple(op.input(Is)...);
  }
  template <size_t InputCount>
  auto Inputs(turboshaft::OpIndex node) {
    const turboshaft::Operation& op = Get(node);
    DCHECK_EQ(InputCount, op.input_count);
    return InputsImpl(op, std::make_index_sequence<InputCount>());
  }

  // When we want to do branch-if-overflow fusion, we need to be mindful of the
  // 1st projection of the OverflowBinop:
  //   - If it has no uses, all good, we can do the fusion.
  //   - If it has any uses, then they must all be already defined: doing the
  //     fusion will lead to emitting the 1st projection, and any non-defined
  //     operation is earlier in the graph by construction, which means that it
  //     won't be able to use the 1st projection that will now be defined later.
  bool CanDoBranchIfOverflowFusion(turboshaft::OpIndex node);

  // Records that this ProtectedLoad node can be deleted if not used, even
  // though it has a required_when_unused effect.
  void SetProtectedLoadToRemove(turboshaft::OpIndex node) {
    DCHECK(this->IsProtectedLoad(node));
    protected_loads_to_remove_->Add(node.id());
  }

  // Records that this node embeds a ProtectedLoad as operand, and so it is
  // itself a "protected" instruction, for which we'll need to record the source
  // position.
  void MarkAsProtected(turboshaft::OpIndex node) {
    additional_protected_instructions_->Add(node.id());
  }

  void UpdateSourcePosition(Instruction* instruction, turboshaft::OpIndex node);
  bool IsCommutative(turboshaft::OpIndex node) const;
  turboshaft::OpIndex block_terminator(const turboshaft::Block* block) const {
    return schedule_->PreviousIndex(block->end());
  }

  // TODO(nicohartmann): Maybe we should get rid of this.
  turboshaft::Graph* turboshaft_graph() const { return schedule_; }

  turboshaft::Block* block(turboshaft::Graph* schedule,
                           turboshaft::OpIndex node) const {
    // TODO(nicohartmann@): This might be too slow and we should consider
    // precomputing.
    return &schedule->Get(schedule->BlockOf(node));
  }

  RpoNumber rpo_number(const turboshaft::Block* block) const {
    return RpoNumber::FromInt(block->index().id());
  }

  const ZoneVector<turboshaft::Block*>& rpo_order(turboshaft::Graph* schedule) {
    return schedule->blocks_vector();
  }

  bool IsLoopHeader(const turboshaft::Block* block) const {
    return block->IsLoop();
  }

  size_t PredecessorCount(const turboshaft::Block* block) const {
    return block->PredecessorCount();
  }
  turboshaft::Block* PredecessorAt(const turboshaft::Block* block,
                                   size_t index) const {
    return block->Predecessors()[index];
  }

  base::iterator_range<turboshaft::Graph::OpIndexIterator> nodes(
      const turboshaft::Block* block) {
    return schedule_->OperationIndices(*block);
  }

  bool IsRetain(turboshaft::OpIndex node) const {
    return Get(node).Is<turboshaft::RetainOp>();
  }
  bool IsHeapConstant(turboshaft::OpIndex node) const {
    const turboshaft::ConstantOp* constant =
        TryCast<turboshaft::ConstantOp>(node);
    if (constant == nullptr) return false;
    return constant->kind == turboshaft::ConstantOp::Kind::kHeapObject;
  }
  bool IsExternalConstant(turboshaft::OpIndex node) const {
    const turboshaft::ConstantOp* constant =
        TryCast<turboshaft::ConstantOp>(node);
    if (constant == nullptr) return false;
    return constant->kind == turboshaft::ConstantOp::Kind::kExternal;
  }
  bool IsRelocatableWasmConstant(turboshaft::OpIndex node) const {
    const turboshaft::ConstantOp* constant =
        TryCast<turboshaft::ConstantOp>(node);
    if (constant == nullptr) return false;
    return constant->kind ==
           turboshaft::any_of(
               turboshaft::ConstantOp::Kind::kRelocatableWasmCall,
               turboshaft::ConstantOp::Kind::kRelocatableWasmStubCall);
  }
  bool IsLoadOrLoadImmutable(turboshaft::OpIndex node) const {
    return Get(node).opcode == turboshaft::Opcode::kLoad;
  }
  bool IsProtectedLoad(turboshaft::OpIndex node) const;

  bool is_load(turboshaft::OpIndex node) const {
    const turboshaft::Operation& op = Get(node);
    return op.Is<turboshaft::LoadOp>()
#if V8_ENABLE_WEBASSEMBLY
           || op.Is<turboshaft::Simd128LoadTransformOp>()
#if V8_ENABLE_WASM_SIMD256_REVEC
           || op.Is<turboshaft::Simd256LoadTransformOp>()
#endif  // V8_ENABLE_WASM_SIMD256_REVEC
#endif
        ;  // NOLINT(whitespace/semicolon)
  }

  class LoadView {
   public:
    LoadView(turboshaft::Graph* graph, turboshaft::OpIndex node) : node_(node) {
      switch (graph->Get(node_).opcode) {
        case turboshaft::Opcode::kLoad:
          load_ = &graph->Get(node_).Cast<turboshaft::LoadOp>();
          break;
#if V8_ENABLE_WEBASSEMBLY
        case turboshaft::Opcode::kSimd128LoadTransform:
          load_transform_ =
              &graph->Get(node_).Cast<turboshaft::Simd128LoadTransformOp>();
          break;
#if V8_ENABLE_WASM_SIMD256_REVEC
        case turboshaft::Opcode::kSimd256LoadTransform:
          load_transform256_ =
              &graph->Get(node_).Cast<turboshaft::Simd256LoadTransformOp>();
          break;
#endif  // V8_ENABLE_WASM_SIMD256_REVEC
#endif  // V8_ENABLE_WEBASSEMBLY
        default:
          UNREACHABLE();
      }
    }
    LoadRepresentation loaded_rep() const {
      DCHECK_NOT_NULL(load_);
      return load_->machine_type();
    }
    turboshaft::MemoryRepresentation ts_loaded_rep() const {
      DCHECK_NOT_NULL(load_);
      return load_->loaded_rep;
    }
    turboshaft::RegisterRepresentation ts_result_rep() const {
      DCHECK_NOT_NULL(load_);
      return load_->result_rep;
    }
    bool is_protected(bool* traps_on_null) const {
      if (kind().with_trap_handler) {
        if (load_) {
          *traps_on_null = load_->kind.trap_on_null;
#if V8_ENABLE_WEBASSEMBLY
        } else {
#if V8_ENABLE_WASM_SIMD256_REVEC
          DCHECK(
              (load_transform_ && !load_transform_->load_kind.trap_on_null) ||
              (load_transform256_ &&
               !load_transform256_->load_kind.trap_on_null));
#else
          DCHECK(load_transform_);
          DCHECK(!load_transform_->load_kind.trap_on_null);
#endif  // V8_ENABLE_WASM_SIMD256_REVEC
          *traps_on_null = false;
#endif  // V8_ENABLE_WEBASSEMBLY
        }
        return true;
      }
      return false;
    }
    bool is_atomic() const { return kind().is_atomic; }

    turboshaft::OpIndex base() const {
      if (load_) return load_->base();
#if V8_ENABLE_WEBASSEMBLY
      if (load_transform_) return load_transform_->base();
#if V8_ENABLE_WASM_SIMD256_REVEC
      if (load_transform256_) return load_transform256_->base();
#endif  // V8_ENABLE_WASM_SIMD256_REVEC
#endif
      UNREACHABLE();
    }
    turboshaft::OpIndex index() const {
      if (load_) return load_->index().value_or_invalid();
#if V8_ENABLE_WEBASSEMBLY
      if (load_transform_) return load_transform_->index();
#if V8_ENABLE_WASM_SIMD256_REVEC
      if (load_transform256_) return load_transform256_->index();
#endif  // V8_ENABLE_WASM_SIMD256_REVEC
#endif
      UNREACHABLE();
    }
    int32_t displacement() const {
      static_assert(
          std::is_same_v<decltype(turboshaft::StoreOp::offset), int32_t>);
      if (load_) {
        int32_t offset = load_->offset;
        if (load_->kind.tagged_base) {
          CHECK_GE(offset,
                   std::numeric_limits<int32_t>::min() + kHeapObjectTag);
          offset -= kHeapObjectTag;
        }
        return offset;
#if V8_ENABLE_WEBASSEMBLY
      } else if (load_transform_) {
        int32_t offset = load_transform_->offset;
        DCHECK(!load_transform_->load_kind.tagged_base);
        return offset;
#if V8_ENABLE_WASM_SIMD256_REVEC
      } else if (load_transform256_) {
        int32_t offset = load_transform256_->offset;
        DCHECK(!load_transform256_->load_kind.tagged_base);
        return offset;
#endif  // V8_ENABLE_WASM_SIMD256_REVEC
#endif
      }
      UNREACHABLE();
    }
    uint8_t element_size_log2() const {
      static_assert(
          std::is_same_v<decltype(turboshaft::StoreOp::element_size_log2),
                         uint8_t>);
      if (load_) return load_->element_size_log2;
#if V8_ENABLE_WEBASSEMBLY
      if (load_transform_) return 0;
#if V8_ENABLE_WASM_SIMD256_REVEC
      if (load_transform256_) return 0;
#endif  // V8_ENABLE_WASM_SIMD256_REVEC
#endif
      UNREACHABLE();
    }

    operator turboshaft::OpIndex() const { return node_; }

   private:
    turboshaft::LoadOp::Kind kind() const {
      if (load_) return load_->kind;
#if V8_ENABLE_WEBASSEMBLY
      if (load_transform_) return load_transform_->load_kind;
#if V8_ENABLE_WASM_SIMD256_REVEC
      if (load_transform256_) return load_transform256_->load_kind;
#endif  // V8_ENABLE_WASM_SIMD256_REVEC
#endif
      UNREACHABLE();
    }

    turboshaft::OpIndex node_;
    const turboshaft::LoadOp* load_ = nullptr;
#if V8_ENABLE_WEBASSEMBLY
    const turboshaft::Simd128LoadTransformOp* load_transform_ = nullptr;
#if V8_ENABLE_WASM_SIMD256_REVEC
    const turboshaft::Simd256LoadTransformOp* load_transform256_ = nullptr;
#endif  // V8_ENABLE_WASM_SIMD256_REVEC
#endif
  };

  LoadView load_view(turboshaft::OpIndex node) {
    DCHECK(is_load(node));
    return LoadView(schedule_, node);
  }

  class StoreView {
   public:
    StoreView(turboshaft::Graph* graph, turboshaft::OpIndex node)
        : node_(node) {
      op_ = &graph->Get(node_).Cast<turboshaft::StoreOp>();
    }

    StoreRepresentation stored_rep() const {
      return {op_->stored_rep.ToMachineType().representation(),
              op_->write_barrier};
    }
    turboshaft::MemoryRepresentation ts_stored_rep() const {
      return op_->stored_rep;
    }
    std::optional<AtomicMemoryOrder> memory_order() const {
      // TODO(nicohartmann@): Currently we don't support memory orders.
      if (op_->kind.is_atomic) return AtomicMemoryOrder::kSeqCst;
      return std::nullopt;
    }
    MemoryAccessKind access_kind() const {
      return op_->kind.with_trap_handler
                 ? MemoryAccessKind::kProtectedByTrapHandler
                 : MemoryAccessKind::kNormal;
    }
    bool is_atomic() const { return op_->kind.is_atomic; }

    turboshaft::OpIndex base() const { return op_->base(); }
    turboshaft::OptionalOpIndex index() const { return op_->index(); }
    turboshaft::OpIndex value() const { return op_->value(); }
    IndirectPointerTag indirect_pointer_tag() const {
      return static_cast<IndirectPointerTag>(op_->indirect_pointer_tag());
    }
    int32_t displacement() const {
      static_assert(
          std::is_same_v<decltype(turboshaft::StoreOp::offset), int32_t>);
      int32_t offset = op_->offset;
      if (op_->kind.tagged_base) {
        CHECK_GE(offset, std::numeric_limits<int32_t>::min() + kHeapObjectTag);
        offset -= kHeapObjectTag;
      }
      return offset;
    }
    uint8_t element_size_log2() const {
      static_assert(
          std::is_same_v<decltype(turboshaft::StoreOp::element_size_log2),
                         uint8_t>);
      return op_->element_size_log2;
    }

    bool is_store_trap_on_null() const {
      return op_->kind.with_trap_handler && op_->kind.trap_on_null;
    }

    operator turboshaft::OpIndex() const { return node_; }

   private:
    turboshaft::OpIndex node_;
    const turboshaft::StoreOp* op_;
  };

  StoreView store_view(turboshaft::OpIndex node) {
    return StoreView(schedule_, node);
  }

#if V8_ENABLE_WEBASSEMBLY
  // TODO(391750831): Inline this.
  class SimdShuffleView {
   public:
    explicit SimdShuffleView(const turboshaft::Graph* graph,
                             turboshaft::OpIndex node)
        : node_(node) {
      op128_ = &graph->Get(node).Cast<turboshaft::Simd128ShuffleOp>();
      // Initialize input mapping.
      for (int i = 0; i < op128_->input_count; ++i) {
        input_mapping_.push_back(i);
      }
    }

    bool isSimd128() const {
      // TODO(nicohartmann@): Extend when we add support for Simd256.
      return true;
    }

    const uint8_t* data() const { return op128_->shuffle; }

    turboshaft::OpIndex input(int index) const {
      DCHECK_LT(index, op128_->input_count);
      return op128_->input(input_mapping_[index]);
    }

    void SwapInputs() { std::swap(input_mapping_[0], input_mapping_[1]); }

    void DuplicateFirstInput() {
      DCHECK_LE(2, input_mapping_.size());
      input_mapping_[1] = input_mapping_[0];
    }

    operator turboshaft::OpIndex() const { return node_; }

   private:
    turboshaft::OpIndex node_;
    base::SmallVector<int, 2> input_mapping_;
    const turboshaft::Simd128ShuffleOp* op128_;
  };

  SimdShuffleView simd_shuffle_view(turboshaft::OpIndex node) {
    return SimdShuffleView(schedule_, node);
  }
#endif

 private:
  friend OperandGenerator;

  bool UseInstructionScheduling() const {
    return enable_scheduling_ && InstructionScheduler::SchedulerSupported();
  }

  void AppendDeoptimizeArguments(InstructionOperandVector* args,
                                 DeoptimizeReason reason, uint32_t node_id,
                                 FeedbackSource const& feedback,
                                 turboshaft::OpIndex frame_state,
                                 DeoptimizeKind kind = DeoptimizeKind::kEager);

  void EmitTableSwitch(const SwitchInfo& sw,
                       InstructionOperand const& index_operand);
  void EmitBinarySearchSwitch(const SwitchInfo& sw,
                              InstructionOperand const& value_operand);

  void MarkAsTableSwitchTarget(const turboshaft::Block* block);

  void TryRename(InstructionOperand* op);
  int GetRename(int virtual_register);
  void SetRename(turboshaft::OpIndex node, turboshaft::OpIndex rename);
  void UpdateRenames(Instruction* instruction);
  void UpdateRenamesInPhi(PhiInstruction* phi);

  // Inform the instruction selection that {node} was just defined.
  void MarkAsDefined(turboshaft::OpIndex node);

  // Inform the instruction selection that {node} has at least one use and we
  // will need to generate code for it.
  void MarkAsUsed(turboshaft::OpIndex node);

  // Sets the effect level of {node}.
  void SetEffectLevel(turboshaft::OpIndex node, int effect_level);

  // Inform the register allocation of the representation of the value produced
  // by {node}.
  void MarkAsRepresentation(MachineRepresentation rep,
                            turboshaft::OpIndex node);
  void MarkAsRepresentation(turboshaft::RegisterRepresentation rep,
                            turboshaft::OpIndex node) {
    MarkAsRepresentation(rep.machine_representation(), node);
  }
  void MarkAsWord32(turboshaft::OpIndex node) {
    MarkAsRepresentation(MachineRepresentation::kWord32, node);
  }
  void MarkAsWord64(turboshaft::OpIndex node) {
    MarkAsRepresentation(MachineRepresentation::kWord64, node);
  }
  void MarkAsFloat32(turboshaft::OpIndex node) {
    MarkAsRepresentation(MachineRepresentation::kFloat32, node);
  }
  void MarkAsFloat64(turboshaft::OpIndex node) {
    MarkAsRepresentation(MachineRepresentation::kFloat64, node);
  }
  void MarkAsSimd128(turboshaft::OpIndex node) {
    MarkAsRepresentation(MachineRepresentation::kSimd128, node);
  }
  void MarkAsSimd256(turboshaft::OpIndex node) {
    MarkAsRepresentation(MachineRepresentation::kSimd256, node);
  }
  void MarkAsTagged(turboshaft::OpIndex node) {
    MarkAsRepresentation(MachineRepresentation::kTagged, node);
  }
  void MarkAsCompressed(turboshaft::OpIndex node) {
    MarkAsRepresentation(MachineRepresentation::kCompressed, node);
  }

  // Inform the register allocation of the representation of the unallocated
  // operand {op}.
  void MarkAsRepresentation(MachineRepresentation rep,
                            const InstructionOperand& op);

  enum CallBufferFlag {
    kCallCodeImmediate = 1u << 0,
    kCallAddressImmediate = 1u << 1,
    kCallTail = 1u << 2,
    kCallFixedTargetRegister = 1u << 3
  };
  using CallBufferFlags = base::Flags<CallBufferFlag>;

  // Initialize the call buffer with the InstructionOperands, nodes, etc,
  // corresponding
  // to the inputs and outputs of the call.
  // {call_code_immediate} to generate immediate operands to calls of code.
  // {call_address_immediate} to generate immediate operands to address calls.
  void InitializeCallBuffer(turboshaft::OpIndex call, CallBuffer* buffer,
                            CallBufferFlags flags, turboshaft::OpIndex callee,
                            turboshaft::OptionalOpIndex frame_state_opt,
                            base::Vector<const turboshaft::OpIndex> arguments,
                            int return_count, int stack_slot_delta = 0);
  bool IsTailCallAddressImmediate();

  void UpdateMaxPushedArgumentCount(size_t count);

  using StateObjectDeduplicator = TurboshaftStateObjectDeduplicator;
  FrameStateDescriptor* GetFrameStateDescriptor(turboshaft::OpIndex node);
  size_t AddInputsToFrameStateDescriptor(FrameStateDescriptor* descriptor,
                                         turboshaft::OpIndex state,
                                         OperandGenerator* g,
                                         StateObjectDeduplicator* deduplicator,
                                         InstructionOperandVector* inputs,
                                         FrameStateInputKind kind, Zone* zone);
  size_t AddInputsToFrameStateDescriptor(StateValueList* values,
                                         InstructionOperandVector* inputs,
                                         OperandGenerator* g,
                                         StateObjectDeduplicator* deduplicator,
                                         turboshaft::OpIndex node,
                                         FrameStateInputKind kind, Zone* zone);
  size_t AddOperandToStateValueDescriptor(StateValueList* values,
                                          InstructionOperandVector* inputs,
                                          OperandGenerator* g,
                                          StateObjectDeduplicator* deduplicator,
                                          turboshaft::OpIndex input,
                                          MachineType type,
                                          FrameStateInputKind kind, Zone* zone);

  // ===========================================================================
  // ============= Architecture-specific graph covering methods. ===============
  // ===========================================================================

  // Visit nodes in the given block and generate code.
  void VisitBlock(const turboshaft::Block* block);

  // Visit the node for the control flow at the end of the block, generating
  // code if necessary.
  void VisitControl(const turboshaft::Block* block);

  // Visit the node and generate code, if any.
  void VisitNode(turboshaft::OpIndex node);

  // Visit the node and generate code for IEEE 754 functions.
  void VisitFloat64Ieee754Binop(turboshaft::OpIndex, InstructionCode code);
  void VisitFloat64Ieee754Unop(turboshaft::OpIndex, InstructionCode code);

#define DECLARE_GENERATOR_T(x) void Visit##x(turboshaft::OpIndex node);
  DECLARE_GENERATOR_T(Word32And)
  DECLARE_GENERATOR_T(Word32Xor)
  DECLARE_GENERATOR_T(Int32Add)
  DECLARE_GENERATOR_T(Int32Sub)
  DECLARE_GENERATOR_T(Int32Mul)
  DECLARE_GENERATOR_T(Int32MulHigh)
  DECLARE_GENERATOR_T(Int32Div)
  DECLARE_GENERATOR_T(Int32Mod)
  DECLARE_GENERATOR_T(Uint32Div)
  DECLARE_GENERATOR_T(Uint32Mod)
  DECLARE_GENERATOR_T(Uint32MulHigh)
  DECLARE_GENERATOR_T(Word32Or)
  DECLARE_GENERATOR_T(Word32Sar)
  DECLARE_GENERATOR_T(Word32Shl)
  DECLARE_GENERATOR_T(Word32Shr)
  DECLARE_GENERATOR_T(Word32Rol)
  DECLARE_GENERATOR_T(Word32Ror)
  DECLARE_GENERATOR_T(Word64Shl)
  DECLARE_GENERATOR_T(Word64Sar)
  DECLARE_GENERATOR_T(Word64Shr)
  DECLARE_GENERATOR_T(Word64Rol)
  DECLARE_GENERATOR_T(Word64Ror)
  DECLARE_GENERATOR_T(Int32AddWithOverflow)
  DECLARE_GENERATOR_T(Int32MulWithOverflow)
  DECLARE_GENERATOR_T(Int32SubWithOverflow)
  DECLARE_GENERATOR_T(Int64AddWithOverflow)
  DECLARE_GENERATOR_T(Int64SubWithOverflow)
  DECLARE_GENERATOR_T(Int64MulWithOverflow)
  DECLARE_GENERATOR_T(Int64Add)
  DECLARE_GENERATOR_T(Word64And)
  DECLARE_GENERATOR_T(Word64Or)
  DECLARE_GENERATOR_T(Word64Xor)
  DECLARE_GENERATOR_T(Int64Sub)
  DECLARE_GENERATOR_T(Int64Mul)
  DECLARE_GENERATOR_T(Int64MulHigh)
  DECLARE_GENERATOR_T(Int64Div)
  DECLARE_GENERATOR_T(Int64Mod)
  DECLARE_GENERATOR_T(Uint64Div)
  DECLARE_GENERATOR_T(Uint64Mod)
  DECLARE_GENERATOR_T(Uint64MulHigh)
  DECLARE_GENERATOR_T(Word32AtomicStore)
  DECLARE_GENERATOR_T(Word64AtomicStore)
  DECLARE_GENERATOR_T(Word32Equal)
  DECLARE_GENERATOR_T(Word64Equal)
  DECLARE_GENERATOR_T(Int32LessThan)
  DECLARE_GENERATOR_T(Int32LessThanOrEqual)
  DECLARE_GENERATOR_T(Int64LessThan)
  DECLARE_GENERATOR_T(Int64LessThanOrEqual)
  DECLARE_GENERATOR_T(Uint32LessThan)
  DECLARE_GENERATOR_T(Uint32LessThanOrEqual)
  DECLARE_GENERATOR_T(Uint64LessThan)
  DECLARE_GENERATOR_T(Uint64LessThanOrEqual)
  DECLARE_GENERATOR_T(Float64Sub)
  DECLARE_GENERATOR_T(Float64Div)
  DECLARE_GENERATOR_T(Float32Equal)
  DECLARE_GENERATOR_T(Float32LessThan)
  DECLARE_GENERATOR_T(Float32LessThanOrEqual)
  DECLARE_GENERATOR_T(Float64Equal)
  DECLARE_GENERATOR_T(Float64LessThan)
  DECLARE_GENERATOR_T(Float64LessThanOrEqual)
  DECLARE_GENERATOR_T(Load)
  DECLARE_GENERATOR_T(StackPointerGreaterThan)
  DECLARE_GENERATOR_T(Store)
  DECLARE_GENERATOR_T(ProtectedStore)
  DECLARE_GENERATOR_T(BitcastTaggedToWord)
  DECLARE_GENERATOR_T(BitcastWordToTagged)
  DECLARE_GENERATOR_T(BitcastSmiToWord)
  DECLARE_GENERATOR_T(ChangeInt32ToInt64)
  DECLARE_GENERATOR_T(ChangeInt32ToFloat64)
  DECLARE_GENERATOR_T(ChangeFloat32ToFloat64)
  DECLARE_GENERATOR_T(RoundFloat64ToInt32)
  DECLARE_GENERATOR_T(TruncateFloat64ToWord32)
  DECLARE_GENERATOR_T(TruncateFloat64ToFloat32)
  DECLARE_GENERATOR_T(TruncateFloat64ToFloat16RawBits)
  DECLARE_GENERATOR_T(TruncateFloat32ToInt32)
  DECLARE_GENERATOR_T(TruncateFloat32ToUint32)
  DECLARE_GENERATOR_T(ChangeFloat16RawBitsToFloat64)
  DECLARE_GENERATOR_T(ChangeFloat64ToInt32)
  DECLARE_GENERATOR_T(ChangeFloat64ToUint32)
  DECLARE_GENERATOR_T(ChangeFloat64ToInt64)
  DECLARE_GENERATOR_T(ChangeFloat64ToUint64)
  DECLARE_GENERATOR_T(TruncateFloat64ToInt64)
  DECLARE_GENERATOR_T(RoundInt32ToFloat32)
  DECLARE_GENERATOR_T(RoundInt64ToFloat32)
  DECLARE_GENERATOR_T(RoundInt64ToFloat64)
  DECLARE_GENERATOR_T(RoundUint32ToFloat32)
  DECLARE_GENERATOR_T(RoundUint64ToFloat32)
  DECLARE_GENERATOR_T(RoundUint64ToFloat64)
  DECLARE_GENERATOR_T(ChangeInt64ToFloat64)
  DECLARE_GENERATOR_T(ChangeUint32ToFloat64)
  DECLARE_GENERATOR_T(ChangeUint32ToUint64)
  DECLARE_GENERATOR_T(Float64ExtractLowWord32)
  DECLARE_GENERATOR_T(Float64ExtractHighWord32)
  DECLARE_GENERATOR_T(Float32Add)
  DECLARE_GENERATOR_T(Float32Sub)
  DECLARE_GENERATOR_T(Float32Mul)
  DECLARE_GENERATOR_T(Float32Div)
  DECLARE_GENERATOR_T(Float32Max)
  DECLARE_GENERATOR_T(Float32Min)
  DECLARE_GENERATOR_T(Float64Atan2)
  DECLARE_GENERATOR_T(Float64Max)
  DECLARE_GENERATOR_T(Float64Min)
  DECLARE_GENERATOR_T(Float64Add)
  DECLARE_GENERATOR_T(Float64Mul)
  DECLARE_GENERATOR_T(Float64Mod)
  DECLARE_GENERATOR_T(Float64Pow)
  DECLARE_GENERATOR_T(BitcastWord32ToWord64)
  DECLARE_GENERATOR_T(BitcastFloat32ToInt32)
  DECLARE_GENERATOR_T(BitcastFloat64ToInt64)
  DECLARE_GENERATOR_T(BitcastInt32ToFloat32)
  DECLARE_GENERATOR_T(BitcastInt64ToFloat64)
  DECLARE_GENERATOR_T(Float32Abs)
  DECLARE_GENERATOR_T(Float32Neg)
  DECLARE_GENERATOR_T(Float32RoundDown)
  DECLARE_GENERATOR_T(Float32RoundTiesEven)
  DECLARE_GENERATOR_T(Float32RoundTruncate)
  DECLARE_GENERATOR_T(Float32RoundUp)
  DECLARE_GENERATOR_T(Float32Sqrt)
  DECLARE_GENERATOR_T(Float64Abs)
  DECLARE_GENERATOR_T(Float64Acos)
  DECLARE_GENERATOR_T(Float64Acosh)
  DECLARE_GENERATOR_T(Float64Asin)
  DECLARE_GENERATOR_T(Float64Asinh)
  DECLARE_GENERATOR_T(Float64Atan)
  DECLARE_GENERATOR_T(Float64Atanh)
  DECLARE_GENERATOR_T(Float64Cbrt)
  DECLARE_GENERATOR_T(Float64Cos)
  DECLARE_GENERATOR_T(Float64Cosh)
  DECLARE_GENERATOR_T(Float64Exp)
  DECLARE_GENERATOR_T(Float64Expm1)
  DECLARE_GENERATOR_T(Float64Log)
  DECLARE_GENERATOR_T(Float64Log1p)
  DECLARE_GENERATOR_T(Float64Log10)
  DECLARE_GENERATOR_T(Float64Log2)
  DECLARE_GENERATOR_T(Float64Neg)
  DECLARE_GENERATOR_T(Float64RoundDown)
  DECLARE_GENERATOR_T(Float64RoundTiesAway)
  DECLARE_GENERATOR_T(Float64RoundTiesEven)
  DECLARE_GENERATOR_T(Float64RoundTruncate)
  DECLARE_GENERATOR_T(Float64RoundUp)
  DECLARE_GENERATOR_T(Float64Sin)
  DECLARE_GENERATOR_T(Float64Sinh)
  DECLARE_GENERATOR_T(Float64Sqrt)
  DECLARE_GENERATOR_T(Float64Tan)
  DECLARE_GENERATOR_T(Float64Tanh)
  DECLARE_GENERATOR_T(Float64SilenceNaN)
  DECLARE_GENERATOR_T(Word32Clz)
  DECLARE_GENERATOR_T(Word32Ctz)
  DECLARE_GENERATOR_T(Word32ReverseBytes)
  DECLARE_GENERATOR_T(Word32Popcnt)
  DECLARE_GENERATOR_T(Word64Popcnt)
  DECLARE_GENERATOR_T(Word64Clz)
  DECLARE_GENERATOR_T(Word64Ctz)
  DECLARE_GENERATOR_T(Word64ReverseBytes)
  DECLARE_GENERATOR_T(SignExtendWord8ToInt32)
  DECLARE_GENERATOR_T(SignExtendWord16ToInt32)
  DECLARE_GENERATOR_T(SignExtendWord8ToInt64)
  DECLARE_GENERATOR_T(SignExtendWord16ToInt64)
  DECLARE_GENERATOR_T(TruncateInt64ToInt32)
  DECLARE_GENERATOR_T(StackSlot)
  DECLARE_GENERATOR_T(LoadRootRegister)
  DECLARE_GENERATOR_T(DebugBreak)
  DECLARE_GENERATOR_T(TryTruncateFloat32ToInt64)
  DECLARE_GENERATOR_T(TryTruncateFloat64ToInt64)
  DECLARE_GENERATOR_T(TryTruncateFloat32ToUint64)
  DECLARE_GENERATOR_T(TryTruncateFloat64ToUint64)
  DECLARE_GENERATOR_T(TryTruncateFloat64ToInt32)
  DECLARE_GENERATOR_T(TryTruncateFloat64ToUint32)
  DECLARE_GENERATOR_T(Int32PairAdd)
  DECLARE_GENERATOR_T(Int32PairSub)
  DECLARE_GENERATOR_T(Int32PairMul)
  DECLARE_GENERATOR_T(Word32PairShl)
  DECLARE_GENERATOR_T(Word32PairShr)
  DECLARE_GENERATOR_T(Word32PairSar)
  DECLARE_GENERATOR_T(Float64InsertLowWord32)
  DECLARE_GENERATOR_T(Float64InsertHighWord32)
  DECLARE_GENERATOR_T(Comment)
  DECLARE_GENERATOR_T(Word32ReverseBits)
  DECLARE_GENERATOR_T(Word64ReverseBits)
  DECLARE_GENERATOR_T(AbortCSADcheck)
  DECLARE_GENERATOR_T(StorePair)
  DECLARE_GENERATOR_T(UnalignedLoad)
  DECLARE_GENERATOR_T(UnalignedStore)
  DECLARE_GENERATOR_T(Int32AbsWithOverflow)
  DECLARE_GENERATOR_T(Int64AbsWithOverflow)
  DECLARE_GENERATOR_T(TruncateFloat64ToUint32)
  DECLARE_GENERATOR_T(SignExtendWord32ToInt64)
  DECLARE_GENERATOR_T(TraceInstruction)
  DECLARE_GENERATOR_T(MemoryBarrier)
  DECLARE_GENERATOR_T(Pause)
  DECLARE_GENERATOR_T(LoadStackCheckOffset)
  DECLARE_GENERATOR_T(LoadFramePointer)
  DECLARE_GENERATOR_T(LoadParentFramePointer)
  DECLARE_GENERATOR_T(ProtectedLoad)
  DECLARE_GENERATOR_T(Word32AtomicAdd)
  DECLARE_GENERATOR_T(Word32AtomicSub)
  DECLARE_GENERATOR_T(Word32AtomicAnd)
  DECLARE_GENERATOR_T(Word32AtomicOr)
  DECLARE_GENERATOR_T(Word32AtomicXor)
  DECLARE_GENERATOR_T(Word32AtomicExchange)
  DECLARE_GENERATOR_T(Word32AtomicCompareExchange)
  DECLARE_GENERATOR_T(Word64AtomicAdd)
  DECLARE_GENERATOR_T(Word64AtomicSub)
  DECLARE_GENERATOR_T(Word64AtomicAnd)
  DECLARE_GENERATOR_T(Word64AtomicOr)
  DECLARE_GENERATOR_T(Word64AtomicXor)
  DECLARE_GENERATOR_T(Word64AtomicExchange)
  DECLARE_GENERATOR_T(Word64AtomicCompareExchange)
  DECLARE_GENERATOR_T(Word32AtomicLoad)
  DECLARE_GENERATOR_T(Word64AtomicLoad)
  DECLARE_GENERATOR_T(Word32AtomicPairLoad)
  DECLARE_GENERATOR_T(Word32AtomicPairStore)
  DECLARE_GENERATOR_T(Word32AtomicPairAdd)
  DECLARE_GENERATOR_T(Word32AtomicPairSub)
  DECLARE_GENERATOR_T(Word32AtomicPairAnd)
  DECLARE_GENERATOR_T(Word32AtomicPairOr)
  DECLARE_GENERATOR_T(Word32AtomicPairXor)
  DECLARE_GENERATOR_T(Word32AtomicPairExchange)
  DECLARE_GENERATOR_T(Word32AtomicPairCompareExchange)
  DECLARE_GENERATOR_T(TaggedAtomicExchange)
  DECLARE_GENERATOR_T(TaggedAtomicCompareExchange)
  DECLARE_GENERATOR_T(Simd128ReverseBytes)
  MACHINE_SIMD128_OP_LIST(DECLARE_GENERATOR_T)
  MACHINE_SIMD256_OP_LIST(DECLARE_GENERATOR_T)
  IF_WASM(DECLARE_GENERATOR_T, LoadStackPointer)
  IF_WASM(DECLARE_GENERATOR_T, SetStackPointer)
#undef DECLARE_GENERATOR_T

  // Visit the load node with a value and opcode to replace with.
  void VisitLoad(turboshaft::OpIndex node, turboshaft::OpIndex value,
                 InstructionCode opcode);
  void VisitParameter(turboshaft::OpIndex node);
  void VisitIfException(turboshaft::OpIndex node);
  void VisitOsrValue(turboshaft::OpIndex node);
  void VisitPhi(turboshaft::OpIndex node);
  void VisitProjection(turboshaft::OpIndex node);
  void VisitConstant(turboshaft::OpIndex node);
  void VisitCall(turboshaft::OpIndex call, turboshaft::Block* handler = {});
  void VisitDeoptimizeIf(turboshaft::OpIndex node);
  void VisitTrapIf(turboshaft::OpIndex node);
  void VisitTailCall(turboshaft::OpIndex call);
  void VisitGoto(turboshaft::Block* target);
  void VisitBranch(turboshaft::OpIndex input, turboshaft::Block* tbranch,
                   turboshaft::Block* fbranch);
  void VisitSwitch(turboshaft::OpIndex node, const SwitchInfo& sw);
  void VisitDeoptimize(DeoptimizeReason reason, uint32_t node_id,
                       FeedbackSource const& feedback,
                       turboshaft::OpIndex frame_state);
  void VisitSelect(turboshaft::OpIndex node);
  void VisitReturn(turboshaft::OpIndex node);
  void VisitRetain(turboshaft::OpIndex node);
  void VisitUnreachable(turboshaft::OpIndex node);
  void VisitStaticAssert(turboshaft::OpIndex node);
  void VisitBitcastWord32PairToFloat64(turboshaft::OpIndex node);

  void TryPrepareScheduleFirstProjection(turboshaft::OpIndex maybe_projection);

  void VisitStackPointerGreaterThan(turboshaft::OpIndex node,
                                    FlagsContinuation* cont);

  void VisitWordCompareZero(turboshaft::OpIndex user, turboshaft::OpIndex value,
                            FlagsContinuation* cont);

  void EmitPrepareArguments(ZoneVector<PushParameter>* arguments,
                            const CallDescriptor* call_descriptor,
                            turboshaft::OpIndex node);
  void EmitPrepareResults(ZoneVector<PushParameter>* results,
                          const CallDescriptor* call_descriptor,
                          turboshaft::OpIndex node);

  // In LOONG64, calling convention uses free GP param register to pass
  // floating-point arguments when no FP param register is available. But
  // gap does not support moving from FPR to GPR, so we add EmitMoveFPRToParam
  // to complete movement.
  void EmitMoveFPRToParam(InstructionOperand* op, LinkageLocation location);
  // Moving floating-point param from GP param register to FPR to participate in
  // subsequent operations, whether CallCFunction or normal floating-point
  // operations.
  void EmitMoveParamToFPR(turboshaft::OpIndex node, int index);

  void AddOutputToSelectContinuation(OperandGenerator* g, int first_input_index,
                                     turboshaft::OpIndex node);

  void ConsumeEqualZero(turboshaft::OpIndex* user, turboshaft::OpIndex* value,
                        FlagsContinuation* cont);

  // ===========================================================================
  // ============= Vector instruction (SIMD) helper fns. =======================
  // ===========================================================================
  void VisitI8x16RelaxedSwizzle(turboshaft::OpIndex node);

#if V8_ENABLE_WEBASSEMBLY
  // Canonicalize shuffles to make pattern matching simpler. Returns the shuffle
  // indices, and a boolean indicating if the shuffle is a swizzle (one input).
  template <const int simd_size = kSimd128Size,
            const int shuffle_size = simd_size>
  void CanonicalizeShuffle(SimdShuffleView& view, uint8_t* shuffle,
                           bool* is_swizzle)
    requires((simd_size == kSimd128Size || simd_size == kSimd256Size) &&
             (simd_size % shuffle_size == 0))
  {
    // Get raw shuffle indices.
    if constexpr (simd_size == kSimd128Size) {
      static_assert(shuffle_size == kSimd128Size ||
                    shuffle_size == kSimd128HalfSize);
      DCHECK(view.isSimd128());
      memcpy(shuffle, view.data(), shuffle_size);
    } else if constexpr (simd_size == kSimd256Size) {
      static_assert(shuffle_size == kSimd256Size);
      DCHECK(!view.isSimd128());
      memcpy(shuffle, view.data(), kSimd256Size);
    } else {
      UNREACHABLE();
    }
    bool needs_swap;
    bool inputs_equal =
        GetVirtualRegister(view.input(0)) == GetVirtualRegister(view.input(1));
    wasm::SimdShuffle::CanonicalizeShuffle<simd_size, shuffle_size>(
        inputs_equal, shuffle, &needs_swap, is_swizzle);
    if (needs_swap) {
      SwapShuffleInputs(view);
    }
    // Duplicate the first input; for some shuffles on some architectures, it's
    // easiest to implement a swizzle as a shuffle so it might be used.
    if (*is_swizzle) {
      view.DuplicateFirstInput();
    }
  }

  // Swaps the two first input operands of the node, to help match shuffles
  // to specific architectural instructions.
  void SwapShuffleInputs(SimdShuffleView& node);

#if V8_ENABLE_WASM_DEINTERLEAVED_MEM_OPS
  void VisitSimd128LoadPairDeinterleave(turboshaft::OpIndex node);
#endif  // V8_ENABLE_WASM_DEINTERLEAVED_MEM_OPS

  void VisitMemoryCopy(turboshaft::OpIndex node);
  void VisitMemoryFill(turboshaft::OpIndex node);

#if V8_ENABLE_WASM_SIMD256_REVEC
  void VisitSimd256LoadTransform(turboshaft::OpIndex node);

#ifdef V8_TARGET_ARCH_X64
  void VisitSimd256Shufd(turboshaft::OpIndex node);
  void VisitSimd256Shufps(turboshaft::OpIndex node);
  void VisitSimd256Unpack(turboshaft::OpIndex node);
  void VisitSimdPack128To256(turboshaft::OpIndex node);
#endif  // V8_TARGET_ARCH_X64
#endif  // V8_ENABLE_WASM_SIMD256_REVEC

#ifdef V8_TARGET_ARCH_X64
  bool CanOptimizeF64x2PromoteLowF32x4(turboshaft::OpIndex node);
#endif

#endif  // V8_ENABLE_WEBASSEMBLY

  // ===========================================================================

  turboshaft::Graph* schedule() const { return schedule_; }
  Linkage* linkage() const { return linkage_; }
  InstructionSequence* sequence() const { return sequence_; }
  base::Vector<const turboshaft::OpIndex> turboshaft_uses(
      turboshaft::OpIndex node) const {
    DCHECK(turboshaft_use_map_.has_value());
    return turboshaft_use_map_->uses(node);
  }
  Zone* instruction_zone() const { return sequence()->zone(); }
  Zone* zone() const { return zone_; }

  void set_instruction_selection_failed() {
    instruction_selection_failed_ = true;
  }
  bool instruction_selection_failed() { return instruction_selection_failed_; }

  FlagsCondition GetComparisonFlagCondition(
      const turboshaft::ComparisonOp& op) const;

  void MarkPairProjectionsAsWord32(turboshaft::OpIndex node);
  bool IsSourcePositionUsed(turboshaft::OpIndex node);
  void VisitWord32AtomicBinaryOperation(turboshaft::OpIndex node,
                                        ArchOpcode int8_op, ArchOpcode uint8_op,
                                        ArchOpcode int16_op,
                                        ArchOpcode uint16_op,
                                        ArchOpcode word32_op);
  void VisitWord64AtomicBinaryOperation(turboshaft::OpIndex node,
                                        ArchOpcode uint8_op,
                                        ArchOpcode uint16_op,
                                        ArchOpcode uint32_op,
                                        ArchOpcode uint64_op);

#if V8_TARGET_ARCH_64_BIT
  bool ZeroExtendsWord32ToWord64(turboshaft::OpIndex node,
                                 int recursion_depth = 0);
  void MarkNodeAsNotZeroExtended(turboshaft::OpIndex node);
  bool ZeroExtendsWord32ToWord64NoPhis(turboshaft::OpIndex node);

  enum class Upper32BitsState : uint8_t {
    kNotYetChecked,
    kZero,
    kMayBeNonZero,
  };
#endif  // V8_TARGET_ARCH_64_BIT

  struct FrameStateInput {
    FrameStateInput(turboshaft::OpIndex node_, FrameStateInputKind kind_)
        : node(node_), kind(kind_) {}

    turboshaft::OpIndex node;
    FrameStateInputKind kind;

    struct Hash {
      size_t operator()(FrameStateInput const& source) const {
        return base::hash_combine(source.node,
                                  static_cast<size_t>(source.kind));
      }
    };

    struct Equal {
      bool operator()(FrameStateInput const& lhs,
                      FrameStateInput const& rhs) const {
        return lhs.node == rhs.node && lhs.kind == rhs.kind;
      }
    };
  };

  struct CachedStateValues;
  class CachedStateValuesBuilder;

  // ===========================================================================

  Zone* const zone_;
  Linkage* const linkage_;
  InstructionSequence* const sequence_;
  source_position_table_t* const source_positions_;
  SourcePositionMode const source_position_mode_;
  Features features_;
  turboshaft::Graph* const schedule_;
  const turboshaft::Block* current_block_;
  ZoneVector<Instruction*> instructions_;
  InstructionOperandVector continuation_inputs_;
  InstructionOperandVector continuation_outputs_;
  InstructionOperandVector continuation_temps_;
  BitVector defined_;
  BitVector used_;
  IntVector effect_level_;
  int current_effect_level_;
  IntVector virtual_registers_;
  IntVector virtual_register_rename_;
  InstructionScheduler* scheduler_;
  EnableScheduling enable_scheduling_;
  EnableRootsRelativeAddressing enable_roots_relative_addressing_;
  EnableSwitchJumpTable enable_switch_jump_table_;
  ZoneUnorderedMap<FrameStateInput, CachedStateValues*,
                   typename FrameStateInput::Hash,
                   typename FrameStateInput::Equal>
      state_values_cache_;

  Frame* frame_;
  bool instruction_selection_failed_;
  ZoneVector<std::pair<int, int>> instr_origins_;
  EnableTraceTurboJson trace_turbo_;
  EnsureDeterministicNan ensure_deterministic_nan_;
  TickCounter* const tick_counter_;
  // The broker is only used for unparking the LocalHeap for diagnostic printing
  // for failed StaticAsserts.
  JSHeapBroker* const broker_;

  // Store the maximal unoptimized frame height and an maximal number of pushed
  // arguments (for calls). Later used to apply an offset to stack checks.
  size_t* max_unoptimized_frame_height_;
  size_t* max_pushed_argument_count_;

  // Turboshaft-adapter only.
  std::optional<turboshaft::UseMap> turboshaft_use_map_;
  std::optional<BitVector> protected_loads_to_remove_;
  std::optional<BitVector> additional_protected_instructions_;

#if V8_TARGET_ARCH_64_BIT
  size_t node_count_;

  // Holds lazily-computed results for whether phi nodes guarantee their upper
  // 32 bits to be zero. Indexed by node ID; nobody reads or writes the values
  // for non-phi nodes.
  ZoneVector<Upper32BitsState> phi_states_;
#endif
};

}  // namespace compiler
}  // namespace internal
}  // namespace v8

#endif  // V8_COMPILER_BACKEND_INSTRUCTION_SELECTOR_H_
