// B-2 baseline (T1) tests - the plan-stage oracle suite (Task T1-B).
//
// WHY THIS FILE EXISTS:
// T1's entire output is the StencilPlan (docs/stencils.md SS7): a pure,
// deterministic function of (verified RBC, StencilSet, options). This suite
// pins that function's observable behavior exactly as frozen in
// include/b2/baseline/{Stencil.h, StencilSet.h, Plan.h, Compiler.h}:
//
//   1. SET CONSTRUCTION  - the v0 manifest is complete, deterministic, and
//                          self-consistent (null stencil, one opcode stencil
//                          per Op, the 7 superinstructions, 16 manifest-only
//                          entries, candidates() ordering, find()).
//   2. AVAILABILITY PINS - the honest v0 map (interp_contract SS6/SS7):
//                          which ops plan today and which wait on runtime
//                          features.
//   3. PLAN BASICS       - header fields, tiling, layout contiguity.
//   4. PATCH COMPUTATION - one pin per PatchSource family the v0 holes use.
//   5. FUSION            - producer-consumer superinstructions, the broken-
//                          link negative, the diagnostic off switch, the
//                          fusion-safety guard, the iinc/goto backedge idiom.
//   6. REFUSALS          - every RefuseReason, each with its documented
//                          detail shape (Amendment A: never a partial plan).
//   7. VERIFYPLAN        - the auditor actually audits: one corruption per
//                          invariant family, each must fire a non-empty
//                          error.
//   8. DEOPT/METADATA    - Trap/Guard/CallException points, plan-allocated
//                          vs rbc-declared ids, exception-edge translation,
//                          poll stack maps (SS9).
//   9. DUMP FORMAT       - determinism, byte-exact golden, shape pins
//                          (Rule 124: the dump is the replay proof).
//  10. BACKEDGE POLLS    - the require_backedge_polls membership check.
//
// DISCIPLINE (docs/cpp26_standards.md Part B):
// - Programs are embedded RBC text (parseRbcText), the same surface the
//   corpus and b2plan use; cases the text grammar cannot spell (deopt ids at
//   the plan-allocated base, duplicate rbc deopt ids) are built as Ins
//   records in C++ (compilePlan accepts any verified Method shape).
// - Every expectation below was probed against the real implementation
//   (b2plan runs recorded in the worklog) before being pinned; where the
//   implementation disagrees with a frozen header, the deviation is pinned
//   AS OBSERVED with a BUG note instead of being silently trusted.
// - Deterministic throughout: no clocks, no addresses, no unordered
//   iteration whose order is observable.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "TestHarness.h"
#include "b2/baseline/Compiler.h"
#include "b2/baseline/Plan.h"
#include "b2/baseline/Stencil.h"
#include "b2/baseline/StencilSet.h"
#include "b2/rbc/Opcode.h"
#include "b2/rbc/Rbc.h"
#include "b2/rbc/RbcText.h"

