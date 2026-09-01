#pragma once
// B-2 Interpreter - the T0 direct-threaded register interpreter: public API
// and the v0 execution contract.
//
// WHY THIS FILE EXISTS:
// T0 is the baseline execution engine, the universal correctness fallback
// (Rule 96), the reference implementation against the Java semantic oracle
// (Rule 67, Rule 133), and the deopt target of every compiled tier (Rule 4).
// This header pins, in one place, everything a consumer tier or a test needs
// to rely on: the run API, the frame model (Frame.h), the value model
// (Value.h), and - as normative comments - THE DISPATCH LOOP ALGORITHM,
// THE CALL PROTOCOL, THE EXCEPTION ALGORITHM, and the INTERIM QUICKENED-OPCODE
// PINS. docs/interp_contract.md is the prose form of this contract; the two
// change together (contract version bump + ADVISORY to all tiers).
//
// ===========================================================================
// THE DISPATCH LOOP ALGORITHM (normative v0)
// ===========================================================================
// The core is ONE loop over an EXPLICIT frame stack (never C++ recursion per
// Java call - StackOverflowError must be a Java-visible trap, not a C++
// crash, and deopt needs materializable frames):
//
//   run(entry, args):
//     1. HARD GATE: verify every method of the program (rbc::verify).
//        Any diagnostic -> RunStatus::VerifyFailed. (Law: verifier before
//        execution; the interpreter never executes unverified RBC.)
//     2. Resolve the entry method by name+descriptor; missing ->
//        Threw(java/lang/NoSuchMethodError). Push Frame{locals=args
//        (receiver first for instance methods), regs=Bottom x numRegs,
//        pc=0}; bump the invocation counter; enter the receiver's monitor
//        (or the class object's for a synchronized static) per JVMS 8.4.3.6.
//     3. FETCH: fr = frames.back(); ins = fr.method->code[fr.pc].
//        DISPATCH: switch (ins.op) - all 150 opcodes, dense jump table.
//     4. Each opcode handler either (a) computes into regs/locals and
//        advances fr.pc++ (fall-through pc+1, rbc_spec.md SS2.2), (b) sets
//        fr.pc to a branch/switch target, (c) returns, or (d) traps.
//     5. Every opcode that can trap funnels into THE EXCEPTION ALGORITHM.
//
// Dispatch threading (charter: direct-threaded, minimal overhead): TWO
// forms over ONE set of handler bodies (interp_contract.md SS4, v1.1.0).
// The portable token-threaded core - fetch op, indirect jump through the
// compiler's dense switch jump table, handler, next fetch - is ALWAYS
// buildable and is the exhaustiveness proof (no default clause; -Wswitch).
// The computed-goto core (labels-as-values; the v1 milestone, LANDED) is
// selected at build time with B2_INTERP_COMPUTED_GOTO (default ON for
// GNU/Clang; the TU compiles as gnu++26): a designated-initializer label
// table plus per-handler-tail dispatch, so the branch predictor learns
// per-opcode transitions. Semantics are identical by construction; the
// portable test binary runs the identical corpus against the identical
// goldens as the differential proof (Law 36).
//
// Hot-path discipline (Rules 6/7/8/9/16/118): the dispatch switch and its
// fast paths contain no allocation, no C++ exceptions, no RTTI, no locks,
// no strings, no shared_ptr/function. Allocation happens only at calls
// (frame push), traps (exception objects), and IC misses. The verifier's
// guarantees (register/slot/cp bounds, types) are NOT re-checked on the
// fast path; defense-in-depth B2_ASSERTs fire in debug builds and release
// converts invariant violations into java/lang/InternalError results
// (total, never a crash - the verifier's discipline, Rule 47).
//
// ===========================================================================
// THE CALL PROTOCOL (normative)
// ===========================================================================
// For every invoke* (rbc_spec.md SS3.15): args occupy consecutive registers
// a..a+b-1 (receiver = a[0] for virtual/special/interface; none for static);
// dst receives the result (unused when the callee is void).
//
//   1. Resolution order for the target:
//      invokevirtual/invokeinterface: inline cache first - site key is
//        (caller MethodId, call pc); hit -> target MethodId; miss ->
//        builtin probe (receiver class) -> builtin execution; else resolve
//        the MethodRef, fill the IC, execute. (v0 world: no class hierarchy,
//        so virtual dispatch resolves by (name, descriptor) - the IC seam
//        is where real vtable/itable dispatch plugs in.)
//      invokespecial/invokestatic: resolve the MethodRef (no IC needed;
//        their target is static). <init> and <clinit> resolve like any
//        other method.
//      invoke*_quick: imm carries the resolved id directly (pins below).
//      invokedynamic: Threw(java/lang/BootstrapMethodError) - v0 is
//        verifier-only (rbc_spec.md SS10).
//   2. Class initialization (JVMS 5.5): invokestatic, getstatic, putstatic,
//      new, and ldc-of-Class on a not-yet-initialized class push the
//      class's <clinit>()V frame WITHOUT advancing the current pc, after
//      marking the class initialized; when <clinit> returns, the original
//      instruction re-executes and proceeds. Recursion terminates because
//      the class is marked before running.
//   3. Push: Frame{method=target, pc=0, locals=arg values in order,
//      regs=Bottom x numRegs}; the CALLER's pc stays AT the invoke (it is
//      advanced only on normal return) so exception coverage is checked at
//      the call site (JVMS: the call site is the throwing location).
//      Frame depth limit: pushing past maxFrames traps
//      java/lang/StackOverflowError (checked BEFORE the push, at the call).
//   4. Normal return: pop the frame; release the callee frame's remaining
//      monitors (v0 divergence: balanced streams are unaffected, unbalanced
//      ones become well-defined - balance checking is deferred, rbc_spec.md
//      SS10); write the returned Value into the caller's dst iff the return
//      descriptor is non-void; caller pc = callPc + 1; bump nothing.
//   5. Arguments are READ, never clobbered (frame residency, SS1.3).
//
// ===========================================================================
// THE EXCEPTION ALGORITHM (normative; Rule 74 - exceptions are values)
// ===========================================================================
//   throwException(obj):            [obj non-null; athrow traps NPE itself]
//     loop over the frame stack, innermost first:
//       fr = top; for each ExceptionHandler h of fr.method, IN TABLE ORDER,
//         where h.start <= fr.pc < h.end (the invoke's pc for a caller):
//           if h.catchType < 0 (catch-all) or
//              isAssignableFrom(catchClass, classOf(obj)): CAUGHT ->
//                fr.pc = h.handler
//                fr.regs = Bottom x numRegs (ALL registers die, SS5.3)
//                fr.regs[0] = Ref(obj) (exception delivery, SS1.3)
//                pending exception cleared; CONTINUE DISPATCH.
//       not caught: release fr's monitors LIFO (best-effort; failures
//         during unwind-release are swallowed - the original exception
//         wins), pop fr.
//     stack empty -> RunStatus::Threw with obj (uncaught; the caller/tool
//     reports "Exception in thread \"main\" <class>: <message>").
//
// Traps that raise built-in exceptions (idiv/irem/ldiv/lrem by zero,
// null dereference, array bounds, negative array size, checkcast,
// aastore, monitorenter/exit misuse, athrow null, missing method,
// invokedynamic, StackOverflow) all allocate the exception object via
// Runtime::makeException (JVM messages pinned) and enter the same
// algorithm. The interpreter counts exceptionsThrown (stats).
//
// ===========================================================================
// INTERIM QUICKENED-OPCODE PINS (v0; docs/interp_contract.md)
// ===========================================================================
// The quickener does not exist yet, but T0 already executes quickened
// streams (charter deliverable 1). v0 pins:
//   getfield_quick/putfield_quick: imm = byte offset within the object's
//     field storage; v0 reference layout is Value slots, so
//     offset = slot * sizeof(Value) (16). Runtime::{fieldOffsetOf,
//     slotOfFieldOffset} convert.
//   invokespecial_quick/invokestatic_quick: imm = MethodId (v0 pin:
//     index into the Program's method table).
//   invokevirtual_quick/invokeinterface_quick: imm = IC site id; v0 pin:
//     the site id of a call site is the pc of the call instruction itself
//     (unique within its method; combined with the executing method it
//     identifies the site). The real quickener will allocate global site
//     ids - that change RFCs through this team and bumps the contract.
//
// ===========================================================================
// SAFETY POINTS AND PROFILING (Rules 88, 111, 114, 119)
// ===========================================================================
// safepoint_poll, every invoke*, every allocation, and every backward
// branch poll: v0 = check the (never-set-in-v0) global safepoint-request
// flag + count the poll. Latency is bounded by construction (every poll
// site checks; there are no long instruction sequences without polls -
// loops carry safepoint_poll on backedges by lowering convention).
// Backward branches and method entries bump saturating counters
// (Runtime::profiles). Stats are exposed per run for tier management and
// Rule 133 differential harnesses; nothing in v0 acts on heat yet
// (tiering arrives with T1).
//
// ===========================================================================
// THE DISPATCH PROFILE (ICDG Phase 1, normative v1.2.0)
// ===========================================================================
// Per-call-site dispatch histograms (docs/icdg.md SS7 "T0 is the profile
// source" and SS24 Phase 1: call-site profiling + receiver type profile +
// target stability). T0 produces the DATA; the policy (dispatch-state
// classification, stability scoring, GuardInline) belongs to the ICDG
// consumers (charter: "the team produces the data, not the policy").
//
//   Site key: (caller MethodId, call pc) - the SAME key as the site ICs.
//     invokevirtual/interface: the IC site id (the pc; quickened forms use
//       imm, which equals the pc on well-formed streams, so quick and
//       un-quickened forms of one site share profile state).
//     invokespecial/invokestatic (and their quick forms): the pc (their
//       imm is the target id, not a site id; there is no IC for them).
//   What is counted: every SUCCESSFUL dispatch - IC hit and miss paths
//     alike (the hit path reads the receiver's true class; the IC's y slot
//     is last-miss data and may have drifted). Builtin-executed sites bump
//     the site total only (the target is external: no RBC MethodId).
//     Trapped calls (receiver NPE, missing method, StackOverflow) and
//     <clinit> re-execution pushes never dispatched and are not counted.
//   Receiver histogram: up to kMaxDispatchProfileEntries entries keyed by
//     receiver ClassId, each carrying the resolved target and a saturating
//     count. The (kMax+1)-th distinct receiver class sets the sticky
//     megamorphic flag; entries freeze at that point (the total keeps
//     counting). Static-resolution families (special/static) carry one
//     entry keyed by the target with recvClass = kDispatchNoRecvClass.
//   Storage: [MethodId][site] vectors, lazily sized on first record (Rule
//     7); saturating adds (Rule 114); always-on (tiering data, like
//     Runtime's MethodProfile bumps); accumulates across run()/resume()
//     calls on one Interpreter instance (like siteICs_); deterministic by
//     construction (Rule 124). v0 is single-threaded; the future
//     concurrent form is the sanctioned racy-but-bounded one.
//   Exposure: dispatchProfiles() is read-only observability for the ICDG
//     ingestion (Passes), tier management, and tests; nothing in T0
//     consumes it (Rule 119 observability, no behavior change - Law 36
//     differentials stay byte-identical).

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "b2/interp/Frame.h"
#include "b2/interp/Runtime.h"
#include "b2/interp/Value.h"
#include "b2/rbc/Rbc.h"
#include "b2/rbc/Verifier.h"

namespace b2::interp {

// --- dispatch profile (ICDG Phase 1; docs/icdg.md SS7/SS24) ------------------

// The invoke family a profiled site belongs to (icdg.md CallKind reduced to
// the v0 world: MethodHandle/InvokeDynamic/Native do not dispatch yet).
enum class DispatchSiteKind : std::uint8_t {
  Virtual,    // invokevirtual / invokevirtual_quick
  Interface,  // invokeinterface / invokeinterface_quick
  Special,    // invokespecial / invokespecial_quick (static resolution)
  Static,     // invokestatic / invokestatic_quick (static resolution)
};

// Receiver-histogram capacity per site (Rule 23 named constant). 1 observed
// class = monomorphic, 2 = bimorphic, 3 = polymorphic, the 4th distinct
// class flips the sticky megamorphic flag (the 1/2/3/inf IC shape).
inline constexpr std::size_t kMaxDispatchProfileEntries = 3;

// Sentinel receiver class for static-resolution families (special/static:
// the target does not depend on the receiver) and for count-only records
// (builtin targets - no RBC MethodId exists to name).
inline constexpr std::uint32_t kDispatchNoRecvClass = 0xFFFF'FFFFu;
// Sentinel target id: the site total was counted but no RBC target was
// resolved (builtin execution). Entries never carry it.
inline constexpr std::uint32_t kDispatchNoTarget = 0xFFFF'FFFFu;

// One receiver-class observation at one site (icdg.md SS4.3
// DispatchCandidate's raw-data form: target, receiver class, counts).
struct DispatchEntry {
  std::uint32_t recvClass = kDispatchNoRecvClass; // ClassId.v (sentinel for
                                                  // static-resolution entries)
  std::uint32_t target = kDispatchNoTarget;       // MethodId.v
  std::uint32_t count = 0;                        // saturating observations
};

// The per-site profile. `count` is the site heat; `entries` is the receiver
// histogram (first-seen order, deterministic); `megamorphic` freezes it.
struct DispatchSiteProfile {
  DispatchSiteKind kind = DispatchSiteKind::Virtual;
  bool megamorphic = false; // sticky: > kMaxDispatchProfileEntries classes
  std::uint32_t count = 0;  // saturating successful dispatches at the site
  DispatchEntry entries[kMaxDispatchProfileEntries];
};

struct InterpConfig {
  // Frame depth cap; pushing past this traps StackOverflowError (Java
  // -Xss analogue). Default matches a small default JVM stack.
  std::uint32_t maxFrames = 8192;
  // Emit frame-state dumps at every safepoint poll (the deopt fixture
  // generator; deterministic, Rule 124). Off by default.
  bool traceSafepoints = false;
  // Collect instruction/poll/IC statistics (cheap counters; always-on is
  // fine, but tests that pin stats explicitly may want determinism control).
  bool collectStats = true;
};

// Per-run observable counters (Rule 119: tier behavior must be observable).
struct InterpStats {
  std::uint64_t instructions = 0;   // opcodes retired
  std::uint64_t polls = 0;          // safepoint polls taken
  std::uint64_t calls = 0;          // frames pushed
  std::uint64_t icHits = 0;         // inline-cache hits
  std::uint64_t icMisses = 0;       // inline-cache misses (resolution)
  std::uint64_t exceptions = 0;     // exceptions thrown (incl. rethrows)
  std::uint64_t allocations = 0;    // objects + arrays allocated
};

enum class RunStatus : std::uint8_t {
  Returned,     // entry method returned; result holds the return value
                // (Bottom tag when void)
  Threw,        // uncaught exception escaped the entry frame
  VerifyFailed, // the program failed the verify hard gate; verifyDiags set
};

struct RunResult {
  RunStatus status = RunStatus::Returned;
  Value result{};                          // Returned: the return value
  ObjRef exception{};                      // Threw: the exception object
  std::vector<rbc::VerifyDiag> verifyDiags;// VerifyFailed
  InterpStats stats{};                     // always filled (config permitting)
  std::string safepointTrace;              // dumps when traceSafepoints
};

// The T0 interpreter. One instance per program execution; not thread-safe
// (v0 is single-threaded; the concurrent form is specified in
// docs/interp_contract.md).
class Interpreter {
public:
  explicit Interpreter(const rbc::Program& program,
                       const InterpConfig& config = {});