namespace {

using namespace b2::baseline;
namespace rbc = b2::rbc;
using b2::rbc::parseRbcText;
using b2::rbc::Program;

// ===========================================================================
// Harness helpers
// ===========================================================================

// The v0 manifest is deterministic (Rule 124); one shared instance keeps the
// suite fast without introducing order dependence.
[[nodiscard]] const StencilSet& theSet() {
  static const StencilSet set = builtinStencilSetV0();
  return set;
}

[[nodiscard]] Program parseOk(const std::string& text) {
  auto parsed = parseRbcText(text);
  if (!parsed) {
    b2::test::recordFailure(__FILE__, __LINE__,
                            "program did not parse: " + parsed.error().message);
    return Program{};
  }
  return std::move(*parsed);
}

// Compiles with default options and records a rich failure on refusal.
[[nodiscard]] PlanResult compileOk(const Program& p, std::uint32_t idx,
                                   const char* what) {
  const PlanResult r = compilePlan(p, idx, theSet(), CompileOptions{});
  if (!r.ok) {
    b2::test::recordFailure(
        __FILE__, __LINE__,
        std::string(what) + ": expected a plan, refused: " +
            std::string(refuseReasonName(r.reason)) + " (" + r.detail + ")");
  }
  return r;
}

[[nodiscard]] const PatchValue* findPatch(const StencilInstance& inst,
                                          PatchKind kind, PatchSource src) {
  for (const PatchValue& pv : inst.patch_values) {
    if (pv.kind == kind && pv.source == src) {
      return &pv;
    }
  }
  return nullptr;
}

[[nodiscard]] const StencilInstance* instanceAt(const StencilPlan& plan,
                                                std::uint32_t rbcPc) {
  for (const StencilInstance& inst : plan.instances) {
    if (inst.rbc_pc_start == rbcPc) {
      return &inst;
    }
  }
  return nullptr;
}

// Independent re-implementation of plan invariants 1+2 (Plan.h): the
// instances tile [0, code.size()) in rbc order with no gaps or overlaps,
// output offsets are contiguous starting at 0, and every instance covers
// exactly the instructions its stencil's pattern declares. Deliberately NOT
// delegated to verifyPlan - the suite must not merely trust the auditor.
void checkTiling(const StencilPlan& plan, const rbc::Method& m,
                 const char* what) {
  std::uint32_t expectedPc = 0;
  std::uint32_t expectedOffset = 0;
  for (std::size_t i = 0; i < plan.instances.size(); ++i) {
    const StencilInstance& inst = plan.instances[i];
    if (inst.rbc_pc_start != expectedPc) {
      b2::test::recordFailure(
          __FILE__, __LINE__,
          std::string(what) + ": instance " + std::to_string(i) +
              " starts at rbc pc " + std::to_string(inst.rbc_pc_start) +
              ", expected " + std::to_string(expectedPc) + " (tiling)");
      return;
    }
    if (inst.rbc_pc_end <= inst.rbc_pc_start ||
        inst.rbc_pc_end > m.code.size()) {
      b2::test::recordFailure(
          __FILE__, __LINE__,
          std::string(what) + ": instance " + std::to_string(i) +
              " has broken rbc range [" +
              std::to_string(inst.rbc_pc_start) + "," +
              std::to_string(inst.rbc_pc_end) + ")");
      return;
    }
    if (inst.output_offset != expectedOffset) {
      b2::test::recordFailure(
          __FILE__, __LINE__,
          std::string(what) + ": instance " + std::to_string(i) +
              " output offset " + std::to_string(inst.output_offset) +
              ", expected " + std::to_string(expectedOffset));
      return;
    }
    const StencilDesc& desc = theSet().desc(inst.stencil);
    const std::uint32_t len = inst.rbc_pc_end - inst.rbc_pc_start;
    if (desc.pattern_len != len) {
      b2::test::recordFailure(
          __FILE__, __LINE__,
          std::string(what) + ": instance " + std::to_string(i) +
              " covers " + std::to_string(len) + " instructions but stencil " +
              std::string(desc.name) + " pattern has " +
              std::to_string(desc.pattern_len));
      return;
    }
    expectedPc = inst.rbc_pc_end;
    expectedOffset += inst.output_size;
  }
  CHECK_MSG(expectedPc == m.code.size(),
            std::string(what) + ": instances end at rbc pc " +
                std::to_string(expectedPc) + " but code size is " +
                std::to_string(m.code.size()));
  CHECK_MSG(plan.code_size == expectedOffset,
            std::string(what) + ": code_size " +
                std::to_string(plan.code_size) + " != end of last instance " +
                std::to_string(expectedOffset));
}

[[nodiscard]] std::vector<std::string> splitLines(const std::string& s) {
  std::vector<std::string> lines;
  std::string current;
  for (const char c : s) {
    if (c == '\n') {
      lines.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  // A trailing newline yields no extra (empty) line under this split.
  if (!current.empty()) {
    lines.push_back(current);
  }
  return lines;
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

// ===========================================================================
// Test programs (all probed through b2plan before being pinned; shapes that
// failed verification or planning were adjusted and re-probed - see the
// worklog T1-B entry).
// ===========================================================================

// Straight-line: two iconsts, a trapping idiv, return. code_size 64.
constexpr const char* kStraight =
    ".class Main\n"
    ".method static main ()V\n"
    ".regs 3\n"
    ".locals 0\n"
    "iconst r0 5\n"
    "iconst r1 0\n"
    "idiv r2 r0 r1\n"
    "return\n"
    ".end\n";

// Golden-dump minimal method: iconst 42; ireturn. 16 dump lines.
constexpr const char* kTiny =
    ".class Main\n"
    ".method static f ()I\n"
    ".regs 1\n"
    ".locals 0\n"
    "iconst r0 42\n"
    "ireturn r0\n"
    ".end\n";

// The iload/iload/iadd producer-consumer chain (fusable).
constexpr const char* kFusionPos =
    ".class Main\n"
    ".method static add2 (II)I\n"
    ".regs 3\n"
    ".locals 2\n"
    "iload r0 l0\n"
    "iload r1 l1\n"
    "iadd r2 r0 r1\n"
    "ireturn r2\n"
    ".end\n";

// Same ops, but the iadd reads r2 (iconst) instead of the two loads: the
// useAsA/useAsB links cannot hold, so no fusion.
constexpr const char* kFusionBroken =
    ".class Main\n"
    ".method static add2 (II)I\n"
    ".regs 4\n"
    ".locals 2\n"
    "iconst r2 7\n"
    "iload r0 l0\n"
    "iload r1 l1\n"
    "iadd r3 r2 r2\n"
    "ireturn r3\n"
    ".end\n";

// Fusion-safety guard: the forward ifle targets the iadd at pc 8, strictly
// inside the would-be iload_iload_iadd window [6,9), so the fusion must not
// fire there (the branch still lands on a real instance boundary).
constexpr const char* kFusionGuard =
    ".class Main\n"
    ".method static main ()V\n"
    ".regs 5\n"
    ".locals 2\n"
    "iconst r0 1\n"
    "istore r0 l0\n"
    "iconst r1 2\n"
    "istore r1 l1\n"
    "iconst r4 1\n"
    "ifle r4 Ljoin\n"
    "iload r0 l0\n"
    "iload r1 l1\n"
    "Ljoin:\n"
    "iadd r2 r0 r1\n"
    "istore r2 l0\n"
    "return\n"
    ".end\n";

// The loop idiom: iinc immediately before the backedge goto, with the
// SafepointPoll at the loop head (where require_backedge_polls expects it;
// sum_loop.rbc's shape with the poll immediately before the goto is the
// poll-before-backedge convention b2plan --no-backedge-check documents).
constexpr const char* kLoop =
    ".class Main\n"
    ".method static loop (I)I\n"
    ".regs 1\n"
    ".locals 1\n"
    "Lloop:\n"
    "safepoint_poll\n"
    "iload r0 l0\n"
    "ifle r0 Ldone\n"
    "iinc r0 -1\n"
    "goto Lloop\n"
    "Ldone:\n"
    "ireturn r0\n"
    ".end\n";

// guard_non_null with rbc-declared deopt id 7.
constexpr const char* kGuard =
    ".class Main\n"
    ".method static g (Ljava/lang/Object;)I\n"
    ".regs 2\n"
    ".locals 1\n"
    "aload r0 l0\n"
    "guard_non_null r0 7\n"
    "iconst r1 1\n"
    "ireturn r1\n"
    ".end\n";

// Quickened static call (SS7: imm = MethodId = method-table index).
constexpr const char* kCallQuick =
    ".class Main\n"
    ".method static callee (I)I\n"
    ".regs 1\n"
    ".locals 1\n"
    "iload r0 l0\n"
    "ireturn r0\n"
    ".end\n"
    "\n"
    ".method static caller ()I\n"
    ".regs 3\n"
    ".locals 0\n"
    "iconst r1 7\n"
    "iconst r0 0\n"
    "invokestatic_quick r0 r1 r1 0\n"
    "ireturn r0\n"
    ".end\n";

// try/catch over a trapping idiv: one edge, handler entry at pc 4.
constexpr const char* kTryCatch =
    ".class Main\n"
    ".method static f (I)I\n"
    ".regs 3\n"
    ".locals 1\n"
    ".const c0 = class \"java/lang/ArithmeticException\"\n"
    "iconst r0 10\n"
    "iconst r1 0\n"
    "Ltry:\n"
    "idiv r2 r0 r1\n"
    "Lend:\n"
    "ireturn r2\n"
    "Lh:\n"
    "iconst r2 -1\n"
    "ireturn r2\n"
    ".catch c0 from Ltry to Lend handler Lh\n"
    ".end\n";

// Backward branch to a non-poll target: MissingBackedgePoll.
constexpr const char* kNoPoll =
    ".class Main\n"
    ".method static f (I)I\n"
    ".regs 1\n"
    ".locals 1\n"
    "iload r0 l0\n"
    "Lloop:\n"
    "iinc r0 -1\n"
    "ifgt r0 Lloop\n"
    "ireturn r0\n"
    ".end\n";

// Forward branch to a non-poll target: forward edges are exempt.
constexpr const char* kFwdEdge =
    ".class Main\n"
    ".method static f (I)I\n"
    ".regs 1\n"
    ".locals 1\n"
    "iload r0 l0\n"
    "ifle r0 Lout\n"
    "iinc r0 -1\n"
    "Lout:\n"
    "ireturn r0\n"
    ".end\n";

// Type-broken: iadd on null refs -> rbc::verify fails.
constexpr const char* kBadType =
    ".class Main\n"
    ".method static bad ()I\n"
    ".regs 3\n"
    ".locals 0\n"
    "aconst_null r0 0\n"
    "aconst_null r1 0\n"
    "iadd r2 r0 r1\n"
    "ireturn r2\n"
    ".end\n";

// Un-quickened virtual call: NoStencilForOp (v0 availability map).
constexpr const char* kVirtual =
    ".class Main\n"
    ".method static main ()V\n"
    ".regs 3\n"
    ".locals 0\n"
    ".const c0 = field java/lang/System out Ljava/io/PrintStream;\n"
    ".const c1 = method java/io/PrintStream println (I)V\n"
    "getstatic r0 c0\n"
    "iconst r1 5\n"
    "invokevirtual r2 r0 r1 c1\n"
    "return\n"
    ".end\n";

// aload+getfield (fuses): pins the ConstPoolIndex + pending FieldOffset holes.
// The trailing iconst/iadd/ireturn keep the getter idiom (aload_getfield_
// ireturn) from swallowing the whole method, so instance 0 is exactly the
// two-instruction aload_getfield fusion.
constexpr const char* kGetField =
    ".class Main\n"
    ".method static bump (Ljava/lang/Object;)I\n"
    ".regs 4\n"
    ".locals 1\n"
    ".const c0 = field Main count I\n"
    "aload r0 l0\n"
    "getfield r1 r0 c0\n"
    "iconst r2 1\n"
    "iadd r3 r1 r2\n"
    "ireturn r3\n"
    ".end\n";

// Two-method program with an INSTANCE method: pins method_index/static pins.
constexpr const char* kTwoMethods =
    ".class Main\n"
    ".method static helper (I)I\n"
    ".regs 1\n"
    ".locals 1\n"
    "iload r0 l0\n"
    "iinc r0 3\n"
    "ireturn r0\n"
    ".end\n"
    "\n"
    ".method public pick (I)I\n"
    ".regs 2\n"
    ".locals 2\n"
    "aload r0 l0\n"
    "iload r1 l1\n"
    "ireturn r1\n"
    ".end\n";

// Two trapping ops (idiv + irem): two plan-allocated deopt points, used by
// the duplicate-deopt-id tamper test.
constexpr const char* kTwoTraps =
    ".class Main\n"
    ".method static t (I)I\n"
    ".regs 3\n"
    ".locals 1\n"
    "iload r0 l0\n"
    "iload r1 l0\n"
    "idiv r2 r0 r1\n"
    "irem r2 r2 r1\n"
    "ireturn r2\n"
    ".end\n";

// ===========================================================================
// 1. SET CONSTRUCTION
// ===========================================================================

B2_TEST(baseline_set_deterministic) {
  const StencilSet a = builtinStencilSetV0();
  const StencilSet b = builtinStencilSetV0();
  CHECK(a.stencils.size() == b.stencils.size());
  CHECK(a.version.magic == b.version.magic);
  CHECK(a.version.version == b.version.version);
  CHECK(a.version.target_arch == b.version.target_arch);
  CHECK(a.version.abi_hash == b.version.abi_hash);
  bool identical = true;
  for (std::size_t i = 0; i < a.stencils.size() && i < b.stencils.size(); ++i) {
    const StencilDesc& x = a.stencils[i];
    const StencilDesc& y = b.stencils[i];
    if (x.id != y.id || x.name != y.name || x.category != y.category ||
        x.pattern_len != y.pattern_len || x.flags != y.flags ||
        x.availability != y.availability || x.patch_count != y.patch_count ||
        x.nominal_size != y.nominal_size || x.frame_reads != y.frame_reads ||
        x.frame_writes != y.frame_writes) {
      identical = false;
    }
    for (std::uint8_t k = 0; k < StencilDesc::kMaxPattern; ++k) {
      if (x.pattern[k].op != y.pattern[k].op ||
          x.pattern[k].useAsA != y.pattern[k].useAsA ||
          x.pattern[k].useAsB != y.pattern[k].useAsB ||
          x.pattern[k].thenDstOf != y.pattern[k].thenDstOf) {
        identical = false;
      }
    }
    for (std::uint8_t k = 0; k < StencilDesc::kMaxPatches; ++k) {
      if (x.patches[k].code_offset != y.patches[k].code_offset ||
          x.patches[k].kind != y.patches[k].kind ||
          x.patches[k].width != y.patches[k].width ||
          x.patches[k].source != y.patches[k].source) {
        identical = false;
      }
    }
  }
  CHECK_MSG(identical,
            "two builtinStencilSetV0() builds must be field-identical (Rule 124)");
}

B2_TEST(baseline_set_shape_and_version) {
  const StencilSet& set = theSet();
  // 1 null + one opcode stencil per Op + 7 superinstructions + 16 manifest-only.
  CHECK(set.stencils.size() ==
        1 + rbc::opCount() + 7 + 16);
  // Null stencil entry 0: invalid id, empty name, empty pattern.
  CHECK(!set.stencils[0].id.valid());
  CHECK(set.stencils[0].name.empty());
  CHECK(set.stencils[0].pattern_len == 0);
  // Version fields equal the frozen constants (Stencil Rule 4).
  CHECK(set.version.magic == kStencilSetMagic);
  CHECK(set.version.version == kStencilSetVersionV0);
  CHECK(set.version.target_arch == 0); // 0 = target-neutral descriptors (v0)
  CHECK(set.version.abi_hash == 0);    // no machine bytes yet
}

B2_TEST(baseline_set_opcode_stencil_per_op) {
  const StencilSet& set = theSet();
  for (std::uint16_t o = 0; o < rbc::opCount(); ++o) {
    const rbc::Op op = static_cast<rbc::Op>(o);
    const StencilDesc* found = nullptr;
    int matches = 0;
    for (const StencilDesc& d : set.stencils) {
      if (d.category == StencilCategory::Opcode && d.pattern_len == 1 &&
          d.pattern[0].op == op) {
        ++matches;
        if (found == nullptr) {
          found = &d;
        }
      }
    }
    CHECK_MSG(found != nullptr,
              "no opcode stencil for op " + std::string(rbc::opName(op)));
    CHECK_MSG(matches == 1,
              "op " + std::string(rbc::opName(op)) + " has " +
                  std::to_string(matches) + " opcode stencils, expected 1");
    if (found != nullptr) {
      CHECK_MSG(found->name == rbc::opName(op),
                "opcode stencil for " + std::string(rbc::opName(op)) +
                    " is named " + std::string(found->name));
    }
  }
}

B2_TEST(baseline_set_bounds_and_dense_ids) {
  const StencilSet& set = theSet();
  for (std::size_t i = 0; i < set.stencils.size(); ++i) {
    const StencilDesc& d = set.stencils[i];
    CHECK_MSG(d.patch_count <= StencilDesc::kMaxPatches,
              std::string(d.name) + " declares " +
                  std::to_string(d.patch_count) + " patches > kMaxPatches");
    CHECK_MSG(d.pattern_len <= StencilDesc::kMaxPattern,
              std::string(d.name) + " pattern_len " +
                  std::to_string(d.pattern_len) + " > kMaxPattern");
    CHECK_MSG(d.id.id == i,
              "stencil at index " + std::to_string(i) + " carries id " +
                  std::to_string(d.id.id) + " (Rule 15: dense indices)");
  }
}

B2_TEST(baseline_set_candidates_order) {
  const StencilSet& set = theSet();
  for (std::uint16_t o = 0; o < rbc::opCount(); ++o) {
    const rbc::Op op = static_cast<rbc::Op>(o);
    const auto cands = set.candidates(op);
    CHECK_MSG(!cands.empty(),
              "op " + std::string(rbc::opName(op)) + " has no candidates");
    for (std::size_t i = 1; i < cands.size(); ++i) {
      CHECK_MSG(cands[i - 1].pattern_len >= cands[i].pattern_len,
                "candidates(" + std::string(rbc::opName(op)) +
                    ") not longest-pattern-first at " + std::to_string(i));
      if (cands[i - 1].pattern_len == cands[i].pattern_len) {
        const bool prevAvailable =
            cands[i - 1].availability == StencilAvailability::Available;
        const bool thisAvailable =
            cands[i].availability == StencilAvailability::Available;
        CHECK_MSG(!( !prevAvailable && thisAvailable ),
                  "candidates(" + std::string(rbc::opName(op)) +
                      ") put NeedsRuntimeFeature before an equal-length "
                      "Available stencil at " +
                      std::to_string(i));
      }
    }
  }
  // Concrete shapes: the ops with superinstructions.
  {
    const auto cands = set.candidates(rbc::Op::Iload);
    CHECK(cands.size() == 4); // iload_iload_{iadd,isub,imul} + iload
    CHECK(cands[0].pattern_len == 3);
    CHECK(cands[1].pattern_len == 3);
    CHECK(cands[2].pattern_len == 3);
    CHECK(cands[3].pattern_len == 1);
    CHECK(cands[3].name == "iload");
  }
  {
    const auto cands = set.candidates(rbc::Op::Aload);
    CHECK(cands.size() == 4); // aload_getfield, aload_arraylength_if,
                              // aload_getfield_ireturn + aload
    CHECK(cands[0].pattern_len == 3);
    CHECK(cands[3].pattern_len == 1);
  }
  {
    const auto cands = set.candidates(rbc::Op::Iinc);
    CHECK(cands.size() == 2); // iinc_goto + iinc
    CHECK(cands[0].name == "iinc_goto");
    CHECK(cands[0].pattern_len == 2);
    CHECK(cands[1].name == "iinc");
    CHECK(cands[1].pattern_len == 1);
  }
  {
    // Ops with no superinstruction: exactly their own opcode stencil.
    const auto cands = set.candidates(rbc::Op::Iadd);
    CHECK(cands.size() == 1);
    CHECK(cands[0].name == "iadd");
    CHECK(cands[0].pattern_len == 1);
  }
  {
    // Set v2: the un-quickened virtual call is Available (the T1 helper
    // seam); invokedynamic remains the honest NeedsRuntimeFeature case.
    const auto cands = set.candidates(rbc::Op::Invokevirtual);
    CHECK(cands.size() == 1);
    CHECK(cands[0].pattern_len == 1);
    CHECK(cands[0].availability == StencilAvailability::Available);
    const auto indy = set.candidates(rbc::Op::Invokedynamic);
    CHECK(indy.size() == 1);
    CHECK(indy[0].availability == StencilAvailability::NeedsRuntimeFeature);
  }
}

// The Available-before-NeedsRuntimeFeature clause is unobservable on the
// builtin v0 set (no op mixes availabilities at equal pattern length), so it
// is exercised with a minimal set that follows the documented layout
// discipline (StencilSet.cpp candidates(): sorted by first op, pattern_len
// desc, availability asc, construction order as tie-break).
B2_TEST(baseline_set_candidates_custom_order) {
  StencilSet custom;
  custom.version.magic = kStencilSetMagic;
  custom.version.version = kStencilSetVersionV0;
  custom.stencils.push_back(StencilDesc{}); // entry 0: the null stencil
  StencilDesc available;
  available.name = "iadd";
  available.category = StencilCategory::Opcode;
  available.pattern[0].op = rbc::Op::Iadd;
  available.pattern_len = 1;
  available.availability = StencilAvailability::Available;
  available.nominal_size = 16;
  StencilDesc needs = available;
  needs.name = "iadd_would_be";
  needs.availability = StencilAvailability::NeedsRuntimeFeature;
  custom.stencils.push_back(available);
  custom.stencils.push_back(needs);
  for (std::size_t i = 0; i < custom.stencils.size(); ++i) {
    custom.stencils[i].id = StencilId{static_cast<std::uint32_t>(i)};
  }
  const auto cands = custom.candidates(rbc::Op::Iadd);
  CHECK(cands.size() == 2);
  if (cands.size() == 2) {
    CHECK(cands[0].availability == StencilAvailability::Available);
    CHECK(cands[1].availability == StencilAvailability::NeedsRuntimeFeature);
  }
}

B2_TEST(baseline_set_find) {
  const StencilSet& set = theSet();
  const StencilId iadd = set.find("iadd");
  CHECK(iadd.valid());
  CHECK(set.desc(iadd).category == StencilCategory::Opcode);
  CHECK(set.desc(iadd).pattern_len == 1);
  CHECK(set.desc(iadd).pattern[0].op == rbc::Op::Iadd);

  const StencilId super = set.find("iload_iload_iadd");
  CHECK(super.valid());
  CHECK(set.desc(super).category == StencilCategory::Superinstruction);

  const StencilId callStatic = set.find("call_static");
  CHECK(callStatic.valid());
  CHECK(set.desc(callStatic).category == StencilCategory::Call);
  CHECK(set.desc(callStatic).pattern_len == 0);

  // Documented name collision (StencilSet.cpp manifest comment): the OPCODE
  // stencil for Op::GuardNonNull is also named "guard_non_null" and sorts
  // ahead of the manifest tail, so find() answers the plannable twin.
  const StencilId gnn = set.find("guard_non_null");
  CHECK(gnn.valid());
  CHECK(set.desc(gnn).pattern_len == 1);
  CHECK(set.desc(gnn).pattern[0].op == rbc::Op::GuardNonNull);
  CHECK(set.desc(gnn).category == StencilCategory::Opcode);
  CHECK(set.desc(gnn).availability == StencilAvailability::Available);

  CHECK(!set.find("not_a_stencil").valid());
  CHECK(!set.find("").valid()); // the null stencil answers "" with id 0
}

B2_TEST(baseline_set_superinstructions) {
  const StencilSet& set = theSet();
  struct SuperPin {
    const char* name;
    rbc::Op firstOp;
    std::uint32_t nominalSize;
  };
  const SuperPin pins[] = {
      {"iload_iload_iadd", rbc::Op::Iload, 3 * 16 - 4},
      {"iload_iload_isub", rbc::Op::Iload, 3 * 16 - 4},
      {"iload_iload_imul", rbc::Op::Iload, 3 * 16 - 4},
      {"aload_getfield", rbc::Op::Aload, 2 * 16 - 4},
      {"aload_arraylength_if", rbc::Op::Aload, 3 * 16 - 4},
      {"iinc_goto", rbc::Op::Iinc, 2 * 16 - 4},
      {"aload_getfield_ireturn", rbc::Op::Aload, 3 * 16 - 4},
  };
  int superCount = 0;
  for (const StencilDesc& d : set.stencils) {
    if (d.category == StencilCategory::Superinstruction) {
      ++superCount;
    }
  }
  CHECK(superCount == 7);
  for (const SuperPin& pin : pins) {
    const StencilId id = set.find(pin.name);
    CHECK_MSG(id.valid(), std::string(pin.name) + " missing from the manifest");
    if (!id.valid()) {
      continue;
    }
    const StencilDesc& d = set.desc(id);
    CHECK_MSG(d.category == StencilCategory::Superinstruction,
              std::string(pin.name) + " is not a Superinstruction");
    CHECK_MSG(d.pattern[0].op == pin.firstOp,
              std::string(pin.name) + " does not start with its first op");
    CHECK_MSG(d.availability == StencilAvailability::Available,
              std::string(pin.name) + " must be Available");
    // nominal_size == sum of the parts' opcode stencils minus the fusion
    // saving (kFusionSavingBytes = 4; StencilSet.cpp Rule-23 constants).
    CHECK_MSG(d.nominal_size == pin.nominalSize,
              std::string(pin.name) + " nominal size " +
                  std::to_string(d.nominal_size) + " != " +
                  std::to_string(pin.nominalSize));
  }
  // Fused hole sets are the concatenation of the per-step holes.
  {
    const StencilDesc& d = set.desc(set.find("iload_iload_iadd"));
    CHECK(d.patch_count == 2);
    if (d.patch_count == 2) {
      CHECK(d.patches[0].kind == PatchKind::SlotOffset);
      CHECK(d.patches[0].source == PatchSource::FrameSlot);
      CHECK(d.patches[1].kind == PatchKind::SlotOffset);
      CHECK(d.patches[1].source == PatchSource::FrameSlot);
    }
  }
  {
    const StencilDesc& d = set.desc(set.find("iinc_goto"));
    CHECK(d.patch_count == 2);
    if (d.patch_count == 2) {
      CHECK(d.patches[0].kind == PatchKind::Imm32);
      CHECK(d.patches[0].source == PatchSource::InsImm);
      CHECK(d.patches[1].kind == PatchKind::BranchRel32);
      CHECK(d.patches[1].source == PatchSource::BranchTarget);
    }
  }
  {
    const StencilDesc& d = set.desc(set.find("aload_getfield"));
    CHECK(d.patch_count == 3);
    if (d.patch_count == 3) {
      CHECK(d.patches[0].kind == PatchKind::SlotOffset);
      CHECK(d.patches[0].source == PatchSource::FrameSlot);
      CHECK(d.patches[1].kind == PatchKind::ConstPoolIndex);
      CHECK(d.patches[1].source == PatchSource::CpIndex);
      CHECK(d.patches[2].kind == PatchKind::FieldOffset);
      CHECK(d.patches[2].source == PatchSource::RuntimeField);
    }
  }
}

B2_TEST(baseline_set_manifest_only) {
  const StencilSet& set = theSet();
  // All pattern_len == 0 entries besides the null stencil.
  std::vector<const StencilDesc*> manifest;
  for (const StencilDesc& d : set.stencils) {
    if (d.pattern_len == 0 && d.id.valid()) {
      manifest.push_back(&d);
    }
  }
  CHECK(manifest.size() == 16);
  const char* names[] = {
      "call_static",
      "call_virtual_monomorphic",
      "call_virtual_bimorphic",
      "call_interface_megamorphic",
      "call_invokedynamic",
      "guard_non_null",
      "guard_class_id",
      "deopt_stub_eager",
      "deopt_stub_uncommon_trap",
      "deopt_stub_exception",
      "allocation_slow_path",
      "throw_null_pointer",
      "throw_array_index_oob",
      "throw_arithmetic",
      "class_init_check",
      "safepoint_slow_path",
  };
  for (const char* name : names) {
    bool found = false;
    for (const StencilDesc* d : manifest) {
      if (d->name == name) {
        found = true;
        // Call / Deopt / RuntimeHelper entries are NeedsRuntimeFeature
        // (StencilSet.h: they are the SS3.3/SS3.6/SS3.7 artifacts the
        // instantiator links once machine bytes exist).
        if (d->category == StencilCategory::Call ||
            d->category == StencilCategory::Deopt ||
            d->category == StencilCategory::RuntimeHelper) {
          CHECK_MSG(d->availability == StencilAvailability::NeedsRuntimeFeature,
                    std::string(name) + " must be NeedsRuntimeFeature");
        }
        break;
      }
    }
    CHECK_MSG(found, std::string(name) + " missing from the manifest tail");
  }
  // The standalone Guard-category guard_non_null entry (distinct from its
  // opcode-stencil twin; reachable only by direct iteration because of the
  // documented name collision).
  const StencilDesc* manifestGuard = nullptr;
  for (const StencilDesc* d : manifest) {
    if (d->name == "guard_non_null" && d->category == StencilCategory::Guard) {
      manifestGuard = d;
    }
  }
  CHECK(manifestGuard != nullptr);
  if (manifestGuard != nullptr) {
    // BUG 1 (T1-B report) FIXED by the integrator: the manifest loop now
    // carries a per-spec availability field and guard_non_null is stamped
    // Available per the StencilSet.h spec ("guard_non_null (Available: hole
    // = DeoptId)"). Inert for planning either way (pattern_len == 0 entries
    // are never selected; find() answers the opcode-stencil twin), but the
    // manifest now matches the frozen contract.
    CHECK_MSG(manifestGuard->availability == StencilAvailability::Available,
              "manifest guard_non_null must be Available per StencilSet.h "
              "spec (regression of the T1-B BUG 1 fix)");
  }
}

// ===========================================================================
// 2. AVAILABILITY PINS
//
// The map mirrors interp_contract SS6 (the honest v0 list) and SS7 (the
// quickened-opcode interim pins): what actually runs is what may plan.
// ===========================================================================

B2_TEST(baseline_availability_needs_runtime_feature) {
  const StencilSet& set = theSet();
  struct Pin {
    const char* name;
    const char* why;
  };
  // interp_contract SS6 item 4: invokedynamic raises BootstrapMethodError
  // (verifier-only op); MethodType/MethodHandle ldc raise InternalError (the
  // op-level stencil cannot split constant kinds yet). SS6 item 2:
  // deopt_trap's only path unconditionally enters the deopt runtime, whose
  // stubs are themselves NeedsRuntimeFeature. StencilSet.h: un-quickened
  // invokes need real IC/ABI patching (SS3.3); guard_class needs the
  // KlassId hole resolved against real class ids.
  // Set version 2 (MSG-20260830-004): the un-quickened invoke* and ldc
  // flipped Available (the T1 runtime-helper seam landed); the v1 archive
  // gap made multianewarray the newest NeedsRuntimeFeature entry.
  const Pin pins[] = {
      {"invokedynamic", "SS6 item 4: BootstrapMethodError in v0"},
      {"guard_class", "KlassId hole pending real class-id patching"},
      {"deopt_trap", "SS6 item 2: unconditionally enters the deopt runtime"},
      {"multianewarray",
       "codegen_contract SS12: no archive body in the v1 instantiation"},
  };
  for (const Pin& pin : pins) {
    const StencilId id = set.find(pin.name);
    CHECK_MSG(id.valid(), std::string(pin.name) + " stencil missing");
    if (id.valid()) {
      CHECK_MSG(set.desc(id).availability ==
                    StencilAvailability::NeedsRuntimeFeature,
                std::string(pin.name) + " must be NeedsRuntimeFeature (" +
                    pin.why + ")");
    }
  }
}

B2_TEST(baseline_availability_quickened_available) {
  const StencilSet& set = theSet();
  // interp_contract SS7: the quickened imm encodings are plan-computable -
  // MethodId (method-table index) for static/special, IC site id (= call pc)
  // for virtual/interface, resolved field byte offset (= slot * 16) for the
  // field pair. No cp resolution, no IC fill at plan time.
  const char* names[] = {
      "invokevirtual_quick",   "invokespecial_quick",
      "invokestatic_quick",    "invokeinterface_quick",
      "getfield_quick",        "putfield_quick",
  };
  for (const char* name : names) {
    const StencilId id = set.find(name);
    CHECK_MSG(id.valid(), std::string(name) + " stencil missing");
    if (id.valid()) {
      CHECK_MSG(set.desc(id).availability == StencilAvailability::Available,
                std::string(name) + " must be Available (SS7 interim pins)");
    }
  }
}

B2_TEST(baseline_availability_plain_available) {
  const StencilSet& set = theSet();
  // Set version 2 (MSG-20260830-004): the un-quickened invoke* and ldc are
  // Available now that the T1 runtime-helper seam executes them.
  const char* flipped[] = {
      "ldc",         "invokevirtual", "invokespecial",
      "invokestatic", "invokeinterface",
  };
  for (const char* name : flipped) {
    const StencilId id = set.find(name);
    CHECK_MSG(id.valid(), std::string(name) + " stencil missing");
    if (id.valid()) {
      CHECK_MSG(set.desc(id).availability == StencilAvailability::Available,
                std::string(name) +
                    " must be Available (the T1 helper seam, set v2)");
    }
  }

  // A representative sweep of the ops T1 plans today: arithmetic, control,
  // field/static-field, allocation, monitors, guards, polls, returns.
  const char* names[] = {
      "nop",          "safepoint_poll", "aconst_null", "iconst",
      "fconst",       "lconst",         "dconst",      "iload",
      "lload",        "fload",          "dload",       "aload",
      "istore",       "lstore",         "fstore",      "dstore",
      "astore",       "imove",          "lmove",       "fmove",
      "dmove",        "amove",          "iadd",        "isub",
      "imul",         "idiv",           "irem",        "ineg",
      "ishl",         "ishr",           "iushr",       "iand",
      "ior",          "ixor",           "iinc",        "ladd",
      "fadd",         "dadd",           "icmp",        "lcmp",
      "fcmpl",        "fcmpg",          "dcmpl",      "dcmpg",
      "i2l",          "i2f",            "i2d",         "l2i",
      "f2i",          "d2i",            "i2b",         "i2c",
      "i2s",          "goto",           "ifeq",        "ifne",
      "iflt",         "ifge",           "ifgt",        "ifle",
      "ifnull",       "ifnonnull",      "if_icmpeq",   "if_icmpne",
      "if_icmplt",    "if_icmpge",      "if_icmpgt",   "if_icmple",
      "if_acmpeq",    "if_acmpne",      "tableswitch", "lookupswitch",
      "getfield",     "putfield",       "getstatic",   "putstatic",
      "ldc",          "invokevirtual",  "invokespecial", "invokestatic",
      "invokeinterface",
      "newarray",     "anewarray",      "arraylength", "iaload",
      "laload",       "faload",         "daload",      "aaload",
      "baload",       "caload",         "saload",      "iastore",
      "lastore",      "fastore",        "dastore",     "aastore",
      "bastore",      "castore",        "sastore",
      "new",          "checkcast",      "instanceof",  "monitorenter",
      "monitorexit",  "return",         "ireturn",     "lreturn",
      "freturn",      "dreturn",        "areturn",     "athrow",
      "guard_non_null",
  };
  for (const char* name : names) {
    const StencilId id = set.find(name);
    CHECK_MSG(id.valid(), std::string(name) + " stencil missing");
    if (id.valid()) {
      CHECK_MSG(set.desc(id).availability == StencilAvailability::Available,
                std::string(name) + " must be Available in v0");
    }
  }
}

// ===========================================================================
// 3. PLAN BASICS
// ===========================================================================

B2_TEST(baseline_plan_straight_line) {
  const Program prog = parseOk(kStraight);
  const PlanResult r = compileOk(prog, 0, "straight-line");
  if (!r.ok) {
    return;
  }
  const StencilPlan& plan = r.plan;
  // Header pins.
  CHECK(plan.method_index == 0);
  CHECK(plan.method_name == "main");
  CHECK(plan.method_descriptor == "()V");
  CHECK(plan.method_static);
  CHECK(plan.num_regs == 3);
  CHECK(plan.num_locals == 0);
  CHECK(plan.set_version.magic == kStencilSetMagic);
  CHECK(plan.set_version.version == kStencilSetVersionV0);
  CHECK(plan.entry_native_offset == 0);
  CHECK(plan.code_size == 4 * 16);
  // One instance per instruction, opcode stencils only.
  CHECK(plan.instances.size() == 4);
  CHECK(plan.fusion_count == 0);
  // Invariants 1+2 (asserted independently of verifyPlan).
  checkTiling(plan, prog.methods[0], "straight-line");
  // The auditor agrees.
  const PlanCheckResult check = verifyPlan(plan, prog.methods[0], theSet());
  CHECK_MSG(check.ok, "verifyPlan rejected the straight-line plan: " + check.error);
  // Patch/deopt accounting: two iconst patches, one idiv trap.
  CHECK(plan.patch_count == 2);
  CHECK(plan.deopt_points.size() == 1);
  CHECK(plan.safepoint_count == 0);
}

B2_TEST(baseline_plan_header_fields) {
  const Program prog = parseOk(kTwoMethods);
  // Method 1 is the instance method "pick": pins method_index, static=false,
  // and the frame layout of a `this`-carrying method.
  const PlanResult r = compileOk(prog, 1, "pick");
  if (!r.ok) {
    return;
  }
  const StencilPlan& plan = r.plan;
  CHECK(plan.method_index == 1);
  CHECK(plan.method_name == "pick");
  CHECK(plan.method_descriptor == "(I)I");
  CHECK(!plan.method_static);
  CHECK(plan.num_regs == 2);
  CHECK(plan.num_locals == 2); // this + the int param
  CHECK(plan.entry_native_offset == 0);
  CHECK(plan.code_size == 3 * 16);
  CHECK(plan.instances.size() == 3);
  checkTiling(plan, prog.methods[1], "pick");
  // compilePlanFor resolves the same method by name+descriptor.
  const PlanResult byName =
      compilePlanFor(prog, "pick", "(I)I", theSet(), CompileOptions{});
  CHECK(byName.ok);
  if (byName.ok) {
    CHECK(dumpPlan(byName.plan, theSet()) == dumpPlan(plan, theSet()));
  }
}

B2_TEST(baseline_plan_determinism) {
  const Program prog = parseOk(kLoop);
  const PlanResult a = compileOk(prog, 0, "determinism A");
  const PlanResult b = compileOk(prog, 0, "determinism B");
  if (!a.ok || !b.ok) {
    return;
  }
  // Invariant 7: same RBC + same set + same options => byte-identical plan
  // (the dump is the proof; Rule 124).
  CHECK(dumpPlan(a.plan, theSet()) == dumpPlan(b.plan, theSet()));
  // Structural spot-checks beyond the dump: metadata vectors too.
  CHECK(a.plan.instances.size() == b.plan.instances.size());
  CHECK(a.plan.pc_map.size() == b.plan.pc_map.size());
  CHECK(a.plan.deopt_points.size() == b.plan.deopt_points.size());
  CHECK(a.plan.stack_maps.size() == b.plan.stack_maps.size());
  CHECK(a.plan.code_size == b.plan.code_size);
  CHECK(a.plan.fusion_count == b.plan.fusion_count);
}

// ===========================================================================
// 4. PATCH COMPUTATION
// ===========================================================================

B2_TEST(baseline_patch_iload_slot) {
  const Program prog = parseOk(kFusionPos);
  const PlanResult r = compileOk(prog, 0, "iload patch");
  if (!r.ok) {
    return;
  }
  // The fused instance carries one SlotOffset per iload step, each valued
  // with the step's own slot number (the plan carries the slot; the
  // slot*16 arithmetic lives at instantiation, SS8 frame model).
  const StencilInstance& inst = r.plan.instances[0];
  CHECK(inst.patch_values.size() == 2);
  const PatchValue* p0 = findPatch(inst, PatchKind::SlotOffset, PatchSource::FrameSlot);
  CHECK(p0 != nullptr);
  if (p0 != nullptr) {
    CHECK(!p0->pending_runtime);
    CHECK(p0->value == 0); // iload r0 l0 -> slot 0
  }
  // The second hole is the second iload's slot (1).
  if (inst.patch_values.size() == 2) {
    CHECK(inst.patch_values[1].kind == PatchKind::SlotOffset);
    CHECK(inst.patch_values[1].source == PatchSource::FrameSlot);
    CHECK(inst.patch_values[1].value == 1);
    CHECK(!inst.patch_values[1].pending_runtime);
  }
}

B2_TEST(baseline_patch_iconst_imm) {
  const Program prog = parseOk(kStraight);
  const PlanResult r = compileOk(prog, 0, "iconst patch");
  if (!r.ok) {
    return;
  }
  const StencilInstance& inst = r.plan.instances[0];
  const PatchValue* pv = findPatch(inst, PatchKind::Imm32, PatchSource::InsImm);
  CHECK(pv != nullptr);
  if (pv != nullptr) {
    CHECK(!pv->pending_runtime);
    CHECK(pv->value == 5); // iconst r0 5
    CHECK(pv->value == prog.methods[0].code[0].imm);
  }
}

B2_TEST(baseline_patch_goto_branch_target) {
  const Program prog = parseOk(kFwdEdge);
  const PlanResult r = compileOk(prog, 0, "branch patch");
  if (!r.ok) {
    return;
  }
  // ifle at pc 1 targets Lout at pc 3: the BranchRel32 hole carries the rbc
  // pc (PatchSource::BranchTarget = "imm as an rbc pc").
  const StencilInstance* inst = instanceAt(r.plan, 1);
  CHECK(inst != nullptr);
  const PatchValue* pv =
      inst != nullptr ? findPatch(*inst, PatchKind::BranchRel32, PatchSource::BranchTarget)
                      : nullptr;
  CHECK(pv != nullptr);
  if (pv != nullptr) {
    CHECK(!pv->pending_runtime);
    CHECK(pv->value == 3);
    CHECK(pv->value == prog.methods[0].code[1].imm);
  }
}

B2_TEST(baseline_patch_getfield_cp_and_field) {
  const Program prog = parseOk(kGetField);
  const PlanResult r = compileOk(prog, 0, "getfield patches");
  if (!r.ok) {
    return;
  }
  // aload_getfield fused instance: SlotOffset (receiver slot) +
  // ConstPoolIndex (value == cp index of the FieldRef) + FieldOffset
  // (pending_runtime=true: resolved from cp[imm] at instantiation).
  const StencilInstance& inst = r.plan.instances[0];
  CHECK(inst.rbc_pc_start == 0);
  CHECK(inst.rbc_pc_end == 2);
  CHECK(inst.patch_values.size() == 3);
  const PatchValue* cpv =
      findPatch(inst, PatchKind::ConstPoolIndex, PatchSource::CpIndex);
  CHECK(cpv != nullptr);
  if (cpv != nullptr) {
    CHECK(!cpv->pending_runtime);
    CHECK(cpv->value == 0); // c0
    CHECK(cpv->value == prog.methods[0].code[1].imm);
  }
  const PatchValue* fpv =
      findPatch(inst, PatchKind::FieldOffset, PatchSource::RuntimeField);
  CHECK(fpv != nullptr);
  if (fpv != nullptr) {
    CHECK(fpv->pending_runtime);
    CHECK(fpv->value == 0); // semantic input: the cp index to resolve
  }
}

B2_TEST(baseline_patch_guard_deopt_id) {
  const Program prog = parseOk(kGuard);
  const PlanResult r = compileOk(prog, 0, "guard patch");
  if (!r.ok) {
    return;
  }
  const StencilInstance* inst = instanceAt(r.plan, 1);
  CHECK(inst != nullptr);
  const PatchValue* pv =
      inst != nullptr ? findPatch(*inst, PatchKind::DeoptId, PatchSource::DeoptIdSource)
                      : nullptr;
  CHECK(pv != nullptr);
  if (pv != nullptr) {
    CHECK(!pv->pending_runtime);
    CHECK(pv->value == 7); // guard_non_null r0 7
    CHECK(pv->value == prog.methods[0].code[1].imm);
  }
}

B2_TEST(baseline_patch_getfield_quick) {
  // SS7 pin: getfield_quick's imm IS the resolved byte offset
  // (slot * sizeof(Value) = slot * 16) - plan-computable, no cp, no IC. The
  // PLAN carries the value verbatim; the slot*16 semantics live at
  // instantiation. (In the text grammar a quick field op's imm is spelled as
  // a cN reference whose POOL INDEX equals the offset - the hand-written
  // quickened-stream convention from tests/interp/corpus/quickened.rbc.)
  const std::string text =
      ".class Main\n"
      ".method static bump (Ljava/lang/Object;I)I\n"
      ".regs 3\n"
      ".locals 2\n"
      ".const c0 = class \"Main\"\n"
      "iconst r0 0\n"
      "aload r1 l0\n"
      "getfield_quick r0 r1 c0\n"
      "iload r2 l1\n"
      "iadd r2 r0 r2\n"
      "ireturn r2\n"
      ".end\n";
  const Program prog = parseOk(text);
  const PlanResult r = compileOk(prog, 0, "getfield_quick patch");
  if (!r.ok) {
    return;
  }
  const StencilInstance* inst = instanceAt(r.plan, 2);
  CHECK(inst != nullptr);
  if (inst != nullptr) {
    CHECK(inst->patch_values.size() == 1);
    const PatchValue* pv =
        findPatch(*inst, PatchKind::FieldOffset, PatchSource::InsImm);
    CHECK(pv != nullptr);
    if (pv != nullptr) {
      CHECK(!pv->pending_runtime);
      CHECK(pv->value == prog.methods[0].code[2].imm); // carried verbatim
      CHECK(pv->value == 0);                           // c0 -> offset 0
    }
  }
}

// ===========================================================================
// 5. FUSION
// ===========================================================================

B2_TEST(baseline_fusion_iload_iload_iadd) {
  const Program prog = parseOk(kFusionPos);
  const PlanResult r = compileOk(prog, 0, "fusion positive");
  if (!r.ok) {
    return;
  }
  const StencilPlan& plan = r.plan;
  CHECK(plan.fusion_count == 1);
  CHECK(plan.instances.size() == 2);
  const StencilInstance& fused = plan.instances[0];
  CHECK(theSet().desc(fused.stencil).name == "iload_iload_iadd");
  CHECK(fused.rbc_pc_start == 0);
  CHECK(fused.rbc_pc_end == 3); // covers exactly the 3 instructions
  // Nominal size == parts minus the fusion saving: 3*16 - 4 == 44.
  CHECK(fused.output_size == 44);
  CHECK(fused.output_offset == 0);
  // ireturn follows at the fused boundary.
  CHECK(plan.instances[1].rbc_pc_start == 3);
  CHECK(plan.instances[1].output_offset == 44);
  checkTiling(plan, prog.methods[0], "fusion positive");
}

B2_TEST(baseline_fusion_broken_link) {
  const Program prog = parseOk(kFusionBroken);
  const PlanResult r = compileOk(prog, 0, "fusion broken link");
  if (!r.ok) {
    return;
  }
  const StencilPlan& plan = r.plan;
  // The iadd reads r2 (iconst), not the loads' r0/r1: the useAsA/useAsB
  // producer-consumer links cannot hold, so every instruction plans as its
  // own opcode stencil.
  CHECK(plan.fusion_count == 0);
  CHECK(plan.instances.size() == 5);
  for (const StencilInstance& inst : plan.instances) {
    CHECK(inst.rbc_pc_end - inst.rbc_pc_start == 1);
  }
  checkTiling(plan, prog.methods[0], "fusion broken link");
}

B2_TEST(baseline_fusion_disabled) {
  const Program prog = parseOk(kFusionPos);
  CompileOptions options;
  options.superinstructions = false; // the --no-fusion diagnostic mode
  const PlanResult r = compilePlan(prog, 0, theSet(), options);
  CHECK_MSG(r.ok, "no-fusion mode must still plan: " + r.detail);
  if (!r.ok) {
    return;
  }
  CHECK(r.plan.fusion_count == 0);
  CHECK(r.plan.instances.size() == 4);
  CHECK(r.plan.code_size == 4 * 16);
  checkTiling(r.plan, prog.methods[0], "fusion disabled");
}

B2_TEST(baseline_fusion_guard_branch_target) {
  // The forward ifle targets the iadd at pc 8, strictly inside the would-be
  // iload_iload_iadd window [6,9). PlanBuilder's fusion-safety guard
  // (documented in its file header) suppresses the fusion there: a mid-
  // window branch target would be unpatchable and would break the pc map's
  // single-observable-boundary-at-START pin.
  const Program prog = parseOk(kFusionGuard);
  const PlanResult r = compileOk(prog, 0, "fusion guard");
  if (!r.ok) {
    return;
  }
  const StencilPlan& plan = r.plan;
  CHECK(plan.fusion_count == 0);
  CHECK(plan.instances.size() == 11); // one per instruction
  // The branch target (pc 8) lands on a real instance boundary.
  const StencilInstance* target = instanceAt(plan, 8);
  CHECK(target != nullptr);
  if (target != nullptr) {
    CHECK(theSet().desc(target->stencil).name == "iadd");
    CHECK(target->rbc_pc_start == 8);
  }
  // And a pc_map entry exists for it.
  bool mapped = false;
  for (const PcMapEntry& e : plan.pc_map) {
    if (e.rbc_pc == 8) {
      mapped = true;
    }
  }
  CHECK(mapped);
  checkTiling(plan, prog.methods[0], "fusion guard");
  // Control: the identical shape WITHOUT the branch fuses (kFusionPos), so
  // it is the branch target - not the program shape - that suppresses here.
}

B2_TEST(baseline_fusion_iinc_goto_backedge) {
  const Program prog = parseOk(kLoop);
  const PlanResult r = compileOk(prog, 0, "iinc_goto backedge");
  if (!r.ok) {
    return;
  }
  const StencilPlan& plan = r.plan;
  CHECK(plan.fusion_count == 1);
  // The iinc_goto instance covers [3,5): iinc at pc 3, goto at pc 4.
  const StencilInstance* fused = instanceAt(plan, 3);
  CHECK(fused != nullptr);
  if (fused != nullptr) {
    CHECK(theSet().desc(fused->stencil).name == "iinc_goto");
    CHECK(fused->rbc_pc_start == 3);
    CHECK(fused->rbc_pc_end == 5);
    CHECK(fused->output_size == 2 * 16 - 4); // 28
    // Fused holes: the iinc immediate then the goto target.
    const PatchValue* imm =
        findPatch(*fused, PatchKind::Imm32, PatchSource::InsImm);
    CHECK(imm != nullptr);
    if (imm != nullptr) {
      CHECK(imm->value == 0xFFFFFFFFu); // iinc r0 -1, carried as u32
    }
    const PatchValue* br =
        findPatch(*fused, PatchKind::BranchRel32, PatchSource::BranchTarget);
    CHECK(br != nullptr);
    if (br != nullptr) {
      CHECK(br->value == 0); // goto Lloop -> the poll at rbc pc 0
    }
  }
  // The backedge check passed because the poll sits at the loop head (the
  // backward goto target pc 0 IS the safepoint_poll instruction).
  checkTiling(plan, prog.methods[0], "iinc_goto backedge");
}

// ===========================================================================
// 6. REFUSALS (every RefuseReason pinned; Amendment A: never a partial plan)
// ===========================================================================

B2_TEST(baseline_refuse_unverifiable) {
  const Program prog = parseOk(kBadType);
  const PlanResult r = compilePlan(prog, 0, theSet(), CompileOptions{});
  CHECK(!r.ok);
  CHECK(r.reason == RefuseReason::UnverifiableMethod);
  CHECK(!r.detail.empty());
  CHECK(contains(r.detail, "iadd expects int"));
}

B2_TEST(baseline_refuse_no_stencil) {
  // Set v2 flipped invoke* Available (MSG-20260830-004), so the honest
  // NoStencilForOp fixture is now invokedynamic (still verifier-only).
  constexpr const char* kIndy =
      ".class Main\n"
      ".method static main ()V\n"
      ".regs 3\n"
      ".locals 0\n"
      ".const c0 = indy run ()V\n"
      "iconst r0 1\n"
      "invokedynamic r1 r0 r0 c0\n"
      "return\n"
      ".end\n";
  const Program prog = parseOk(kIndy);
  const PlanResult r = compilePlan(prog, 0, theSet(), CompileOptions{});
  CHECK(!r.ok);
  CHECK(r.reason == RefuseReason::NoStencilForOp);
  CHECK(!r.detail.empty());
  CHECK(contains(r.detail, "invokedynamic"));
  CHECK(contains(r.detail, "pc 1"));
}

B2_TEST(baseline_refuse_missing_backedge_poll) {
  const Program prog = parseOk(kNoPoll);
  const PlanResult r = compilePlan(prog, 0, theSet(), CompileOptions{});
  CHECK(!r.ok);
  CHECK(r.reason == RefuseReason::MissingBackedgePoll);
  CHECK(!r.detail.empty());
  // The target pc appears in the detail.
  CHECK(contains(r.detail, "pc 1"));
  CHECK(contains(r.detail, "safepoint"));
}

B2_TEST(baseline_refuse_bad_operand_range) {
  // The reachable BadOperandRange arm: an rbc-declared deopt id at (or
  // above) kPlanAllocatedDeoptIdBase cannot be plan-represented without
  // colliding with the plan-allocated id space, so the builder refuses.
  // (Built as Ins records, mirroring the T1-A harness: the hand-built form
  // keeps the repro minimal and independent of text-grammar spellings.)
  Program prog = parseOk(kStraight);
  rbc::Method& m = prog.methods[0];
  m.code.clear();
  m.code.push_back(rbc::Ins(rbc::Op::AconstNull, 0, 0, 0, 0));
  m.code.push_back(rbc::Ins(rbc::Op::GuardNonNull, 0, 0, 0,
                            kPlanAllocatedDeoptIdBase));
  m.code.push_back(rbc::Ins(rbc::Op::Return, 0, 0, 0, 0));
  m.numRegs = 1;
  m.numLocals = 0;
  const PlanResult r = compilePlan(prog, 0, theSet(), CompileOptions{});
  CHECK(!r.ok);
  CHECK(r.reason == RefuseReason::BadOperandRange);
  CHECK(!r.detail.empty());
  // NOTE (width arm): the generic fitsWidth() check ("value exceeds hole
  // width") is NOT triggerable from valid RBC text against the v0 manifest:
  // every v0 hole is 4 bytes wide (kW4, StencilSet.cpp) and every plan-
  // computable payload fits - imm is a u32 (max 0xFFFFFFFF == 2^32-1) and
  // register operands are u16. The arm exists for custom sets with narrower
  // holes (e.g. a 12-bit SlotOffset), which the v0 builder never sees; the
  // deopt-id ceiling above is the reachable BadOperandRange path.
}

B2_TEST(baseline_refuse_budget_counted) {
  // Straight-line program: 4 instances, 64 output bytes, 2 patches, 1 deopt
  // point. Three ceilings pinned here, each naming its limit (the refusal
  // is deterministic because the checks run in a fixed order).
  const Program prog = parseOk(kStraight);
  {
    CompileOptions o;
    o.budget.max_instances = 2;
    const PlanResult r = compilePlan(prog, 0, theSet(), o);
    CHECK(!r.ok);
    CHECK(r.reason == RefuseReason::BudgetExceeded);
    CHECK(contains(r.detail, "max_instances"));
    CHECK(contains(r.detail, "limit=2"));
  }
  {
    CompileOptions o;
    o.budget.max_output_bytes = 40; // the 3rd instance would end at 48
    const PlanResult r = compilePlan(prog, 0, theSet(), o);
    CHECK(!r.ok);
    CHECK(r.reason == RefuseReason::BudgetExceeded);
    CHECK(contains(r.detail, "max_output_bytes"));
  }
  {
    CompileOptions o;
    o.budget.max_patches = 1; // the 2nd iconst makes patch total 2
    const PlanResult r = compilePlan(prog, 0, theSet(), o);
    CHECK(!r.ok);
    CHECK(r.reason == RefuseReason::BudgetExceeded);
    CHECK(contains(r.detail, "max_patches"));
  }
  {
    CompileOptions o;
    o.budget.max_deopt_points = 0; // idiv owns a trap deopt point
    const PlanResult r = compilePlan(prog, 0, theSet(), o);
    CHECK(!r.ok);
    CHECK(r.reason == RefuseReason::BudgetExceeded);
    CHECK(contains(r.detail, "max_deopt_points"));
  }
}

B2_TEST(baseline_refuse_budget_fusions) {
  const Program prog = parseOk(kLoop);
  {
    CompileOptions o;
    o.budget.max_fusions = 0; // the iinc_goto fusion is the first emit > 1
    const PlanResult r = compilePlan(prog, 0, theSet(), o);
    CHECK(!r.ok);
    CHECK(r.reason == RefuseReason::BudgetExceeded);
    CHECK(contains(r.detail, "max_fusions"));
  }
  {
    CompileOptions o; // default budget: the same program plans and fuses
    const PlanResult r = compilePlan(prog, 0, theSet(), o);
    CHECK_MSG(r.ok, "default budget must plan the loop: " + r.detail);
    if (r.ok) {
      CHECK(r.plan.fusion_count == 1);
    }
  }
}

B2_TEST(baseline_refuse_internal_invariant) {
  const Program prog = parseOk(kStraight);
  {
    // Method index out of range: an internal-caller error.
    const PlanResult r = compilePlan(prog, 99, theSet(), CompileOptions{});
    CHECK(!r.ok);
    CHECK(r.reason == RefuseReason::InternalInvariant);
    CHECK(contains(r.detail, "out of range"));
  }
  {
    // compilePlanFor with a method that does not exist.
    const PlanResult r =
        compilePlanFor(prog, "nope", "()V", theSet(), CompileOptions{});
    CHECK(!r.ok);
    CHECK(r.reason == RefuseReason::InternalInvariant);
    CHECK(contains(r.detail, "method not found"));
  }
  {
    // Two guards sharing one rbc deopt id: plan invariant 4 (unique ids)
    // cannot be upheld, so the builder refuses loudly instead of emitting a
    // plan the auditor would reject.
    Program p = parseOk(kStraight);
    rbc::Method& m = p.methods[0];
    m.code.clear();
    m.code.push_back(rbc::Ins(rbc::Op::AconstNull, 0, 0, 0, 0));
    m.code.push_back(rbc::Ins(rbc::Op::GuardNonNull, 0, 0, 0, 3));
    m.code.push_back(rbc::Ins(rbc::Op::GuardNonNull, 0, 0, 0, 3));
    m.code.push_back(rbc::Ins(rbc::Op::Return, 0, 0, 0, 0));
    m.numRegs = 1;
    m.numLocals = 0;
    const PlanResult r = compilePlan(p, 0, theSet(), CompileOptions{});
    CHECK(!r.ok);
    CHECK(r.reason == RefuseReason::InternalInvariant);
    CHECK(contains(r.detail, "duplicate rbc deopt id"));
  }
  {
    // Control: distinct guard ids plan fine with two Guard points in id
    // order (rbc-declared, below the plan-allocated base).
    Program p = parseOk(kStraight);
    rbc::Method& m = p.methods[0];
    m.code.clear();
    m.code.push_back(rbc::Ins(rbc::Op::AconstNull, 0, 0, 0, 0));
    m.code.push_back(rbc::Ins(rbc::Op::GuardNonNull, 0, 0, 0, 3));
    m.code.push_back(rbc::Ins(rbc::Op::GuardNonNull, 0, 0, 0, 4));
    m.code.push_back(rbc::Ins(rbc::Op::Return, 0, 0, 0, 0));
    m.numRegs = 1;
    m.numLocals = 0;
    const PlanResult r = compilePlan(p, 0, theSet(), CompileOptions{});
    CHECK(r.ok);
    if (r.ok) {
      CHECK(r.plan.deopt_points.size() == 2);
      if (r.plan.deopt_points.size() == 2) {
        CHECK(r.plan.deopt_points[0].deopt_id == 3);
        CHECK(r.plan.deopt_points[1].deopt_id == 4);
        CHECK(r.plan.deopt_points[0].reason == DeoptReason::Guard);
        CHECK(r.plan.deopt_points[1].reason == DeoptReason::Guard);
      }
    }
  }
}

// ===========================================================================
// 7. verifyPlan NEGATIVES (the auditor actually audits)
// ===========================================================================

B2_TEST(baseline_verify_negatives_layout) {
  const Program prog = parseOk(kStraight);
  const PlanResult r = compileOk(prog, 0, "layout tamper base");
  if (!r.ok) {
    return;
  }
  StencilPlan tampered = r.plan;
  tampered.instances[1].output_offset += 4; // breaks layout (and pc map)
  const PlanCheckResult check =
      verifyPlan(tampered, prog.methods[0], theSet());
  CHECK(!check.ok);
  CHECK(!check.error.empty());
  CHECK(contains(check.error, "output offset"));
}

B2_TEST(baseline_verify_negatives_coverage) {
  const Program prog = parseOk(kFusionPos);
  const PlanResult r = compileOk(prog, 0, "coverage tamper base");
  if (!r.ok) {
    return;
  }
  // Shrink the fused instance's rbc_pc_end: it now covers 2 instructions
  // while its stencil pattern declares 3 (and the tiling would gap).
  StencilPlan tampered = r.plan;
  tampered.instances[0].rbc_pc_end -= 1;
  const PlanCheckResult check =
      verifyPlan(tampered, prog.methods[0], theSet());
  CHECK(!check.ok);
  CHECK(!check.error.empty());
  CHECK(contains(check.error, "instance 0"));
}

B2_TEST(baseline_verify_negatives_pc_map) {
  const Program prog = parseOk(kStraight);
  const PlanResult r = compileOk(prog, 0, "pc map tamper base");
  if (!r.ok) {
    return;
  }
  // Duplicate entry 0 over entry 1: the map no longer matches its instance.
  StencilPlan tampered = r.plan;
  CHECK(tampered.pc_map.size() == 4);
  if (tampered.pc_map.size() == 4) {
    tampered.pc_map[1] = tampered.pc_map[0];
    const PlanCheckResult check =
        verifyPlan(tampered, prog.methods[0], theSet());
    CHECK(!check.ok);
    CHECK(!check.error.empty());
    CHECK(contains(check.error, "pc_map"));
  }
}

B2_TEST(baseline_verify_negatives_deopt_ids) {
  const Program prog = parseOk(kTwoTraps);
  const PlanResult r = compileOk(prog, 0, "deopt id tamper base");
  if (!r.ok) {
    return;
  }
  CHECK(r.plan.deopt_points.size() == 2);
  if (r.plan.deopt_points.size() != 2) {
    return;
  }
  StencilPlan tampered = r.plan;
  tampered.deopt_points[1].deopt_id = tampered.deopt_points[0].deopt_id;
  const PlanCheckResult check =
      verifyPlan(tampered, prog.methods[0], theSet());
  CHECK(!check.ok);
  CHECK(!check.error.empty());
  CHECK(contains(check.error, "duplicate deopt id"));
}

B2_TEST(baseline_verify_negatives_exc_edge) {
  const Program prog = parseOk(kTryCatch);
  const PlanResult r = compileOk(prog, 0, "exc edge tamper base");
  if (!r.ok) {
    return;
  }
  CHECK(r.plan.exception_edges.size() == 1);
  if (r.plan.exception_edges.empty()) {
    return;
  }
  StencilPlan tampered = r.plan;
  tampered.exception_edges[0].native_handler = 0; // wrong native offset
  const PlanCheckResult check =
      verifyPlan(tampered, prog.methods[0], theSet());
  CHECK(!check.ok);
  CHECK(!check.error.empty());
  CHECK(contains(check.error, "native handler"));
}

B2_TEST(baseline_verify_negatives_patch_sites) {
  const Program prog = parseOk(kStraight);
  const PlanResult r = compileOk(prog, 0, "patch tamper base");
  if (!r.ok) {
    return;
  }
  // Extra patch value with an out-of-range site index: the 1:1 hole/value
  // alignment check fires.
  {
    StencilPlan tampered = r.plan;
    PatchValue extra;
    extra.site = 5; // >= the iconst stencil's patch_count (1)
    extra.kind = PatchKind::Imm32;
    extra.source = PatchSource::InsImm;
    tampered.instances[0].patch_values.push_back(extra);
    const PlanCheckResult check =
        verifyPlan(tampered, prog.methods[0], theSet());
    CHECK(!check.ok);
    CHECK(!check.error.empty());
    CHECK(contains(check.error, "patch values"));
  }
  // Corrupt the site index of an existing value: the per-site index check.
  {
    StencilPlan tampered = r.plan;
    CHECK(!tampered.instances[0].patch_values.empty());
    if (!tampered.instances[0].patch_values.empty()) {
      tampered.instances[0].patch_values[0].site = 5;
      const PlanCheckResult check =
          verifyPlan(tampered, prog.methods[0], theSet());
      CHECK(!check.ok);
      CHECK(!check.error.empty());
      CHECK(contains(check.error, "site index"));
    }
  }
}

// ===========================================================================
// 8. DEOPT / STACKMAP / EXCEPTION METADATA (SS9)
// ===========================================================================

B2_TEST(baseline_deopt_trap_idiv) {
  const Program prog = parseOk(kStraight);
  const PlanResult r = compileOk(prog, 0, "idiv trap point");
  if (!r.ok) {
    return;
  }
  const StencilPlan& plan = r.plan;
  CHECK(plan.deopt_points.size() == 1);
  if (plan.deopt_points.size() == 1) {
    const DeoptPoint& dp = plan.deopt_points[0];
    CHECK(dp.reason == DeoptReason::Trap);
    CHECK(dp.deopt_id >= kPlanAllocatedDeoptIdBase);
    CHECK(dp.deopt_id == kPlanAllocatedDeoptIdBase); // first plan-allocated
    CHECK(dp.rbc_pc == 2);                           // the idiv
    CHECK(dp.native_offset == 32);                   // its instance offset
    CHECK(!dp.pending_exception_possible);
  }
}

B2_TEST(baseline_deopt_guard_point) {
  const Program prog = parseOk(kGuard);
  const PlanResult r = compileOk(prog, 0, "guard deopt point");
  if (!r.ok) {
    return;
  }
  const StencilPlan& plan = r.plan;
  CHECK(plan.deopt_points.size() == 1);
  if (plan.deopt_points.size() == 1) {
    const DeoptPoint& dp = plan.deopt_points[0];
    CHECK(dp.reason == DeoptReason::Guard);
    CHECK(dp.deopt_id == 7); // the RBC-declared imm
    CHECK(dp.deopt_id < kPlanAllocatedDeoptIdBase);
    CHECK(dp.rbc_pc == 1);
    CHECK(dp.native_offset == 16);
    CHECK(!dp.pending_exception_possible);
    CHECK(dp.stack_map == 0xFFFF'FFFFu); // guards are not safepoints
  }
}

B2_TEST(baseline_deopt_call_exception) {
  const Program prog = parseOk(kCallQuick);
  const PlanResult r = compileOk(prog, 1, "call exception point");
  if (!r.ok) {
    return;
  }
  const StencilPlan& plan = r.plan;
  CHECK(plan.deopt_points.size() == 1);
  CHECK(plan.stack_maps.size() == 1);
  if (plan.deopt_points.size() == 1 && plan.stack_maps.size() == 1) {
    const DeoptPoint& dp = plan.deopt_points[0];
    CHECK(dp.reason == DeoptReason::CallException);
    CHECK(dp.pending_exception_possible); // re-enters the exception algorithm
    CHECK(dp.deopt_id >= kPlanAllocatedDeoptIdBase);
    CHECK(dp.rbc_pc == 2); // the invokestatic_quick
    const StackMapPoint& sm = plan.stack_maps[0];
    CHECK(sm.kind == StackMapPoint::Kind::Call);
    CHECK(sm.native_offset == dp.native_offset);
    CHECK(sm.rbc_pc == 2);
    CHECK(dp.stack_map == 0); // linked to its Call stack map
  }
}

B2_TEST(baseline_exc_edges_translation) {
  const Program prog = parseOk(kTryCatch);
  const PlanResult r = compileOk(prog, 0, "exception edges");
  if (!r.ok) {
    return;
  }
  const StencilPlan& plan = r.plan;
  CHECK(plan.exception_edges.size() == 1);
  if (plan.exception_edges.size() == 1) {
    const ExceptionEdge& edge = plan.exception_edges[0];
    CHECK(edge.start == 2); // the idiv
    CHECK(edge.end == 3);
    CHECK(edge.handler_pc == 4);
    CHECK(edge.catch_type == 0); // cp index of the ArithmeticException class
    // native_handler == output_offset of the handler's first instance.
    const StencilInstance* handler = instanceAt(plan, edge.handler_pc);
    CHECK(handler != nullptr);
    if (handler != nullptr) {
      CHECK(edge.native_handler == handler->output_offset);
      CHECK(edge.native_handler == 64);
    }
    // The CanTrap idiv instance lists the covering edge.
    const StencilInstance* idiv = instanceAt(plan, 2);
    CHECK(idiv != nullptr);
    if (idiv != nullptr) {
      CHECK(hasFlag(theSet().desc(idiv->stencil).flags, StencilFlag::CanTrap));
      CHECK(idiv->exc_covers.size() == 1);
      if (idiv->exc_covers.size() == 1) {
        CHECK(idiv->exc_covers[0] == 0);
      }
    }
    // Instances OUTSIDE the range carry no coverage.
    const StencilInstance* ret = instanceAt(plan, 3);
    CHECK(ret != nullptr);
    if (ret != nullptr) {
      CHECK(ret->exc_covers.empty());
    }
  }
}

B2_TEST(baseline_stackmap_poll) {
  const Program prog = parseOk(kLoop);
  const PlanResult r = compileOk(prog, 0, "poll stack map");
  if (!r.ok) {
    return;
  }
  const StencilPlan& plan = r.plan;
  CHECK(plan.stack_maps.size() == 1);
  CHECK(plan.safepoint_count == 1);
  if (plan.stack_maps.size() == 1) {
    const StackMapPoint& sm = plan.stack_maps[0];
    CHECK(sm.kind == StackMapPoint::Kind::Poll);
    CHECK(sm.native_offset == 0); // the safepoint_poll instance
    CHECK(sm.rbc_pc == 0);
  }
}

// ===========================================================================
// 9. DUMP FORMAT (Rule 124: the dump is the golden-test replay proof)
// ===========================================================================

B2_TEST(baseline_dump_determinism) {
  const Program prog = parseOk(kLoop);
  const PlanResult r = compileOk(prog, 0, "dump determinism");
  if (!r.ok) {
    return;
  }
  const std::string a = dumpPlan(r.plan, theSet());
  const std::string b = dumpPlan(r.plan, theSet());
  CHECK(a == b);
}

B2_TEST(baseline_dump_golden) {
  // Hand-computed golden for the tiny method (iconst 42; ireturn): 16 lines.
  // Every figure is derivable: two opcode stencils at 16 bytes each; the
  // iconst patch carries imm 42 = 0x2a; no traps, no safepoints, no edges.
  const Program prog = parseOk(kTiny);
  const PlanResult r = compileOk(prog, 0, "dump golden");
  if (!r.ok) {
    return;
  }
  const std::string want =
      "plan method=f descriptor=()I static=1 index=0\n"
      "frame regs=1 locals=0\n"
      "set magic=0x32737463 version=2 target_arch=0 abi_hash=0x0\n"
      "summary code_size=32 entry_native_offset=0 instances=2 fusions=0 "
      "patches=1 deopt_points=0 safepoints=0\n"
      "  inst 0 @0+16 iconst rbc[0,1) patches:1 exc:\n"
      "    patch 0 imm32 <- ins-imm = 0x2a\n"
      "  inst 1 @16+16 ireturn rbc[1,2) patches:0 exc:\n"
      "pc_map:\n"
      "  pcmap 0 native=0 rbc=0 inst=0\n"
      "  pcmap 1 native=16 rbc=1 inst=1\n"
      "stack_maps:\n"
      "deopt_points:\n"
      "exception_edges:\n"
      "end plan instances=2 pc_map=2 stack_maps=0 deopt_points=0 "
      "exception_edges=0\n";
  const std::string got = dumpPlan(r.plan, theSet());
  CHECK_MSG(got == want,
            "golden dump mismatch:\n--- got ---\n" + got + "--- want ---\n" + want);
}

B2_TEST(baseline_dump_shape) {
  const Program prog = parseOk(kTwoMethods);
  const PlanResult r = compileOk(prog, 1, "dump shape");
  if (!r.ok) {
    return;
  }
  const std::string dump = dumpPlan(r.plan, theSet());
  const std::vector<std::string> lines = splitLines(dump);

  // Exact header lines.
  CHECK(lines.size() >= 4);
  if (lines.size() >= 4) {
    CHECK(lines[0] == "plan method=pick descriptor=(I)I static=0 index=1");
    CHECK(lines[1] == "frame regs=2 locals=2");
    CHECK(lines[2] == "set magic=0x32737463 version=2 target_arch=0 abi_hash=0x0");
    CHECK(lines[3].starts_with("summary code_size=48 entry_native_offset=0 "
                               "instances=3 fusions=0 patches=2 deopt_points=0 "
                               "safepoints=0"));
  }

  // Instance lines carry @offset+size, stencil name, rbc range, patch count.
  const std::string instLine = "  inst 0 @0+16 aload rbc[0,1) patches:1 exc:";
  bool found = false;
  for (const std::string& line : lines) {
    if (line == instLine) {
      found = true;
    }
  }
  CHECK_MSG(found, "expected instance line missing: " + instLine);

  // No trailing whitespace anywhere; ends with exactly one final newline.
  for (const std::string& line : lines) {
    CHECK_MSG(line.empty() || (line.back() != ' ' && line.back() != '\t'),
              "trailing whitespace in dump line: \"" + line + "\"");
  }
  CHECK(!dump.empty());
  if (!dump.empty()) {
    CHECK(dump.back() == '\n');
    if (dump.size() >= 2) {
      CHECK(dump[dump.size() - 2] != '\n'); // no blank final line
    }
  }
}

// ===========================================================================
// 10. BACKEDGE POLL CHECK
// ===========================================================================

B2_TEST(baseline_backedge_poll_disabled_ok) {
  // The same un-polled backward branch plans fine once the membership check
  // is off (b2plan's --no-backedge-check: programs lowered with the
  // poll-before-backedge convention instead of poll-at-loop-head).
  const Program prog = parseOk(kNoPoll);
  CompileOptions options;
  options.require_backedge_polls = false;
  const PlanResult r = compilePlan(prog, 0, theSet(), options);
  CHECK_MSG(r.ok, "no-backedge-check must plan the loop: " + r.detail);
  if (r.ok) {
    checkTiling(r.plan, prog.methods[0], "backedge check off");
  }
}

B2_TEST(baseline_backedge_poll_forward_exempt) {
  // Forward edges are exempt: the ifle targets a later, non-poll pc.
  const Program prog = parseOk(kFwdEdge);
  const PlanResult r = compileOk(prog, 0, "forward edge exempt");
  if (!r.ok) {
    return;
  }
  checkTiling(r.plan, prog.methods[0], "forward edge exempt");
}

}  // namespace