  // Runs `name`+`descriptor` with `args` (static: parameters in order;
  // instance: receiver first). Verifies the whole program first (hard
  // gate). Never throws, never crashes: every failure is a RunResult.
  [[nodiscard]] RunResult run(std::string_view name,
                              std::string_view descriptor,
                              std::span<const Value> args);

  // --- deopt entry point (Rule 4; docs/deopt_backend.md Part A) -----------
  // Resumes execution at an arbitrary instruction boundary with an
  // externally reconstructed frame (what every tier's deopt stub calls
  // after materializing T0 state). The frame is copied; locals/regs sizes
  // must match the method (callers built them from the contract). Verifies
  // the program first, like run(). pendingException may be set (exception
  // deopt, Part A SS2.3): dispatch starts inside THE EXCEPTION ALGORITHM at
  // `frame.pc`.
  [[nodiscard]] RunResult resume(Frame frame);

  // --- inspection (tests, tooling, future tier management) ----------------
  [[nodiscard]] const Runtime& runtime() const noexcept { return rt_; }
  [[nodiscard]] Runtime& runtime() noexcept { return rt_; }
  [[nodiscard]] const std::vector<Frame>& frames() const noexcept {
    return frames_;
  } // empty outside run(); during trace hooks it is the live stack
  [[nodiscard]] const InterpConfig& config() const noexcept { return cfg_; }

  // --- ICDG Phase 1: the dispatch profile (the header's normative block) ---
  // [MethodId][site]; lazily sized per method on first record; accumulates
  // across run()/resume() on this instance. Read-only observability: the
  // classification policy lives with the ICDG consumers (Passes team).
  [[nodiscard]] const std::vector<std::vector<DispatchSiteProfile>>&
  dispatchProfiles() const noexcept {
    return dispatchProfiles_;
  }

  // Global safepoint request flag (Rule 111 bounded-latency handshake).
  // v0: nothing sets it except tests; a set flag makes every poll site
  // "park" - which in v0 means finishing the current instruction and
  // recording the request (single-threaded world; the multi-threaded
  // protocol lands with the real runtime).
  static bool safepointRequested() noexcept;
  static void requestSafepoint() noexcept;
  static void clearSafepointRequest() noexcept;

private:
  // Per-call-site inline-cache entry (charter deliverable 4: interpreter
  // inline caches). x/y meaning by site family: field sites (getfield/
  // putfield) cache x = slot index, y = class id; call sites (invokevirtual/
  // invokeinterface) cache x = target MethodId, y = observed receiver class
  // id (monomorphic v0; the y slot is where polymorphic-broadcast goes).
  // Lazily sized per method on first miss (miss-path allocation only,
  // Rule 7). Single-threaded v0; the racy-but-bounded update form is
  // specified in docs/interp_contract.md.
  struct SiteIC {
    std::uint32_t x = 0, y = 0;
    bool valid = false;
  };

  const rbc::Program& program_;
  InterpConfig cfg_;
  Runtime rt_;
  std::vector<Frame> frames_;
  std::vector<std::vector<SiteIC>> siteICs_; // [MethodId][pc]
  std::vector<std::vector<DispatchSiteProfile>> dispatchProfiles_; // ICDG Ph1
  RunResult* result_ = nullptr; // active run's result (stats/trace sink)
};

} // namespace b2::interp
