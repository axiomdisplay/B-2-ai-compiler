// B-2 Interpreter - the Tier-0 direct-threaded RBC dispatch core (Task BE-1).
//
// WHY THIS FILE EXISTS:
// T0 is the baseline execution engine, the universal correctness fallback
// (Rule 96), the reference implementation against the Java semantic oracle
// (Rule 67), and the deopt target of every compiled tier (Rule 4/75). This
// file implements the whole of class b2::interp::Interpreter from the frozen
// include/b2/interp/Interp.h: the dispatch loop in BOTH forms
// (interp_contract.md SS4) - the portable token-threaded switch core, and,
// when B2_INTERP_COMPUTED_GOTO is defined (default for GNU/Clang via CMake;
// this TU then compiles as gnu++26 for labels-as-values), the LANDED v1
// computed-goto core with per-handler-tail dispatch over ONE shared set of
// handler bodies - plus THE CALL PROTOCOL, THE EXCEPTION ALGORITHM, inline
// caches, safepoint polls, profiling counters, and the run()/resume() entry
// points. The two forms are semantics-identical by construction; the
// portable test binary runs the identical corpus against the identical
// goldens as the differential proof (Law 36).
//
// KEY INVARIANTS (normative sources: Interp.h block comments, rbc_spec.md):
// - ONE for(;;) loop over an EXPLICIT frame stack. Java calls never recurse
//   in C++ (StackOverflowError must be a Java-visible trap at maxFrames, and
//   deopt needs materializable frames - the frame stack IS the state).
// - The switch is EXHAUSTIVE over Op with no default clause: a missing
//   handler is a -Wswitch compile error, not a runtime surprise. Op::_Count
//   (the pseudo-enumerator) has an explicit unreachable-guard case that
//   raises InternalError - total, never silent, never a crash (Rule 47).
// - Hot-path discipline (Rules 6/7/8/9/16/118): no C++ exceptions, no RTTI,
//   no std::function/shared_ptr, no virtual calls, no std::string WORK
//   except on the sanctioned cold paths. Sanctioned allocation sites, each
//   documented at its definition: (1) frame pushes (invoke*/<clinit>),
//   (2) trap paths (exception objects + JVM message strings + dotted names),
//   (3) inline-cache misses (lazily sized siteICs_ vectors, resolveField/
//   resolveMethod/classId string interning), (4) the lazy heap growth inside
//   Heap (first touch of a late-resolved field), (5) resolution-bearing
//   opcodes that the frozen Runtime API exposes only by name (checkcast/
//   instanceof/new/ldc/anewarray/getstatic/aastore call classId() per
//   execution; reported as a v0 seam, see the notes at each site).
// - The verifier's guarantees (register/slot/cp bounds, types, termination)
//   are NOT re-checked on the fast path; defense-in-depth probes convert
//   impossible states into java/lang/InternalError results.
// - Numeric semantics are Java's EXACTLY (Rule 72): all 32-bit int math is
//   computed in int64_t and narrowed (int32_t overflow is UB in C++);
//   64-bit math is computed via uint64_t; shifts mask the count; frem/drem
//   are std::fmod (JLS 15.17.3: Java's % is the C fmod, NOT the IEEE
//   remainder - rbc_spec.md SS3.7/SS3.8 say "IEEE remainder" which is a spec
//   typo, reported); float->int conversions clamp by comparison in the
//   value's own domain BEFORE the cast (a C++ cast of an out-of-range float
//   is UB; Java pins NaN->0 and saturation, JLS 5.1.3).
//
// AMBIGUITY RESOLUTIONS (binding decisions, each cross-referenced at its
// code site; full stories in the BE-1 report):
// - A1 Switch payloads follow the CANONICAL layouts the landed toolchain
//   implements and the verifier enforces (Verifier.cpp "switch-table
//   canonical payload layouts": tableswitch [low, high, default, targets...],
//   lookupswitch [N, default, match/target pairs...]) - NOT the flat-pairs
//   reading of rbc_spec.md SS3.11/P6. The verifier is the hard gate; a
//   flat-pairs payload cannot pass it, so the pin's letter is unexecutable.
// - A2 NPE on call receivers precedes the inline cache (Java traps before
//   dispatch; an IC hit must not bypass the null check).
// - A3 <clinit> frames are recognized at return time by name+descriptor
//   "()V" and do NOT advance the caller's pc (the triggering instruction
//   re-executes, JVMS 5.5); every other callee advances to callerPc + 1.
// - A4 Quickened virtual/interface calls resolve a COLD inline cache through
//   cp[imm] (imm doubling as the cp index); a cold cache whose imm is not a
//   MethodRef/InterfaceMethodRef raises InternalError (honest refusal).
// - A5 Unwritten statics/instance fields read as the FIELD TYPE's zero
//   (JLS 4.12.5), coercing Runtime/Heap's Bottom "unset" marker so every
//   slot keeps its verified type (Value.h deopt-state contract).
// - A6 stats.polls counts explicit safepoint_poll retirements; the other
//   Rule 88 poll sites (invoke/alloc/backedge) reduce in v0 to the profile
//   bumps they already perform - the flag's only v0 reaction is "continue".
//
// CROSS-REFERENCES:
// - include/b2/interp/Interp.h (THE contract: dispatch loop, call protocol,
//   exception algorithm, quickened pins, safepoints/profiling)
// - docs/rbc_spec.md SS2 (encoding), SS3.1-SS3.19 (opcode semantics),
//   SS5.3 (handler model), SS6 (quickening)
// - docs/laws.md Rules 6, 7, 8, 9, 16, 67, 72, 74, 75, 88, 96, 111, 114
// - docs/cpp26_standards.md Part A (CS-1..CS-13)

#include "b2/interp/Interp.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace b2::interp {
namespace {

// ============================================================================
// Pinned names and messages (JVM-exact; no magic strings, Law 23 discipline).
// ============================================================================

constexpr std::string_view kClsArithmetic = "java/lang/ArithmeticException";
constexpr std::string_view kClsNullPointer = "java/lang/NullPointerException";
constexpr std::string_view kClsArrayIndexOob =
    "java/lang/ArrayIndexOutOfBoundsException";
constexpr std::string_view kClsNegativeArraySize =
    "java/lang/NegativeArraySizeException";
constexpr std::string_view kClsClassCast = "java/lang/ClassCastException";
constexpr std::string_view kClsArrayStore = "java/lang/ArrayStoreException";
constexpr std::string_view kClsIllegalMonitor =
    "java/lang/IllegalMonitorStateException";
constexpr std::string_view kClsStackOverflow = "java/lang/StackOverflowError";
constexpr std::string_view kClsNoSuchMethod = "java/lang/NoSuchMethodError";
constexpr std::string_view kClsBootstrapMethod =
    "java/lang/BootstrapMethodError";
constexpr std::string_view kClsInternalError = "java/lang/InternalError";

constexpr std::string_view kMsgDivByZero = "/ by zero";
constexpr std::string_view kMsgIndyUnsupported =
    "invokedynamic not supported in v0";
constexpr std::string_view kMsgDeoptTrap =
    "deopt_trap executed in T0 without deopt metadata (v0)";
constexpr std::string_view kMsgLdcUnsupported =
    "ldc of methodtype/methodhandle not supported in v0";
constexpr std::string_view kMsgUnresolvedField = "unresolved field";

// The class-initializer identity (JVMS 2.9: <clinit> is ()V, called only by
// the VM). Used to give <clinit> frames their re-execute-the-trigger return
// semantics (ambiguity resolution A3 at the return handler).
constexpr std::string_view kClinitName = "<clinit>";
constexpr std::string_view kClinitDesc = "()V";

// Java shift-count masks (JLS 15.19): only the low 5/6 bits of the count
// register participate.
constexpr std::uint32_t kIntShiftMask = 0x1Fu;
constexpr std::uint32_t kLongShiftMask = 0x3Fu;

// JVMS 4.4.1 bounds array descriptors at 255 dimensions; a multianewarray
// with more dimension registers than that is hostile input (the count also
// bounds the stack buffer below, keeping the dispatch path allocation-free).
constexpr std::uint16_t kMaxMultiDims = 255;

// ============================================================================
// Rule 111: the global safepoint-request flag.
//
// WHY a function-local std::atomic<std::uint8_t> (and no other global state,
// Law 125): (a) a function-local static has no static-initialization-order
// dependence - the flag is usable from the first poll in any TU's order;
// (b) std::atomic<std::uint8_t> is lock-free on every supported ABI, so the
// poll load cannot block (Rule 118: no locks on hot paths); (c) the ordering
// is RELAXED on purpose: v0 is single-threaded and the flag carries no data
// publication with it (nothing is torn by observing it late - the only v0
// reaction is "finish the instruction and continue"). When the real runtime
// lands, the store becomes release + a handshake state machine; the relaxed
// load already gives bounded latency because every safepoint_poll retires it
// (Rule 111: loops carry safepoint_poll on backedges by lowering convention).
// ============================================================================

std::atomic<std::uint8_t>& safepointFlag() noexcept {
  static std::atomic<std::uint8_t> flag{0};
  return flag;
}

// ============================================================================
// Numeric kernels (Rule 72: Java numeric semantics preserved exactly).
// ============================================================================

// 32-bit two's-complement wraparound: compute in int64_t, narrow once.
// C++ signed overflow is UB; Java wraps. All three ops are exact in 64-bit.
[[nodiscard]] constexpr std::int32_t wrapAdd32(std::int32_t a,
                                               std::int32_t b) noexcept {
  return static_cast<std::int32_t>(static_cast<std::int64_t>(a) +
                                   static_cast<std::int64_t>(b));
}
[[nodiscard]] constexpr std::int32_t wrapSub32(std::int32_t a,
                                               std::int32_t b) noexcept {
  return static_cast<std::int32_t>(static_cast<std::int64_t>(a) -
                                   static_cast<std::int64_t>(b));
}
[[nodiscard]] constexpr std::int32_t wrapMul32(std::int32_t a,
                                               std::int32_t b) noexcept {
  return static_cast<std::int32_t>(static_cast<std::int64_t>(a) *
                                   static_cast<std::int64_t>(b));
}

// 64-bit wraparound: two int64 operands CAN overflow int64, so the arithmetic
// happens in the unsigned domain (well-defined modular arithmetic) and the
// result is bit-cast back.
[[nodiscard]] constexpr std::int64_t wrapAdd64(std::int64_t a,
                                               std::int64_t b) noexcept {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) +
                                   static_cast<std::uint64_t>(b));
}
[[nodiscard]] constexpr std::int64_t wrapSub64(std::int64_t a,
                                               std::int64_t b) noexcept {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) -
                                   static_cast<std::uint64_t>(b));
}
[[nodiscard]] constexpr std::int64_t wrapMul64(std::int64_t a,
                                               std::int64_t b) noexcept {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) *
                                   static_cast<std::uint64_t>(b));
}

// Negation without UB at the MIN boundary: 0 - MIN wraps in the unsigned
// domain and lands back on MIN, exactly Java's -MIN == MIN (JLS 15.15.4).
[[nodiscard]] constexpr std::int32_t neg32(std::int32_t a) noexcept {
  return static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(a));
}
[[nodiscard]] constexpr std::int64_t neg64(std::int64_t a) noexcept {
  return static_cast<std::int64_t>(0ull - static_cast<std::uint64_t>(a));
}

// Float/double -> int32 with Java's JLS 5.1.3 semantics: NaN -> 0, saturation
// at both ends, round toward zero. WHY compare-before-cast: a C++ cast of a
// value outside the target's range is UB; the comparisons below happen in the
// VALUE'S OWN domain (float or double), where 2^31 is exactly representable,
// so the cast only ever sees an in-range value.
template <typename Float>
[[nodiscard]] std::int32_t toInt32Sat(Float v) noexcept {
  if (std::isnan(v)) {
    return 0;
  }
  constexpr Float kUpper = Float(2147483648.0); // 2^31, exact in binary FP
  constexpr Float kLower = Float(-2147483648.0);
  if (v >= kUpper) {
    return std::numeric_limits<std::int32_t>::max();
  }
  if (v < kLower) {
    return std::numeric_limits<std::int32_t>::min();
  }
  return static_cast<std::int32_t>(v);
}

// Float/double -> int64, same discipline with the 2^63 bounds (JLS 5.1.3).
template <typename Float>
[[nodiscard]] std::int64_t toInt64Sat(Float v) noexcept {
  if (std::isnan(v)) {
    return 0;
  }
  constexpr Float kUpper = Float(9223372036854775808.0); // 2^63, exact
  constexpr Float kLower = Float(-9223372036854775808.0);
  if (v >= kUpper) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (v < kLower) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return static_cast<std::int64_t>(v);
}

// ============================================================================
// Value-model helpers.
// ============================================================================

// The zero value of a field type (JLS 4.12.5). Ambiguity resolution A5:
// Runtime/Heap report "never written" statics and instance slots as Bottom;
// the Value.h contract ("slots hold exactly the type the verifier proved")
// forbids leaking Bottom into a typed register, so the dispatch core coerces
// to the declared field type's zero on read.
[[nodiscard]] Value zeroOfField(rbc::RType t) noexcept {
  switch (t) {
  case rbc::RType::Int:
    return Value::intVal(0);
  case rbc::RType::Long:
    return Value::longVal(0);
  case rbc::RType::Float:
    return Value::floatVal(0.0F);
  case rbc::RType::Double:
    return Value::doubleVal(0.0);
  case rbc::RType::Null:
  case rbc::RType::Ref:
    return Value::nullVal();
  case rbc::RType::Bottom:
    break; // malformed descriptor: the caller traps (InternalError)
  }
  return Value::bottom();
}

// Result of the shared class-initialization probe (JVMS 5.5 first use).
enum class InitFlow : std::uint8_t {
  Ready,         // no <clinit> needed; execute the instruction now
  PushedClinit,  // <clinit> frame pushed; re-execute the trigger on return
  UnwoundEmpty,  // a StackOverflowError escaped the whole stack (Threw)
};

// Narrowing mode of the array store families (rbc_spec.md SS3.13: the
// narrowing is STORE-side; baload/caload/saload are plain loads - v0 pin).
enum class StoreNarrow : std::uint8_t { None, Byte, Char, Short };

// ============================================================================
// The T0 dispatch core (THE DISPATCH LOOP ALGORITHM, Interp.h).
//
// WHY a free function template: Interp.h freezes the Interpreter class (no
// private helpers can be added), so the core lives at file scope and receives
// every piece of state explicitly. The inline-cache table's element type is
// PRIVATE to Interpreter, so `SiteTable` is a deduced template parameter -
// naming it here would be ill-formed, while using its public members through
// a deduced reference is legal C++. Both run() and resume() pass the same
// type, so exactly one instantiation exists (internal linkage, no ODR cost).
//
// Returns the terminal RunStatus and fills out.result / out.exception /
// out.stats; frames_ is empty on every exit path (Returned pops the entry
// frame; Threw unwinds it in THE EXCEPTION ALGORITHM).
// ============================================================================
template <typename SiteTable, typename DispatchTable>
[[nodiscard]] RunStatus dispatchLoop(const rbc::Program& program,
                                     const InterpConfig& cfg, Runtime& rt,
                                     std::vector<Frame>& frames,
                                     SiteTable& siteICs,
                                     DispatchTable& dispatchProfiles,
                                     RunResult& out) {

  // The program class, interned once: the only class that can own RBC
  // methods in the v0 one-class world, hence the only class whose <clinit>
  // can ever be pushed (Runtime::needsInit gates on it internally). Used by
  // the invokestatic/invokestatic_quick first-use triggers.
  const ClassId programClass = rt.classId(program.className);

  // Index of a frame's method in the program table (MethodId space). All
  // frame method pointers are validated to point into program.methods (run/
  // resume validate externally-supplied frames; the loop only ever stores
  // pointers obtained from rt.method()/program.find() on verified ids).
  const auto methodIndex = [&program](const Frame& f) noexcept {
    return static_cast<std::uint32_t>(f.method - program.methods.data());
  };

  // Best-effort LIFO monitor release on unwind/return (Interp.h call
  // protocol step 4; JVMS monitorexit-on-throw). The frame's monitor vector
  // is most-recent-first (Frame.h pin), so front-to-back IS LIFO. Failures
  // are swallowed: the original exception wins (Interp.h).
  const auto releaseMonitorsLIFO = [&rt](Frame& f) {
    for (const ObjRef m : f.monitors) {
      (void)rt.heap().monitorexit(m);
    }
    f.monitors.clear();
  };

  // --- THE EXCEPTION ALGORITHM (Interp.h; Rule 74) ---------------------------
  // Returns true when a handler caught the exception (frames.back() is
  // repositioned at the handler entry with r0 holding the exception; the
  // loop re-fetches), false when the stack emptied (out.exception set).
  //
  // WHY pendingException is never SET here: the algorithm runs synchronously
  // from trap to handler, so no safepoint (the only observer, Frame.h) can
  // ever see an exception in flight; the field's producer is resume() with
  // an externally reconstructed exception-deopt frame (Interp.h).
  const auto throwException = [&](ObjRef exc) -> bool {
    if (cfg.collectStats) {
      ++out.stats.exceptions;
    }
    while (!frames.empty()) {
      Frame& f = frames.back();
      const rbc::Method& m = *f.method;
      // Handlers in TABLE ORDER; coverage is [start, end) at the CURRENT pc
      // (for a caller that is the invoke's pc - the call site is the throwing
      // location, JVMS semantics - because the caller's pc is never advanced
      // until the callee returns normally).
      for (const rbc::ExceptionHandler& h : m.handlers) {
        if (f.pc < h.start || f.pc >= h.end) {
          continue;
        }
        if (h.catchType >= 0) {
          // Catch-type classes are interned here (cold path; the only name
          // work in the algorithm). isAssignableFrom covers the builtin
          // exception hierarchy, reflexive exact matches, and arrays.
          const rbc::Const& ct = m.cp[static_cast<std::size_t>(h.catchType)];
          if (!rt.isAssignableFrom(rt.classId(ct.str),
                                   rt.heap().classOf(exc))) {
            continue;
          }
        }
        // CAUGHT (rbc_spec.md SS5.3): registers die, r0 carries the object.
        f.pc = h.handler;
        f.regs.assign(m.numRegs, Value::bottom());
        if (!f.regs.empty()) {
          f.regs[0] = Value::refVal(exc);
        }
        f.pendingException = ObjRef{};
        return true;
      }
      releaseMonitorsLIFO(f);
      frames.pop_back();
    }
    out.exception = exc;
    return false;
  };

  // Trap constructor: build the exception object (Rule 74: exceptions are
  // values; makeException is the sanctioned allocating trap path) and run
  // the algorithm. `msg` may be a temporary std::string - it is consumed
  // (copied into the interned detailMessage) within this full expression.
  const auto raise = [&](std::string_view cls, std::string_view msg) -> bool {
    return throwException(rt.makeException(cls, msg));
  };
  const auto raiseNpe = [&]() -> bool {
    return raise(kClsNullPointer, ""); // v0 pins the classic empty message
  };
  const auto raiseAioobe = [&](std::int32_t idx,
                               std::uint32_t len) -> bool {
    return raise(kClsArrayIndexOob, "Index " + std::to_string(idx) +
                                        " out of bounds for length " +
                                        std::to_string(len));
  };

  // --- class initialization (JVMS 5.5; CALL PROTOCOL step 2) -----------------
  // Marked BEFORE running so recursive first-use terminates; the <clinit>
  // frame's callerPc is the TRIGGER's pc and the return handler does NOT
  // advance it (A3), so the trigger re-executes with the class initialized.
  // needsInit/findClinit gate on the program class internally (BE-2 pin);
  // callers pass whatever class the instruction names and trust the pair.
  const auto pushClinit = [&](ClassId cls) -> bool {
    rt.markInitialized(cls);
    const MethodId clinit = rt.findClinit(cls); // valid: needsInit held
    if (frames.size() >= cfg.maxFrames) {
      return raise(kClsStackOverflow, ""); // empty message (JVM pin)
    }
    Frame nf;
    nf.method = &rt.method(clinit);
    nf.callerPc = frames.back().pc;
    // <clinit>()V is static and parameterless: no locals, regs Bottom.
    nf.regs.assign(nf.method->numRegs, Value::bottom());
    frames.push_back(std::move(nf)); // sanctioned allocation (frame push)
    if (cfg.collectStats) {
      ++out.stats.calls; // a <clinit> execution IS a frame push
    }
    rt.bumpInvocations(clinit);
    return true;
  };

  const auto initClassIfNeeded = [&](ClassId cls) -> InitFlow {
    if (!rt.needsInit(cls)) {
      return InitFlow::Ready;
    }
    return pushClinit(cls) ? InitFlow::PushedClinit : InitFlow::UnwoundEmpty;
  };

  // --- frame push (CALL PROTOCOL step 3) -------------------------------------
  // Shared tail of every invoke*. Returns true when the callee frame was
  // pushed (or a trap was caught somewhere - dispatch continues either way),
  // false only when an exception escaped the whole stack.
  const auto pushCall = [&](const rbc::Ins& call, MethodId target) -> bool {
    const rbc::Method& m = rt.method(target);
    // No body to execute. The JVM raises IncompatibleClassChangeError; the
    // v0 contract pins NoSuchMethodError with the standard message shape
    // (documented divergence - the class model that distinguishes them does
    // not exist yet). Builtin targets never reach here (probed earlier).
    if (m.isAbstract() ||
        (m.flags & rbc::method_flags::Native) != 0) {
      return raise(kClsNoSuchMethod,
                   program.className + "." + m.name + m.descriptor);
    }
    // Checked BEFORE the push, AT the call site (Interp.h): StackOverflowError
    // must be a Java-visible trap, never a C++ container failure.
    if (frames.size() >= cfg.maxFrames) {
      return raise(kClsStackOverflow, ""); // empty message (JVM pin)
    }
    Frame nf;
    nf.method = &m;
    nf.callerPc = frames.back().pc; // caller pc stays AT the invoke
    {
      const Frame& caller = frames.back();
      // Argument span, defensively clamped (verified streams guarantee
      // a + b <= numRegs; hostile resume frames might not).
      const std::size_t base =
          std::min<std::size_t>(call.a, caller.regs.size());
      const std::size_t count =
          std::min<std::size_t>(call.b, caller.regs.size() - base);
      nf.locals.assign(caller.regs.begin() + static_cast<std::ptrdiff_t>(base),
                       caller.regs.begin() +
                           static_cast<std::ptrdiff_t>(base + count));
    }
    if (nf.locals.size() < m.numLocals) {
      nf.locals.resize(m.numLocals, Value::bottom());
    }
    nf.regs.assign(m.numRegs, Value::bottom());
    frames.push_back(std::move(nf)); // sanctioned allocation (frame push)
    if (cfg.collectStats) {
      ++out.stats.calls;
    }
    rt.bumpInvocations(target);
    return true;
  };

  // --- inline-cache storage (charter deliverable 4) ---------------------------
  // siteICs_ is [MethodId][site]; lazily sized on first miss only (Rule 7).
  // Monomorphic v0: field sites cache x=slot,y=declaring class; call sites
  // cache x=target MethodId,y=observed receiver class. The y slot is where
  // polymorphic-broadcast goes; the HIT path deliberately does not consult y
  // (v0 resolution is site-static - documented monomorphic pin).
  const auto fillSiteIC = [&](std::uint32_t mIdx, std::uint32_t site,
                              std::uint32_t x, std::uint32_t y) {
    if (siteICs.size() <= mIdx) {
      siteICs.resize(mIdx + 1);
    }
    auto& perMethod = siteICs[mIdx];
    if (perMethod.size() <= site) {
      perMethod.resize(site + 1);
    }
    auto& entry = perMethod[site];
    entry.x = x;
    entry.y = y;
    entry.valid = true;
  };

  // --- ICDG Phase 1: the per-site dispatch profile (Interp.h block) --------
  // Bumped on EVERY successful dispatch (hit and miss paths; the hit path
  // reads the receiver's TRUE class - the IC y slot is last-miss data and
  // may have drifted). Trapped calls (NPE/SOE/missing method) never
  // dispatched and are not counted. Saturating (Rule 114), lazily sized
  // (Rule 7), always-on tiering data. The sentinel recvClass marks the
  // static-resolution families (one entry keyed by the target) and the
  // sentinel target marks count-only records (builtin sites).
  const auto recordDispatch = [&](std::uint32_t mIdx, std::uint32_t site,
                                  DispatchSiteKind kind,
                                  std::uint32_t recvClass,
                                  std::uint32_t target) {
    constexpr std::uint32_t kSat =
        std::numeric_limits<std::uint32_t>::max();
    if (dispatchProfiles.size() <= mIdx) {
      dispatchProfiles.resize(mIdx + 1);
    }
    auto& perMethod = dispatchProfiles[mIdx];
    if (perMethod.size() <= site) {
      perMethod.resize(site + 1);
    }
    DispatchSiteProfile& p = perMethod[site];
    p.kind = kind;
    if (p.count != kSat) {
      ++p.count; // saturating (Rule 114)
    }
    if (recvClass == kDispatchNoRecvClass) {
      if (target == kDispatchNoTarget) {
        return; // count-only: builtin-executed site (external target)
      }
      // Static-resolution family: one entry, keyed by the static target.
      p.entries[0].recvClass = kDispatchNoRecvClass;
      p.entries[0].target = target;
      if (p.entries[0].count != kSat) {
        ++p.entries[0].count; // saturating (Rule 114)
      }
      return;
    }
    if (!p.megamorphic) {
      for (DispatchEntry& e : p.entries) {
        if (e.count != 0 && e.recvClass == recvClass) {
          e.target = target; // refresh (v0 resolution is stable; honest data)
          if (e.count != kSat) {
            ++e.count; // saturating (Rule 114)
          }
          return;
        }
      }
      for (DispatchEntry& e : p.entries) {
        if (e.count == 0) { // first observation of this receiver class
          e.recvClass = recvClass;
          e.target = target;
          e.count = 1;
          return;
        }
      }
      // (kMaxDispatchProfileEntries + 1)-th distinct class: sticky flag,
      // entries frozen (the total above keeps counting).
      p.megamorphic = true;
    }
  };

  // --- array element families (rbc_spec.md SS3.13) ----------------------------
  // Loads: NPE (null array), AIOOBE (bounds), raw element read. baload/
  // caload/saload are PLAIN loads - the narrowing is store-side (v0 pin:
  // well-typed stores keep sub-int arrays in range, so a raw load is already
  // correctly extended).
  const auto arrayLoad = [&](Frame& f, const rbc::Ins& i) -> bool {
    const Value& arr = f.regs[i.a];
    if (arr.isNull()) {
      return raiseNpe();
    }
    const std::int32_t idx = f.regs[i.b].as.i;
    const std::uint32_t len = rt.heap().arrayLength(arr.ref());
    if (idx < 0 || static_cast<std::uint32_t>(idx) >= len) {
      return raiseAioobe(idx, len);
    }
    const Value v = rt.heap().loadElem(arr.ref(),
                                       static_cast<std::uint32_t>(idx));
    if (v.type == rbc::RType::Bottom) {
      // Defensive: kind misuse on a non-array object (verified streams
      // never get here; the heap refuses rather than guessing a slot).
      return raise(kClsInternalError, "array element access on non-array");
    }
    f.regs[i.dst] = v;
    ++f.pc;
    return true;
  };

  // Stores: NPE, AIOOBE, sub-int narrowing by family, raw store.
  const auto arrayStore = [&](Frame& f, const rbc::Ins& i,
                              StoreNarrow narrow) -> bool {
    const Value& arr = f.regs[i.a];
    if (arr.isNull()) {
      return raiseNpe();
    }
    const std::int32_t idx = f.regs[i.b].as.i;
    const std::uint32_t len = rt.heap().arrayLength(arr.ref());
    if (idx < 0 || static_cast<std::uint32_t>(idx) >= len) {
      return raiseAioobe(idx, len);
    }
    Value v = f.regs[i.dst]; // dst is READ (spec pin P3)
    switch (narrow) {
    case StoreNarrow::None:
      break;
    case StoreNarrow::Byte:
      v = Value::intVal(
          static_cast<std::int32_t>(static_cast<std::int8_t>(v.as.i)));
      break;
    case StoreNarrow::Char:
      v = Value::intVal(
          static_cast<std::int32_t>(static_cast<std::uint16_t>(v.as.i)));
      break;
    case StoreNarrow::Short:
      v = Value::intVal(
          static_cast<std::int32_t>(static_cast<std::int16_t>(v.as.i)));
      break;
    }
    if (!rt.heap().storeElem(arr.ref(), static_cast<std::uint32_t>(idx), v)) {
      return raise(kClsInternalError, "array element access on non-array");
    }
    ++f.pc;
    return true;
  };

  // --- resume() entry: exception deopt (Interp.h) ------------------------------
  // Dispatch may start INSIDE the exception algorithm: an externally
  // reconstructed frame may carry a pending exception (Part A SS2.3). run()
  // frames never set it, so this is a no-op for them.
  if (!frames.empty() && frames.back().pendingException.valid()) {
    const ObjRef exc = frames.back().pendingException;
    frames.back().pendingException = ObjRef{};
    if (!throwException(exc)) {
      return RunStatus::Threw;
    }
  }

  // ===========================================================================

  // ===========================================================================
  // FETCH / DISPATCH (interp_contract.md SS4; the v1 computed-goto milestone,
  // MSG-20260830-005): TWO dispatch forms over ONE set of handler bodies.
  //
  // The portable form is the v0 token-threaded core: fetch op -> exhaustive
  // switch (no default clause: a missing handler is a -Wswitch compile error
  // - the exhaustiveness proof) -> handler -> B2_NEXT (break) -> next fetch.
  //
  // The computed-goto form (B2_INTERP_COMPUTED_GOTO, default ON for GNU/Clang
  // via CMake; this TU then compiles as gnu++26 for labels-as-values) dispatch
  // through kB2DispatchTable AND replicates the indirect branch AT EVERY
  // handler tail (B2_NEXT -> B2_DISPATCH), so the branch predictor learns
  // per-opcode transitions (the classic threaded-dispatch win) instead of
  // sharing one jump site with the whole opcode mix. Semantics are identical
  // BY CONSTRUCTION: same bodies, same fetch/probe/stats order, same
  // redispatch-on-catch; the differential proof is the portable test binary
  // running the identical corpus against the identical goldens (Law 36).
  //
  // `fr` is a token macro over frp (NOT a reference) in both forms: frp is
  // re-acquired at EVERY fetch because frames_ mutates on calls, returns,
  // and unwinds. The v0 reference form was safe only because every frames_
  // mutation site breaks or returns immediately after (the return handler
  // re-binds its own callee/caller locals); the macro form keeps that
  // discipline load-bearing and removes the dangling-reference window.
  //
  // Shared hot-path law (Rules 6/7/8/9/16/118): no allocation, no C++
  // exceptions, no RTTI, no locks in dispatch or handlers.
  // ===========================================================================
#define B2_FETCH_AND_PROBE()                                                    \
  do {                                                                          \
    frp = &frames.back();                                                       \
    if (frp->pc >= frp->method->code.size()) {                                  \
      if (!raise(kClsInternalError, "pc out of range (corrupt frame)")) {       \
        return RunStatus::Threw;                                                \
      }                                                                         \
      B2_REDISPATCH(); /* caught: pc is now a verified handler entry */         \
    }                                                                           \
    ins = frp->method->code[frp->pc]; /* 12-byte copy: survives frames_ churn */\
    if (cfg.collectStats) {                                                     \
      ++out.stats.instructions; /* counted at FETCH (contract) */                \
    }                                                                           \
  } while (false)

#if B2_INTERP_COMPUTED_GOTO

  // Labels-as-values (GNU extension; this TU compiles as gnu++26 with
  // -Wno-pedantic for exactly this reason). Designated initializers make a
  // duplicate opcode a compile error; the null scan below makes a MISSING
  // entry a loud refusal - together with the portable build's -Wswitch
  // exhaustiveness, the table cannot silently drift from the Op enum.
#define B2_CONCAT2(a, b) a##b
#define B2_CONCAT(a, b) B2_CONCAT2(a, b)
#define B2_LABEL(op) B2_CONCAT(B2_L_, op)
#define B2_TARGET(op) B2_LABEL(op):
#define B2_NEXT B2_DISPATCH()
#define B2_REDISPATCH() goto b2_redispatch
#define B2_DISPATCH()                                                           \
  do {                                                                          \
    B2_FETCH_AND_PROBE();                                                       \
    const std::uint16_t b2Op = ins.op;                                          \
    if (b2Op >= b2::rbc::opCount()) {                                           \
      /* The Op::_Count guard of the portable form: corrupt stream state is */  \
      /* a Java-visible error, never an OOB table read (Rule 47). */            \
      if (!raise(kClsInternalError, "unreachable opcode dispatched")) {         \
        return RunStatus::Threw;                                                \
      }                                                                         \
      B2_REDISPATCH();                                                          \
    }                                                                           \
    goto *kB2DispatchTable[b2Op];                                               \
  } while (false)

  static const void* const kB2DispatchTable[b2::rbc::opCount()] = {
      [0] = &&B2_L_Nop, [1] = &&B2_L_SafepointPoll, [2] = &&B2_L_AconstNull,
      [3] = &&B2_L_Iconst, [4] = &&B2_L_Fconst, [5] = &&B2_L_Lconst,
      [6] = &&B2_L_Dconst, [7] = &&B2_L_Ldc, [8] = &&B2_L_Iload,
      [9] = &&B2_L_Lload, [10] = &&B2_L_Fload, [11] = &&B2_L_Dload,
      [12] = &&B2_L_Aload, [13] = &&B2_L_Istore, [14] = &&B2_L_Lstore,
      [15] = &&B2_L_Fstore, [16] = &&B2_L_Dstore, [17] = &&B2_L_Astore,
      [18] = &&B2_L_Imove, [19] = &&B2_L_Lmove, [20] = &&B2_L_Fmove,
      [21] = &&B2_L_Dmove, [22] = &&B2_L_Amove, [23] = &&B2_L_Iadd,
      [24] = &&B2_L_Isub, [25] = &&B2_L_Imul, [26] = &&B2_L_Idiv,
      [27] = &&B2_L_Irem, [28] = &&B2_L_Ineg, [29] = &&B2_L_Ishl,
      [30] = &&B2_L_Ishr, [31] = &&B2_L_Iushr, [32] = &&B2_L_Iand,
      [33] = &&B2_L_Ior, [34] = &&B2_L_Ixor, [35] = &&B2_L_Iinc,
      [36] = &&B2_L_Ladd, [37] = &&B2_L_Lsub, [38] = &&B2_L_Lmul,
      [39] = &&B2_L_Ldiv, [40] = &&B2_L_Lrem, [41] = &&B2_L_Lneg,
      [42] = &&B2_L_Lshl, [43] = &&B2_L_Lshr, [44] = &&B2_L_Lushr,
      [45] = &&B2_L_Land, [46] = &&B2_L_Lor, [47] = &&B2_L_Lxor,
      [48] = &&B2_L_Fadd, [49] = &&B2_L_Fsub, [50] = &&B2_L_Fmul,
      [51] = &&B2_L_Fdiv, [52] = &&B2_L_Frem, [53] = &&B2_L_Fneg,
      [54] = &&B2_L_Dadd, [55] = &&B2_L_Dsub, [56] = &&B2_L_Dmul,
      [57] = &&B2_L_Ddiv, [58] = &&B2_L_Drem, [59] = &&B2_L_Dneg,
      [60] = &&B2_L_Icmp, [61] = &&B2_L_Lcmp, [62] = &&B2_L_Fcmpl,
      [63] = &&B2_L_Fcmpg, [64] = &&B2_L_Dcmpl, [65] = &&B2_L_Dcmpg,
      [66] = &&B2_L_I2l, [67] = &&B2_L_I2f, [68] = &&B2_L_I2d,
      [69] = &&B2_L_L2i, [70] = &&B2_L_L2f, [71] = &&B2_L_L2d,
      [72] = &&B2_L_F2i, [73] = &&B2_L_F2l, [74] = &&B2_L_F2d,
      [75] = &&B2_L_D2i, [76] = &&B2_L_D2l, [77] = &&B2_L_D2f,
      [78] = &&B2_L_I2b, [79] = &&B2_L_I2c, [80] = &&B2_L_I2s,
      [81] = &&B2_L_Goto, [82] = &&B2_L_Ifeq, [83] = &&B2_L_Ifne,
      [84] = &&B2_L_Iflt, [85] = &&B2_L_Ifge, [86] = &&B2_L_Ifgt,
      [87] = &&B2_L_Ifle, [88] = &&B2_L_Ifnull, [89] = &&B2_L_Ifnonnull,
      [90] = &&B2_L_IfIcmpeq, [91] = &&B2_L_IfIcmpne, [92] = &&B2_L_IfIcmplt,
      [93] = &&B2_L_IfIcmpge, [94] = &&B2_L_IfIcmpgt, [95] = &&B2_L_IfIcmple,
      [96] = &&B2_L_IfAcmpeq, [97] = &&B2_L_IfAcmpne, [98] = &&B2_L_Tableswitch,
      [99] = &&B2_L_Lookupswitch, [100] = &&B2_L_Getfield, [101] = &&B2_L_Putfield,
      [102] = &&B2_L_Getstatic, [103] = &&B2_L_Putstatic, [104] = &&B2_L_GetfieldQuick,
      [105] = &&B2_L_PutfieldQuick, [106] = &&B2_L_NewArray, [107] = &&B2_L_AnewArray,
      [108] = &&B2_L_Arraylength, [109] = &&B2_L_Iaload, [110] = &&B2_L_Laload,
      [111] = &&B2_L_Faload, [112] = &&B2_L_Daload, [113] = &&B2_L_Aaload,
      [114] = &&B2_L_Baload, [115] = &&B2_L_Caload, [116] = &&B2_L_Saload,
      [117] = &&B2_L_Iastore, [118] = &&B2_L_Lastore, [119] = &&B2_L_Fastore,
      [120] = &&B2_L_Dastore, [121] = &&B2_L_Aastore, [122] = &&B2_L_Bastore,
      [123] = &&B2_L_Castore, [124] = &&B2_L_Sastore, [125] = &&B2_L_Multianewarray,
      [126] = &&B2_L_New, [127] = &&B2_L_Checkcast, [128] = &&B2_L_Instanceof,
      [129] = &&B2_L_Monitorenter, [130] = &&B2_L_Monitorexit, [131] = &&B2_L_Invokevirtual,
      [132] = &&B2_L_Invokespecial, [133] = &&B2_L_Invokestatic, [134] = &&B2_L_Invokeinterface,
      [135] = &&B2_L_Invokedynamic, [136] = &&B2_L_InvokevirtualQuick, [137] = &&B2_L_InvokespecialQuick,
      [138] = &&B2_L_InvokestaticQuick, [139] = &&B2_L_InvokeinterfaceQuick, [140] = &&B2_L_Return,
      [141] = &&B2_L_Ireturn, [142] = &&B2_L_Lreturn, [143] = &&B2_L_Freturn,
      [144] = &&B2_L_Dreturn, [145] = &&B2_L_Areturn, [146] = &&B2_L_Athrow,
      [147] = &&B2_L_DeoptTrap, [148] = &&B2_L_GuardNonNull, [149] = &&B2_L_GuardClass,
  };
  // Build-bug totality (Rule 47): a null entry means the table list and the
  // Op enum drifted apart. Never dispatch with a hole.
  for (std::uint32_t b2i = 0; b2i < b2::rbc::opCount(); ++b2i) {
    if (kB2DispatchTable[b2i] == nullptr) {
      (void)raise(kClsInternalError, "dispatch table hole (build bug)");
      return RunStatus::Threw;
    }
  }

  Frame* frp = &frames.back();
  rbc::Ins ins{};
  // `fr` as a macro over frp (see the banner above): re-acquired at every
  // fetch, never a dangling reference. Scoped to this function; #undef'd
  // at the dispatch loop's end.
#define fr (*frp)
b2_redispatch:
  B2_DISPATCH();

#else  // !B2_INTERP_COMPUTED_GOTO: the portable token-threaded switch core.

#define B2_TARGET(op) case rbc::Op::op:
#define B2_NEXT break
#define B2_REDISPATCH() continue

  Frame* frp = &frames.back();
  rbc::Ins ins{};
#define fr (*frp)
  for (;;) {
    B2_FETCH_AND_PROBE();
    switch (ins.opcode()) {

#endif  // B2_INTERP_COMPUTED_GOTO


    // ------------------------------------------------------------------ misc
    B2_TARGET(Nop)
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(SafepointPoll) {
      if (cfg.collectStats) {
        ++out.stats.polls;
      }
      if (cfg.traceSafepoints) {
        // The deopt-fixture stream (Frame.h pinned format; Rule 124).
        dumpFrames(frames, rt, out.safepointTrace);
      }
      // Rule 111 handshake, v0 form: observe the request and CONTINUE.
      // Single-threaded v0 has no parking protocol - the flag's only
      // observable effect is through safepointRequested(); the real
      // handshake (release-store + park) lands with the multi-threaded
      // runtime. Latency is bounded by construction: loops carry
      // safepoint_poll on backedges by lowering convention.
      if (Interpreter::safepointRequested()) {
        // v0: record-only poll (nothing to park).
      }
      ++fr.pc;
      B2_NEXT;
    }

    // ------------------------------------------------------------- constants
    B2_TARGET(AconstNull)
      fr.regs[ins.dst] = Value::nullVal();
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Iconst)
      fr.regs[ins.dst] = Value::intVal(static_cast<std::int32_t>(ins.imm));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Fconst)
      // imm is the IEEE 754 bit pattern (rbc_spec.md SS3.2).
      fr.regs[ins.dst] = Value::floatVal(std::bit_cast<float>(ins.imm));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Lconst)
      fr.regs[ins.dst] = Value::longVal(fr.method->cp[ins.imm].i64);
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Dconst)
      fr.regs[ins.dst] = Value::doubleVal(fr.method->cp[ins.imm].f64);
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Ldc) {
      const rbc::Const& c = fr.method->cp[ins.imm];
      switch (c.kind) {
      case rbc::Const::Kind::String:
        // JVMS 5.1 interning: equal constants are one object (if_acmpeq).
        fr.regs[ins.dst] = Value::refVal(rt.internString(c.str));
        ++fr.pc;
        break;
      case rbc::Const::Kind::Class: {
        // ldc-of-Class is a JVMS 5.5 initialization trigger.
        const ClassId cls = rt.classId(c.str);
        const InitFlow inited = initClassIfNeeded(cls);
        if (inited == InitFlow::UnwoundEmpty) {
          return RunStatus::Threw;
        }
        if (inited == InitFlow::PushedClinit) {
          break; // frame pushed; this ldc re-executes after <clinit>
        }
        fr.regs[ins.dst] = Value::refVal(rt.classObject(cls));
        ++fr.pc;
        break;
      }
      case rbc::Const::Kind::MethodType:
      case rbc::Const::Kind::MethodHandle:
        // No method-handle runtime in v0: honest refusal, never silent.
        if (!raise(kClsInternalError, kMsgLdcUnsupported)) {
          return RunStatus::Threw;
        }
        break;
      default:
        // Verifier-restricted to String/Class/MethodType/MethodHandle.
        if (!raise(kClsInternalError, "ldc of unsupported constant kind")) {
          return RunStatus::Threw;
        }
        break;
      }
      B2_NEXT;
    }

    // ---------------------------------------------- locals <-> registers
    // Plain Value copies; no retagging (the verifier proved the slot types).
    B2_TARGET(Iload)
    B2_TARGET(Lload)
    B2_TARGET(Fload)
    B2_TARGET(Dload)
    B2_TARGET(Aload)
      fr.regs[ins.dst] = fr.locals[ins.imm];
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Istore)
    B2_TARGET(Lstore)
    B2_TARGET(Fstore)
    B2_TARGET(Dstore)
    B2_TARGET(Astore)
      fr.locals[ins.imm] = fr.regs[ins.a];
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Imove)
    B2_TARGET(Lmove)
    B2_TARGET(Fmove)
    B2_TARGET(Dmove)
    B2_TARGET(Amove)
      fr.regs[ins.dst] = fr.regs[ins.a];
      ++fr.pc;
      B2_NEXT;

    // -------------------------------------------------- int arithmetic
    B2_TARGET(Iadd)
      fr.regs[ins.dst] = Value::intVal(
          wrapAdd32(fr.regs[ins.a].as.i, fr.regs[ins.b].as.i));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Isub)
      fr.regs[ins.dst] = Value::intVal(
          wrapSub32(fr.regs[ins.a].as.i, fr.regs[ins.b].as.i));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Imul)
      fr.regs[ins.dst] = Value::intVal(
          wrapMul32(fr.regs[ins.a].as.i, fr.regs[ins.b].as.i));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Idiv) {
      const std::int32_t a = fr.regs[ins.a].as.i;
      const std::int32_t b = fr.regs[ins.b].as.i;
      if (b == 0) {
        if (!raise(kClsArithmetic, kMsgDivByZero)) {
          return RunStatus::Threw;
        }
        B2_NEXT; // caught: fr.pc is the handler entry
      }
      std::int32_t r;
      if (a == std::numeric_limits<std::int32_t>::min() && b == -1) {
        r = std::numeric_limits<std::int32_t>::min(); // JLS 15.17.2
      } else {
        r = a / b; // C++ division rounds toward zero, same as Java
      }
      fr.regs[ins.dst] = Value::intVal(r);
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Irem) {
      const std::int32_t a = fr.regs[ins.a].as.i;
      const std::int32_t b = fr.regs[ins.b].as.i;
      if (b == 0) {
        if (!raise(kClsArithmetic, kMsgDivByZero)) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      std::int32_t r;
      if (a == std::numeric_limits<std::int32_t>::min() && b == -1) {
        r = 0; // JLS 15.17.3 (and avoids the C++ UB case)
      } else {
        r = a % b; // sign of dividend, same as Java
      }
      fr.regs[ins.dst] = Value::intVal(r);
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Ineg)
      fr.regs[ins.dst] = Value::intVal(neg32(fr.regs[ins.a].as.i));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Ishl) {
      const std::uint32_t count =
          static_cast<std::uint32_t>(fr.regs[ins.b].as.i) & kIntShiftMask;
      fr.regs[ins.dst] = Value::intVal(static_cast<std::int32_t>(
          static_cast<std::uint32_t>(fr.regs[ins.a].as.i) << count));
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Ishr) {
      const std::uint32_t count =
          static_cast<std::uint32_t>(fr.regs[ins.b].as.i) & kIntShiftMask;
      // C++20+ defines signed right shift as arithmetic ([expr.shift]).
      fr.regs[ins.dst] = Value::intVal(fr.regs[ins.a].as.i >> count);
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Iushr) {
      const std::uint32_t count =
          static_cast<std::uint32_t>(fr.regs[ins.b].as.i) & kIntShiftMask;
      fr.regs[ins.dst] = Value::intVal(static_cast<std::int32_t>(
          static_cast<std::uint32_t>(fr.regs[ins.a].as.i) >> count));
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Iand)
      fr.regs[ins.dst] =
          Value::intVal(fr.regs[ins.a].as.i & fr.regs[ins.b].as.i);
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Ior)
      fr.regs[ins.dst] =
          Value::intVal(fr.regs[ins.a].as.i | fr.regs[ins.b].as.i);
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Ixor)
      fr.regs[ins.dst] =
          Value::intVal(fr.regs[ins.a].as.i ^ fr.regs[ins.b].as.i);
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Iinc)
      // The only read-modify-write arithmetic op: dst is both source and
      // destination (rbc_spec.md SS3.5).
      fr.regs[ins.dst] = Value::intVal(
          wrapAdd32(fr.regs[ins.dst].as.i, static_cast<std::int32_t>(ins.imm)));
      ++fr.pc;
      B2_NEXT;

    // ------------------------------------------------- long arithmetic
    B2_TARGET(Ladd)
      fr.regs[ins.dst] = Value::longVal(
          wrapAdd64(fr.regs[ins.a].as.l, fr.regs[ins.b].as.l));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Lsub)
      fr.regs[ins.dst] = Value::longVal(
          wrapSub64(fr.regs[ins.a].as.l, fr.regs[ins.b].as.l));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Lmul)
      fr.regs[ins.dst] = Value::longVal(
          wrapMul64(fr.regs[ins.a].as.l, fr.regs[ins.b].as.l));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Ldiv) {
      const std::int64_t a = fr.regs[ins.a].as.l;
      const std::int64_t b = fr.regs[ins.b].as.l;
      if (b == 0) {
        if (!raise(kClsArithmetic, kMsgDivByZero)) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      std::int64_t r;
      if (a == std::numeric_limits<std::int64_t>::min() && b == -1) {
        r = std::numeric_limits<std::int64_t>::min(); // JLS 15.17.2
      } else {
        r = a / b;
      }
      fr.regs[ins.dst] = Value::longVal(r);
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Lrem) {
      const std::int64_t a = fr.regs[ins.a].as.l;
      const std::int64_t b = fr.regs[ins.b].as.l;
      if (b == 0) {
        if (!raise(kClsArithmetic, kMsgDivByZero)) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      std::int64_t r;
      if (a == std::numeric_limits<std::int64_t>::min() && b == -1) {
        r = 0; // JLS 15.17.3
      } else {
        r = a % b;
      }
      fr.regs[ins.dst] = Value::longVal(r);
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Lneg)
      fr.regs[ins.dst] = Value::longVal(neg64(fr.regs[ins.a].as.l));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Lshl) {
      // The count register is Int (rbc_spec.md SS3.6).
      const std::uint32_t count =
          static_cast<std::uint32_t>(fr.regs[ins.b].as.i) & kLongShiftMask;
      fr.regs[ins.dst] = Value::longVal(static_cast<std::int64_t>(
          static_cast<std::uint64_t>(fr.regs[ins.a].as.l) << count));
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Lshr) {
      const std::uint32_t count =
          static_cast<std::uint32_t>(fr.regs[ins.b].as.i) & kLongShiftMask;
      fr.regs[ins.dst] = Value::longVal(fr.regs[ins.a].as.l >> count);
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Lushr) {
      const std::uint32_t count =
          static_cast<std::uint32_t>(fr.regs[ins.b].as.i) & kLongShiftMask;
      fr.regs[ins.dst] = Value::longVal(static_cast<std::int64_t>(
          static_cast<std::uint64_t>(fr.regs[ins.a].as.l) >> count));
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Land)
      fr.regs[ins.dst] =
          Value::longVal(fr.regs[ins.a].as.l & fr.regs[ins.b].as.l);
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Lor)
      fr.regs[ins.dst] =
          Value::longVal(fr.regs[ins.a].as.l | fr.regs[ins.b].as.l);
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Lxor)
      fr.regs[ins.dst] =
          Value::longVal(fr.regs[ins.a].as.l ^ fr.regs[ins.b].as.l);
      ++fr.pc;
      B2_NEXT;

    // ------------------------------------------------- float arithmetic
    // IEEE 754 plain C++ ops: no traps ever; division by zero produces an
    // infinity (rbc_spec.md SS3.7).
    B2_TARGET(Fadd)
      fr.regs[ins.dst] =
          Value::floatVal(fr.regs[ins.a].as.f + fr.regs[ins.b].as.f);
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Fsub)
      fr.regs[ins.dst] =
          Value::floatVal(fr.regs[ins.a].as.f - fr.regs[ins.b].as.f);
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Fmul)
      fr.regs[ins.dst] =
          Value::floatVal(fr.regs[ins.a].as.f * fr.regs[ins.b].as.f);
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Fdiv)
      fr.regs[ins.dst] =
          Value::floatVal(fr.regs[ins.a].as.f / fr.regs[ins.b].as.f);
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Frem)
      // JLS 15.17.3: Java's % on floats is C fmod (truncated quotient), NOT
      // the IEEE 754 remainder - rbc_spec.md SS3.7's "IEEE remainder" wording
      // is a spec typo (reported). std::fmod is exact for the semantics.
      fr.regs[ins.dst] =
          Value::floatVal(std::fmod(fr.regs[ins.a].as.f, fr.regs[ins.b].as.f));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Fneg)
      // Flips the sign bit, including on NaN (IEEE 754 negation).
      fr.regs[ins.dst] = Value::floatVal(-fr.regs[ins.a].as.f);
      ++fr.pc;
      B2_NEXT;

    // ------------------------------------------------- double arithmetic
    B2_TARGET(Dadd)
      fr.regs[ins.dst] =
          Value::doubleVal(fr.regs[ins.a].as.d + fr.regs[ins.b].as.d);
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Dsub)
      fr.regs[ins.dst] =
          Value::doubleVal(fr.regs[ins.a].as.d - fr.regs[ins.b].as.d);
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Dmul)
      fr.regs[ins.dst] =
          Value::doubleVal(fr.regs[ins.a].as.d * fr.regs[ins.b].as.d);
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Ddiv)
      fr.regs[ins.dst] =
          Value::doubleVal(fr.regs[ins.a].as.d / fr.regs[ins.b].as.d);
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Drem)
      fr.regs[ins.dst] =
          Value::doubleVal(std::fmod(fr.regs[ins.a].as.d, fr.regs[ins.b].as.d));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(Dneg)
      fr.regs[ins.dst] = Value::doubleVal(-fr.regs[ins.a].as.d);
      ++fr.pc;
      B2_NEXT;

    // ------------------------------------------------------- comparisons
    B2_TARGET(Icmp) {
      const std::int32_t a = fr.regs[ins.a].as.i;
      const std::int32_t b = fr.regs[ins.b].as.i;
      fr.regs[ins.dst] = Value::intVal((a > b) - (a < b));
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Lcmp) {
      const std::int64_t a = fr.regs[ins.a].as.l;
      const std::int64_t b = fr.regs[ins.b].as.l;
      fr.regs[ins.dst] = Value::intVal(static_cast<std::int32_t>((a > b) - (a < b)));
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Fcmpl) {
      const float a = fr.regs[ins.a].as.f;
      const float b = fr.regs[ins.b].as.f;
      const std::int32_t r =
          (std::isnan(a) || std::isnan(b)) ? -1 : (a > b) - (a < b);
      fr.regs[ins.dst] = Value::intVal(r);
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Fcmpg) {
      const float a = fr.regs[ins.a].as.f;
      const float b = fr.regs[ins.b].as.f;
      const std::int32_t r =
          (std::isnan(a) || std::isnan(b)) ? 1 : (a > b) - (a < b);
      fr.regs[ins.dst] = Value::intVal(r);
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Dcmpl) {
      const double a = fr.regs[ins.a].as.d;
      const double b = fr.regs[ins.b].as.d;
      const std::int32_t r =
          (std::isnan(a) || std::isnan(b)) ? -1 : (a > b) - (a < b);
      fr.regs[ins.dst] = Value::intVal(r);
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Dcmpg) {
      const double a = fr.regs[ins.a].as.d;
      const double b = fr.regs[ins.b].as.d;
      const std::int32_t r =
          (std::isnan(a) || std::isnan(b)) ? 1 : (a > b) - (a < b);
      fr.regs[ins.dst] = Value::intVal(r);
      ++fr.pc;
      B2_NEXT;
    }

    // ------------------------------------------------------- conversions
    B2_TARGET(I2l)
      fr.regs[ins.dst] = Value::longVal(
          static_cast<std::int64_t>(fr.regs[ins.a].as.i));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(I2f)
      fr.regs[ins.dst] =
          Value::floatVal(static_cast<float>(fr.regs[ins.a].as.i));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(I2d)
      fr.regs[ins.dst] =
          Value::doubleVal(static_cast<double>(fr.regs[ins.a].as.i));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(L2i)
      fr.regs[ins.dst] = Value::intVal(
          static_cast<std::int32_t>(static_cast<std::uint64_t>(
              fr.regs[ins.a].as.l))); // low 32 bits (unsigned path: no UB)
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(L2f)
      fr.regs[ins.dst] =
          Value::floatVal(static_cast<float>(fr.regs[ins.a].as.l));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(L2d)
      fr.regs[ins.dst] =
          Value::doubleVal(static_cast<double>(fr.regs[ins.a].as.l));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(F2i)
      fr.regs[ins.dst] = Value::intVal(toInt32Sat(fr.regs[ins.a].as.f));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(F2l)
      fr.regs[ins.dst] = Value::longVal(toInt64Sat(fr.regs[ins.a].as.f));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(F2d)
      fr.regs[ins.dst] =
          Value::doubleVal(static_cast<double>(fr.regs[ins.a].as.f));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(D2i)
      fr.regs[ins.dst] = Value::intVal(toInt32Sat(fr.regs[ins.a].as.d));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(D2l)
      fr.regs[ins.dst] = Value::longVal(toInt64Sat(fr.regs[ins.a].as.d));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(D2f)
      fr.regs[ins.dst] =
          Value::floatVal(static_cast<float>(fr.regs[ins.a].as.d));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(I2b)
      fr.regs[ins.dst] = Value::intVal(static_cast<std::int32_t>(
          static_cast<std::int8_t>(fr.regs[ins.a].as.i)));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(I2c)
      fr.regs[ins.dst] = Value::intVal(static_cast<std::int32_t>(
          static_cast<std::uint16_t>(fr.regs[ins.a].as.i)));
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(I2s)
      fr.regs[ins.dst] = Value::intVal(static_cast<std::int32_t>(
          static_cast<std::int16_t>(fr.regs[ins.a].as.i)));
      ++fr.pc;
      B2_NEXT;

    // -------------------------------------------------- branches/switches
    B2_TARGET(Goto)
      if (ins.imm < fr.pc) {
        rt.bumpBackedge(MethodId{methodIndex(fr)});
      }
      fr.pc = ins.imm;
      B2_NEXT;

    B2_TARGET(Ifeq)
      if (fr.regs[ins.a].as.i == 0) {
        if (ins.imm < fr.pc) {
          rt.bumpBackedge(MethodId{methodIndex(fr)});
        }
        fr.pc = ins.imm;
      } else {
        ++fr.pc;
      }
      B2_NEXT;

    B2_TARGET(Ifne)
      if (fr.regs[ins.a].as.i != 0) {
        if (ins.imm < fr.pc) {
          rt.bumpBackedge(MethodId{methodIndex(fr)});
        }
        fr.pc = ins.imm;
      } else {
        ++fr.pc;
      }
      B2_NEXT;

    B2_TARGET(Iflt)
      if (fr.regs[ins.a].as.i < 0) {
        if (ins.imm < fr.pc) {
          rt.bumpBackedge(MethodId{methodIndex(fr)});
        }
        fr.pc = ins.imm;
      } else {
        ++fr.pc;
      }
      B2_NEXT;

    B2_TARGET(Ifge)
      if (fr.regs[ins.a].as.i >= 0) {
        if (ins.imm < fr.pc) {
          rt.bumpBackedge(MethodId{methodIndex(fr)});
        }
        fr.pc = ins.imm;
      } else {
        ++fr.pc;
      }
      B2_NEXT;

    B2_TARGET(Ifgt)
      if (fr.regs[ins.a].as.i > 0) {
        if (ins.imm < fr.pc) {
          rt.bumpBackedge(MethodId{methodIndex(fr)});
        }
        fr.pc = ins.imm;
      } else {
        ++fr.pc;
      }
      B2_NEXT;

    B2_TARGET(Ifle)
      if (fr.regs[ins.a].as.i <= 0) {
        if (ins.imm < fr.pc) {
          rt.bumpBackedge(MethodId{methodIndex(fr)});
        }
        fr.pc = ins.imm;
      } else {
        ++fr.pc;
      }
      B2_NEXT;

    B2_TARGET(Ifnull)
      if (fr.regs[ins.a].isNull()) {
        if (ins.imm < fr.pc) {
          rt.bumpBackedge(MethodId{methodIndex(fr)});
        }
        fr.pc = ins.imm;
      } else {
        ++fr.pc;
      }
      B2_NEXT;

    B2_TARGET(Ifnonnull)
      if (!fr.regs[ins.a].isNull()) {
        if (ins.imm < fr.pc) {
          rt.bumpBackedge(MethodId{methodIndex(fr)});
        }
        fr.pc = ins.imm;
      } else {
        ++fr.pc;
      }
      B2_NEXT;

    B2_TARGET(IfIcmpeq)
      if (fr.regs[ins.a].as.i == fr.regs[ins.b].as.i) {
        if (ins.imm < fr.pc) {
          rt.bumpBackedge(MethodId{methodIndex(fr)});
        }
        fr.pc = ins.imm;
      } else {
        ++fr.pc;
      }
      B2_NEXT;

    B2_TARGET(IfIcmpne)
      if (fr.regs[ins.a].as.i != fr.regs[ins.b].as.i) {
        if (ins.imm < fr.pc) {
          rt.bumpBackedge(MethodId{methodIndex(fr)});
        }
        fr.pc = ins.imm;
      } else {
        ++fr.pc;
      }
      B2_NEXT;

    B2_TARGET(IfIcmplt)
      if (fr.regs[ins.a].as.i < fr.regs[ins.b].as.i) {
        if (ins.imm < fr.pc) {
          rt.bumpBackedge(MethodId{methodIndex(fr)});
        }
        fr.pc = ins.imm;
      } else {
        ++fr.pc;
      }
      B2_NEXT;

    B2_TARGET(IfIcmpge)
      if (fr.regs[ins.a].as.i >= fr.regs[ins.b].as.i) {
        if (ins.imm < fr.pc) {
          rt.bumpBackedge(MethodId{methodIndex(fr)});
        }
        fr.pc = ins.imm;
      } else {
        ++fr.pc;
      }
      B2_NEXT;

    B2_TARGET(IfIcmpgt)
      if (fr.regs[ins.a].as.i > fr.regs[ins.b].as.i) {
        if (ins.imm < fr.pc) {
          rt.bumpBackedge(MethodId{methodIndex(fr)});
        }
        fr.pc = ins.imm;
      } else {
        ++fr.pc;
      }
      B2_NEXT;

    B2_TARGET(IfIcmple)
      if (fr.regs[ins.a].as.i <= fr.regs[ins.b].as.i) {
        if (ins.imm < fr.pc) {
          rt.bumpBackedge(MethodId{methodIndex(fr)});
        }
        fr.pc = ins.imm;
      } else {
        ++fr.pc;
      }
      B2_NEXT;

    B2_TARGET(IfAcmpeq)
      if (Value::sameObject(fr.regs[ins.a], fr.regs[ins.b])) {
        if (ins.imm < fr.pc) {
          rt.bumpBackedge(MethodId{methodIndex(fr)});
        }
        fr.pc = ins.imm;
      } else {
        ++fr.pc;
      }
      B2_NEXT;

    B2_TARGET(IfAcmpne)
      if (!Value::sameObject(fr.regs[ins.a], fr.regs[ins.b])) {
        if (ins.imm < fr.pc) {
          rt.bumpBackedge(MethodId{methodIndex(fr)});
        }
        fr.pc = ins.imm;
      } else {
        ++fr.pc;
      }
      B2_NEXT;

    B2_TARGET(Tableswitch) {
      // AMBIGUITY RESOLUTION A1: the SwitchTable payload follows the CANONICAL
      // layouts that the landed Verifier/RbcText/RbcBuilder implement and
      // enforce - tableswitch: [low, high, default, t(low), ..., t(high)];
      // selector in [low, high] jumps to ints[3 + (sel - low)], anything else
      // to ints[2] (default). rbc_spec.md SS3.11/P6's flat-pairs pin describes
      // a pre-integration contract that the verifier rejects; the verifier is
      // the hard gate, so this is the only executable reading. The default
      // stored by the builder IS pc + 1 (P7 honored at the producer).
      const std::vector<std::int32_t>& tbl =
          fr.method->cp[ins.imm].ints;
      const std::int32_t sel = fr.regs[ins.a].as.i;
      const std::int64_t low = tbl[0];
      const std::int64_t high = tbl[1];
      std::uint32_t target = static_cast<std::uint32_t>(tbl[2]);
      if (sel >= low && sel <= high) {
        target = static_cast<std::uint32_t>(
            tbl[3 + static_cast<std::size_t>(sel - low)]);
      }
      if (target < fr.pc) {
        rt.bumpBackedge(MethodId{methodIndex(fr)});
      }
      fr.pc = target;
      B2_NEXT;
    }

    B2_TARGET(Lookupswitch) {
      // Canonical layout: [N, default, match0, t0, ..., match(N-1), t(N-1)],
      // matches strictly ascending (verifier V-S6). Linear scan.
      const std::vector<std::int32_t>& tbl =
          fr.method->cp[ins.imm].ints;
      const std::int32_t sel = fr.regs[ins.a].as.i;
      std::uint32_t target = static_cast<std::uint32_t>(tbl[1]);
      const std::size_t pairs =
          tbl[0] > 0 ? static_cast<std::size_t>(tbl[0]) : 0u; // hostile clamp
      for (std::size_t i = 0; i < pairs; ++i) {
        if (tbl[2 + 2 * i] == sel) {
          target = static_cast<std::uint32_t>(tbl[3 + 2 * i]);
          break;
        }
      }
      if (target < fr.pc) {
        rt.bumpBackedge(MethodId{methodIndex(fr)});
      }
      fr.pc = target;
      B2_NEXT;
    }

    // ------------------------------------------------------------- fields
    B2_TARGET(Getfield) {
      const Value& obj = fr.regs[ins.a];
      if (obj.isNull()) {
        if (!raiseNpe()) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const std::uint32_t mIdx = methodIndex(fr);
      // IC probe (site key = (MethodId, pc)); lazily sized on miss only.
      bool hit = false;
      std::uint32_t slot = 0;
      if (mIdx < siteICs.size() && fr.pc < siteICs[mIdx].size() &&
          siteICs[mIdx][fr.pc].valid) {
        hit = true;
        slot = siteICs[mIdx][fr.pc].x;
      }
      if (!hit) {
        if (cfg.collectStats) {
          ++out.stats.icMisses;
        }
        const std::optional<ResolvedField> rf =
            rt.resolveField(fr.method->cp[ins.imm]);
        if (!rf) {
          if (!raise(kClsInternalError, kMsgUnresolvedField)) {
            return RunStatus::Threw;
          }
          B2_NEXT;
        }
        slot = rf->slot;
        fillSiteIC(mIdx, fr.pc, rf->slot, rf->cls.v);
      } else if (cfg.collectStats) {
        ++out.stats.icHits;
      }
      Value v = rt.heap().loadField(obj.ref(), slot);
      if (v.type == rbc::RType::Bottom) {
        // AMBIGUITY RESOLUTION A5: Bottom here means "never written" (the
        // heap's unset marker) - the field still carries its JLS 4.12.5
        // default. Coerce through the resolved descriptor so the register
        // keeps its verified type (Value.h deopt-state contract). The hit
        // path lost the descriptor, so re-resolve (idempotent, cold path).
        if (!rt.heap().isInstance(obj.ref())) {
          if (!raise(kClsInternalError, "field access on non-instance")) {
            return RunStatus::Threw;
          }
          B2_NEXT;
        }
        const std::optional<ResolvedField> rf =
            rt.resolveField(fr.method->cp[ins.imm]);
        const rbc::RType ft = rf ? rf->type : rbc::RType::Bottom;
        if (ft == rbc::RType::Bottom) {
          if (!raise(kClsInternalError, "malformed field descriptor")) {
            return RunStatus::Threw;
          }
          B2_NEXT;
        }
        v = zeroOfField(ft);
      }
      fr.regs[ins.dst] = v;
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Putfield) {
      const Value& obj = fr.regs[ins.a];
      if (obj.isNull()) {
        if (!raiseNpe()) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const std::uint32_t mIdx = methodIndex(fr);
      bool hit = false;
      std::uint32_t slot = 0;
      if (mIdx < siteICs.size() && fr.pc < siteICs[mIdx].size() &&
          siteICs[mIdx][fr.pc].valid) {
        hit = true;
        slot = siteICs[mIdx][fr.pc].x;
      }
      if (!hit) {
        if (cfg.collectStats) {
          ++out.stats.icMisses;
        }
        const std::optional<ResolvedField> rf =
            rt.resolveField(fr.method->cp[ins.imm]);
        if (!rf) {
          if (!raise(kClsInternalError, kMsgUnresolvedField)) {
            return RunStatus::Threw;
          }
          B2_NEXT;
        }
        slot = rf->slot;
        fillSiteIC(mIdx, fr.pc, rf->slot, rf->cls.v);
      } else if (cfg.collectStats) {
        ++out.stats.icHits;
      }
      if (!rt.heap().storeField(obj.ref(), slot, fr.regs[ins.b])) {
        if (!raise(kClsInternalError, "field access on non-instance")) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      ++fr.pc; // dst unused (spec pin)
      B2_NEXT;
    }

    B2_TARGET(Getstatic) {
      const rbc::Const& c = fr.method->cp[ins.imm];
      // Builtin statics FIRST (System.out/err singletons) - before
      // resolution and before any class-init consideration.
      const std::optional<ObjRef> builtin = rt.builtinStatic(c);
      if (builtin) {
        fr.regs[ins.dst] = Value::refVal(*builtin);
        ++fr.pc;
        B2_NEXT;
      }
      const std::optional<ResolvedField> rf = rt.resolveField(c);
      if (!rf) {
        if (!raise(kClsInternalError, kMsgUnresolvedField)) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      // JVMS 5.5 trigger on the FieldRef's declaring class. needsInit gates
      // on the program class internally (BE-2 pin) - call and trust it.
      const InitFlow inited = initClassIfNeeded(rf->cls);
      if (inited == InitFlow::UnwoundEmpty) {
        return RunStatus::Threw;
      }
      if (inited == InitFlow::PushedClinit) {
        B2_NEXT; // re-execute this getstatic after <clinit>
      }
      Value v = rt.loadStatic(rt.fieldIdOf(*rf));
      if (v.type == rbc::RType::Bottom) {
        // A5: unset statics default to the field type's zero (JLS 4.12.5 /
        // 8.3.2); Bottom would violate the slot-type contract.
        const rbc::RType ft = rf->type;
        if (ft == rbc::RType::Bottom) {
          if (!raise(kClsInternalError, "malformed field descriptor")) {
            return RunStatus::Threw;
          }
          B2_NEXT;
        }
        v = zeroOfField(ft);
      }
      fr.regs[ins.dst] = v;
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Putstatic) {
      const rbc::Const& c = fr.method->cp[ins.imm];
      const std::optional<ObjRef> builtin = rt.builtinStatic(c);
      if (builtin) {
        // Defensive: the singletons are final builtins; storing to them is
        // not expressible in Java. Refuse rather than corrupt the identity
        // every getstatic depends on.
        if (!raise(kClsInternalError, "store to builtin static")) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const std::optional<ResolvedField> rf = rt.resolveField(c);
      if (!rf) {
        if (!raise(kClsInternalError, kMsgUnresolvedField)) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const InitFlow inited = initClassIfNeeded(rf->cls);
      if (inited == InitFlow::UnwoundEmpty) {
        return RunStatus::Threw;
      }
      if (inited == InitFlow::PushedClinit) {
        B2_NEXT; // re-execute this putstatic after <clinit>
      }
      rt.storeStatic(rt.fieldIdOf(*rf), fr.regs[ins.dst]); // dst is READ
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(GetfieldQuick) {
      // Quickened pin: imm is the resolved BYTE offset; no cp, no IC.
      const Value& obj = fr.regs[ins.a];
      if (obj.isNull()) {
        if (!raiseNpe()) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const std::uint32_t slot = rt.slotOfFieldOffset(ins.imm);
      Value v = rt.heap().loadField(obj.ref(), slot);
      if (v.type == rbc::RType::Bottom) {
        // A5, quickened corner: the descriptor is gone after in-place
        // quickening, so an unwritten field's default value is unknowable
        // here. Honest refusal (v0): write the field before quickening its
        // reads. Reported with the other quickened-pin gaps.
        if (!rt.heap().isInstance(obj.ref())) {
          if (!raise(kClsInternalError, "field access on non-instance")) {
            return RunStatus::Threw;
          }
          B2_NEXT;
        }
        if (!raise(kClsInternalError,
                   "quickened getfield of unwritten field (v0)")) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      fr.regs[ins.dst] = v;
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(PutfieldQuick) {
      const Value& obj = fr.regs[ins.a];
      if (obj.isNull()) {
        if (!raiseNpe()) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const std::uint32_t slot = rt.slotOfFieldOffset(ins.imm);
      if (!rt.heap().storeField(obj.ref(), slot, fr.regs[ins.b])) {
        if (!raise(kClsInternalError, "field access on non-instance")) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      ++fr.pc;
      B2_NEXT;
    }

    // ------------------------------------------------------------- arrays
    B2_TARGET(NewArray) {
      const std::int32_t len = fr.regs[ins.a].as.i;
      if (len < 0) {
        // JVM pin: the message is the decimal size.
        if (!raise(kClsNegativeArraySize, std::to_string(len))) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      fr.regs[ins.dst] = Value::refVal(rt.newArray(
          static_cast<rbc::Atype>(ins.imm), static_cast<std::uint32_t>(len)));
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(AnewArray) {
      const std::int32_t len = fr.regs[ins.a].as.i;
      if (len < 0) {
        if (!raise(kClsNegativeArraySize, std::to_string(len))) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      fr.regs[ins.dst] = Value::refVal(
          rt.newRefArray(rt.classId(fr.method->cp[ins.imm].str),
                         static_cast<std::uint32_t>(len)));
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Arraylength) {
      const Value& arr = fr.regs[ins.a];
      if (arr.isNull()) {
        if (!raiseNpe()) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      if (!rt.heap().isArray(arr.ref())) {
        // Defensive: verified streams only see arrays here.
        if (!raise(kClsInternalError, "arraylength on non-array")) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      fr.regs[ins.dst] =
          Value::intVal(static_cast<std::int32_t>(
              rt.heap().arrayLength(arr.ref())));
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Iaload)
    B2_TARGET(Laload)
    B2_TARGET(Faload)
    B2_TARGET(Daload)
    B2_TARGET(Aaload)
    B2_TARGET(Baload)
    B2_TARGET(Caload)
    B2_TARGET(Saload)
      if (!arrayLoad(fr, ins)) {
        return RunStatus::Threw;
      }
      B2_NEXT;

    B2_TARGET(Iastore)
    B2_TARGET(Lastore)
    B2_TARGET(Fastore)
    B2_TARGET(Dastore)
      if (!arrayStore(fr, ins, StoreNarrow::None)) {
        return RunStatus::Threw;
      }
      B2_NEXT;

    B2_TARGET(Bastore)
      if (!arrayStore(fr, ins, StoreNarrow::Byte)) {
        return RunStatus::Threw;
      }
      B2_NEXT;

    B2_TARGET(Castore)
      if (!arrayStore(fr, ins, StoreNarrow::Char)) {
        return RunStatus::Threw;
      }
      B2_NEXT;

    B2_TARGET(Sastore)
      if (!arrayStore(fr, ins, StoreNarrow::Short)) {
        return RunStatus::Threw;
      }
      B2_NEXT;

    B2_TARGET(Aastore) {
      const Value& arr = fr.regs[ins.a];
      if (arr.isNull()) {
        if (!raiseNpe()) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const std::int32_t idx = fr.regs[ins.b].as.i;
      const std::uint32_t len = rt.heap().arrayLength(arr.ref());
      if (idx < 0 || static_cast<std::uint32_t>(idx) >= len) {
        if (!raiseAioobe(idx, len)) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const Value& val = fr.regs[ins.dst]; // dst READ: the stored value
      if (!val.isNull()) {
        // Element-assignability check against the ARRAY's class name. v0 pin:
        // "[L<cls>;" elements compare via isAssignableFrom(<cls>, value
        // class); "[[..." (array of arrays) requires the exact element
        // descriptor (name minus one '['); primitive-component arrays are
        // unreachable on verified streams (defensive InternalError). The
        // class-name derivation is inherently string-based through the
        // frozen Runtime API (v0 seam: no array-element ClassId accessor).
        const ClassId arrayCls = rt.heap().classOf(arr.ref());
        const std::string_view arrayName = rt.classInfo(arrayCls).name;
        const ClassId valueCls = rt.heap().classOf(val.ref());
        bool ok = false;
        if (arrayName.size() > 2 && arrayName[0] == '[' &&
            arrayName[1] == 'L' && arrayName.back() == ';') {
          const std::string elem(arrayName.substr(2, arrayName.size() - 3));
          ok = rt.isAssignableFrom(rt.classId(elem), valueCls);
        } else if (arrayName.size() >= 2 && arrayName[0] == '[' &&
                   arrayName[1] == '[') {
          ok = rt.classInfo(valueCls).name == arrayName.substr(1);
        } else {
          if (!raise(kClsInternalError,
                     "aastore into primitive-component array")) {
            return RunStatus::Threw;
          }
          B2_NEXT;
        }
        if (!ok) {
          // JVM pin: "class <dotted value class>".
          if (!raise(kClsArrayStore, "class " + rt.dottedClassName(valueCls))) {
            return RunStatus::Threw;
          }
          B2_NEXT;
        }
      }
      if (!rt.heap().storeElem(arr.ref(), static_cast<std::uint32_t>(idx),
                               val)) {
        if (!raise(kClsInternalError, "array element access on non-array")) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Multianewarray) {
      if (ins.b == 0) {
        if (!raise(kClsInternalError, "multianewarray with zero dimensions")) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      if (ins.b > kMaxMultiDims) {
        // JVMS 4.4.1 bounds array descriptors at 255 dimensions; more
        // dimension registers than that is hostile input.
        if (!raise(kClsInternalError,
                   "multianewarray exceeds 255 dimensions")) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const std::string_view clsName = fr.method->cp[ins.imm].str;
      if (clsName.empty() || clsName.front() != '[') {
        if (!raise(kClsInternalError,
                   "multianewarray of non-array class")) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      // Dimension counts occupy consecutive registers a..a+b-1 (P5); the
      // stack buffer (no allocation) is bounded by the 255-dimension clamp.
      std::array<std::int32_t, kMaxMultiDims> dims{};
      bool trapped = false;
      for (std::size_t i = 0; i < ins.b; ++i) {
        const std::size_t reg = ins.a + i; // verified: a + b <= numRegs
        const std::int32_t d = reg < fr.regs.size() ? fr.regs[reg].as.i : 0;
        if (d < 0) {
          // JVM pin: the message is that dimension's decimal length.
          if (!raise(kClsNegativeArraySize, std::to_string(d))) {
            return RunStatus::Threw;
          }
          trapped = true;
          break; // caught: fr.pc is the handler entry; do not allocate
        }
        dims[i] = d;
      }
      if (trapped) {
        B2_NEXT;
      }
      // BE-2 pin: newMultiArray takes the FULL array class; the '[' count is
      // the total nest depth; trailing unspecified dims are length-0 arrays.
      const ObjRef arr = rt.newMultiArray(
          rt.classId(clsName),
          std::span<const std::int32_t>(dims.data(), ins.b));
      fr.regs[ins.dst] = Value::refVal(arr);
      ++fr.pc;
      B2_NEXT;
    }

    // -------------------------------------------------- objects/monitors
    B2_TARGET(New) {
      const ClassId cls = rt.classId(fr.method->cp[ins.imm].str);
      const InitFlow inited = initClassIfNeeded(cls);
      if (inited == InitFlow::UnwoundEmpty) {
        return RunStatus::Threw;
      }
      if (inited == InitFlow::PushedClinit) {
        B2_NEXT; // re-execute this new after <clinit>
      }
      fr.regs[ins.dst] = Value::refVal(rt.newInstance(cls));
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Checkcast) {
      const Value& v = fr.regs[ins.a];
      if (v.isNull()) {
        // Null passes every cast unchanged (JLS 5.5).
        fr.regs[ins.dst] = v;
        ++fr.pc;
        B2_NEXT;
      }
      const ClassId target = rt.classId(fr.method->cp[ins.imm].str);
      const ClassId actual = rt.heap().classOf(v.ref());
      if (rt.isAssignableFrom(target, actual)) {
        fr.regs[ins.dst] = v;
        ++fr.pc;
        B2_NEXT;
      }
      // JVM pin: "class <dotted runtime> cannot be cast to class
      // <dotted target>".
      if (!raise(kClsClassCast, "class " + rt.dottedClassName(actual) +
                                    " cannot be cast to class " +
                                    rt.dottedClassName(target))) {
        return RunStatus::Threw;
      }
      B2_NEXT;
    }

    B2_TARGET(Instanceof) {
      const Value& v = fr.regs[ins.a];
      if (v.isNull()) {
        fr.regs[ins.dst] = Value::intVal(0); // null is never an instance
        ++fr.pc;
        B2_NEXT;
      }
      const ClassId target = rt.classId(fr.method->cp[ins.imm].str);
      const ClassId actual = rt.heap().classOf(v.ref());
      fr.regs[ins.dst] =
          Value::intVal(rt.isAssignableFrom(target, actual) ? 1 : 0);
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Monitorenter) {
      const Value& v = fr.regs[ins.a];
      if (v.isNull()) {
        if (!raiseNpe()) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const Heap::MonFail mf = rt.heap().monitorenter(v.ref());
      if (mf == Heap::MonFail::Null) {
        if (!raiseNpe()) { // defensive: isNull() checked above
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      if (mf == Heap::MonFail::NotOwner) {
        // Unreachable single-threaded (no foreign owner exists); fail loudly.
        if (!raise(kClsIllegalMonitor, "")) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      // Frame.h pins the monitor record MOST-RECENT-FIRST, so the fresh
      // acquire goes to the FRONT (LIFO record; unwinding releases
      // front-to-back).
      fr.monitors.insert(fr.monitors.begin(), v.ref());
      ++fr.pc;
      B2_NEXT;
    }

    B2_TARGET(Monitorexit) {
      const Value& v = fr.regs[ins.a];
      if (v.isNull()) {
        if (!raiseNpe()) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const Heap::MonFail mf = rt.heap().monitorexit(v.ref());
      if (mf == Heap::MonFail::Null) {
        if (!raiseNpe()) { // defensive
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      if (mf == Heap::MonFail::NotOwner) {
        // JVM pin: IllegalMonitorStateException with no message.
        if (!raise(kClsIllegalMonitor, "")) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      // Remove the MOST RECENT occurrence (front scan; the record is
      // most-recent-first, so the first match is the most recent).
      for (std::size_t i = 0; i < fr.monitors.size(); ++i) {
        if (fr.monitors[i] == v.ref()) {
          fr.monitors.erase(fr.monitors.begin() +
                            static_cast<std::ptrdiff_t>(i));
          break;
        }
      }
      ++fr.pc;
      B2_NEXT;
    }

    // --------------------------------------------------------------- calls
    B2_TARGET(Invokevirtual)
    B2_TARGET(Invokeinterface) {
      // AMBIGUITY RESOLUTION A2: the receiver's NPE precedes EVERYTHING,
      // including the IC. Java traps invokeselect on null before dispatch;
      // an IC hit must not bypass that. The IC lookup itself never touches
      // the receiver, so hoisting the check is behavior-neutral for
      // non-null receivers.
      const Value& recv = fr.regs[ins.a];
      if (recv.isNull()) {
        if (!raiseNpe()) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const rbc::Const& ref = fr.method->cp[ins.imm];
      const std::uint32_t mIdx = methodIndex(fr);
      const std::uint32_t site = fr.pc; // profile/IC site key (Interp.h)
      const DispatchSiteKind kind =
          ins.opcode() == rbc::Op::Invokevirtual ? DispatchSiteKind::Virtual
                                                 : DispatchSiteKind::Interface;
      bool hit = false;
      std::uint32_t cached = 0;
      if (mIdx < siteICs.size() && site < siteICs[mIdx].size() &&
          siteICs[mIdx][site].valid) {
        hit = true;
        cached = siteICs[mIdx][site].x;
      }
      if (hit) {
        if (cfg.collectStats) {
          ++out.stats.icHits;
        }
        // The dispatch profile needs the receiver's TRUE class (the IC's y
        // slot is last-miss data and may have drifted): one header read,
        // BEFORE pushCall (fr/recv dangle after the push).
        const std::uint32_t recvCls = rt.heap().classOf(recv.ref()).v;
        if (!pushCall(ins, MethodId{cached})) {
          return RunStatus::Threw;
        }
        recordDispatch(mIdx, site, kind, recvCls, cached);
        B2_NEXT; // frames_ mutated: re-fetch, do not touch fr
      }
      if (cfg.collectStats) {
        ++out.stats.icMisses;
      }
      // Builtin probe (miss path only, Interp.h call protocol step 1).
      const Runtime::Builtin b = rt.lookupBuiltinVirtual(
          rt.heap().classOf(recv.ref()), ref.str2, ref.str3);
      if (b != Runtime::Builtin::None) {
        const std::size_t base =
            std::min<std::size_t>(ins.a, fr.regs.size());
        const std::size_t count =
            std::min<std::size_t>(fr.regs.size() - base, ins.b);
        if (!rt.execBuiltin(
                b, std::span<const Value>(fr.regs.data() + base, count))) {
          if (!raise(kClsInternalError, "builtin arity mismatch")) {
            return RunStatus::Threw;
          }
          B2_NEXT;
        }
        // Builtin sites deliberately never fill the IC: the cache stores RBC
        // MethodIds and execBuiltin has none to store, so builtin call sites
        // are permanent (cheap) misses - the profile still counts them
        // (site total only: the target is external, Interp.h).
        recordDispatch(mIdx, site, kind, kDispatchNoRecvClass,
                       kDispatchNoTarget);
        ++fr.pc;
        B2_NEXT;
      }
      const std::optional<MethodId> target = rt.resolveMethod(ref);
      if (!target) {
        // JVM pin: "<class>.<name><descriptor>", internal class name as-is.
        if (!raise(kClsNoSuchMethod,
                   ref.str + "." + ref.str2 + ref.str3)) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const std::uint32_t recvCls = rt.heap().classOf(recv.ref()).v;
      fillSiteIC(mIdx, site, target->v, recvCls);
      if (!pushCall(ins, *target)) {
        return RunStatus::Threw;
      }
      recordDispatch(mIdx, site, kind, recvCls, target->v);
      B2_NEXT;
    }

    B2_TARGET(Invokespecial) {
      // Direct dispatch: no IC (the target is static). The receiver NPE
      // precedes resolution (A2).
      const Value& recv = fr.regs[ins.a];
      if (recv.isNull()) {
        if (!raiseNpe()) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const std::optional<MethodId> target =
          rt.resolveMethod(fr.method->cp[ins.imm]);
      if (!target) {
        if (!raise(kClsNoSuchMethod, fr.method->cp[ins.imm].str + "." +
                                         fr.method->cp[ins.imm].str2 +
                                         fr.method->cp[ins.imm].str3)) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const std::uint32_t mIdx = methodIndex(fr);
      const std::uint32_t site = fr.pc; // profile site key (Interp.h)
      if (!pushCall(ins, *target)) {
        return RunStatus::Threw;
      }
      recordDispatch(mIdx, site, DispatchSiteKind::Special,
                     kDispatchNoRecvClass, target->v);
      B2_NEXT;
    }

    B2_TARGET(Invokestatic) {
      const std::optional<MethodId> target =
          rt.resolveMethod(fr.method->cp[ins.imm]);
      if (!target) {
        const rbc::Const& ref = fr.method->cp[ins.imm];
        if (!raise(kClsNoSuchMethod, ref.str + "." + ref.str2 + ref.str3)) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      // JVMS 5.5 trigger on the MethodRef's class. resolveMethod already
      // required ref.str == program_.className, so that class IS the interned
      // program class (no extra name work on this path).
      const InitFlow inited = initClassIfNeeded(programClass);
      if (inited == InitFlow::UnwoundEmpty) {
        return RunStatus::Threw;
      }
      if (inited == InitFlow::PushedClinit) {
        B2_NEXT; // re-execute this invokestatic after <clinit>
      }
      const std::uint32_t mIdx = methodIndex(fr);
      const std::uint32_t site = fr.pc; // profile site key (Interp.h)
      if (!pushCall(ins, *target)) {
        return RunStatus::Threw;
      }
      recordDispatch(mIdx, site, DispatchSiteKind::Static,
                     kDispatchNoRecvClass, target->v);
      B2_NEXT;
    }

    B2_TARGET(Invokedynamic)
      // v0 is verifier-only (rbc_spec.md SS10.1): no bootstrap machinery.
      if (!raise(kClsBootstrapMethod, kMsgIndyUnsupported)) {
        return RunStatus::Threw;
      }
      B2_NEXT;

    B2_TARGET(InvokevirtualQuick)
    B2_TARGET(InvokeinterfaceQuick) {
      // v0 pin: imm IS the IC site id (== the call pc on well-formed
      // quickened streams; combined with the executing method it identifies
      // the site, so quick and unquickened forms share IC state when
      // imm == pc).
      const Value& recv = fr.regs[ins.a];
      if (recv.isNull()) {
        if (!raiseNpe()) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      if (ins.imm >= fr.method->code.size()) {
        // Site ids index the per-method IC table; the pin bounds them by
        // the code size. A larger id is hostile (also guards the resize).
        if (!raise(kClsInternalError, "quickened call site id out of range")) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const std::uint32_t mIdx = methodIndex(fr);
      const DispatchSiteKind kind =
          ins.opcode() == rbc::Op::InvokevirtualQuick
              ? DispatchSiteKind::Virtual
              : DispatchSiteKind::Interface;
      bool hit = false;
      std::uint32_t cached = 0;
      if (mIdx < siteICs.size() && ins.imm < siteICs[mIdx].size() &&
          siteICs[mIdx][ins.imm].valid) {
        hit = true;
        cached = siteICs[mIdx][ins.imm].x;
      }
      if (hit) {
        if (cfg.collectStats) {
          ++out.stats.icHits;
        }
        // Same hit-path discipline as the un-quickened form: the TRUE
        // receiver class, read before pushCall (fr/recv dangle after).
        const std::uint32_t recvCls = rt.heap().classOf(recv.ref()).v;
        if (!pushCall(ins, MethodId{cached})) {
          return RunStatus::Threw;
        }
        recordDispatch(mIdx, ins.imm, kind, recvCls, cached);
        B2_NEXT;
      }
      if (cfg.collectStats) {
        ++out.stats.icMisses;
      }
      // AMBIGUITY RESOLUTION A4: a cold quickened cache must still resolve
      // ("full unquickened-style resolve", the contract), but in-place
      // quickening repurposed imm, so the only lawful resolution source is
      // the method pool reached through imm. v0 has no quickener - every
      // quickened stream is hand-written and must carry the cp index in imm.
      // A future quickener writing site ids into imm makes cold misses
      // unresolvable; that changes with global site ids (RFC, contract
      // bump). Until then: imm doubles as the cp index; a non-MethodRef
      // entry is an honest InternalError.
      if (ins.imm >= fr.method->cp.size()) {
        if (!raise(kClsInternalError,
                   "quickened virtual call with cold inline cache (v0)")) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const rbc::Const& ref = fr.method->cp[ins.imm];
      if (ref.kind != rbc::Const::Kind::MethodRef &&
          ref.kind != rbc::Const::Kind::InterfaceMethodRef) {
        if (!raise(kClsInternalError,
                   "quickened virtual call with cold inline cache (v0)")) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const Runtime::Builtin b = rt.lookupBuiltinVirtual(
          rt.heap().classOf(recv.ref()), ref.str2, ref.str3);
      if (b != Runtime::Builtin::None) {
        const std::size_t base =
            std::min<std::size_t>(ins.a, fr.regs.size());
        const std::size_t count =
            std::min<std::size_t>(fr.regs.size() - base, ins.b);
        if (!rt.execBuiltin(
                b, std::span<const Value>(fr.regs.data() + base, count))) {
          if (!raise(kClsInternalError, "builtin arity mismatch")) {
            return RunStatus::Threw;
          }
          B2_NEXT;
        }
        // Builtin sites never fill the IC (same pin as the un-quickened
        // form); the profile counts the site total only.
        recordDispatch(mIdx, ins.imm, kind, kDispatchNoRecvClass,
                       kDispatchNoTarget);
        ++fr.pc;
        B2_NEXT;
      }
      const std::optional<MethodId> target = rt.resolveMethod(ref);
      if (!target) {
        if (!raise(kClsNoSuchMethod, ref.str + "." + ref.str2 + ref.str3)) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const std::uint32_t recvCls = rt.heap().classOf(recv.ref()).v;
      fillSiteIC(mIdx, ins.imm, target->v, recvCls);
      if (!pushCall(ins, *target)) {
        return RunStatus::Threw;
      }
      recordDispatch(mIdx, ins.imm, kind, recvCls, target->v);
      B2_NEXT;
    }

    B2_TARGET(InvokespecialQuick) {
      // v0 pin: imm = MethodId directly. Validated against the program
      // table because rt.method() answers out-of-range ids with a static
      // dummy OUTSIDE program_.methods - a frame built on it would make
      // methodIndex() (pointer subtraction) UB. Hostile ids trap here.
      const Value& recv = fr.regs[ins.a];
      if (recv.isNull()) {
        if (!raiseNpe()) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      if (ins.imm >= program.methods.size()) {
        if (!raise(kClsInternalError, "quickened method id out of range")) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      const std::uint32_t mIdx = methodIndex(fr);
      const std::uint32_t site = fr.pc; // profile site key (Interp.h)
      if (!pushCall(ins, MethodId{ins.imm})) {
        return RunStatus::Threw;
      }
      recordDispatch(mIdx, site, DispatchSiteKind::Special,
                     kDispatchNoRecvClass, ins.imm);
      B2_NEXT;
    }

    B2_TARGET(InvokestaticQuick) {
      if (ins.imm >= program.methods.size()) {
        if (!raise(kClsInternalError, "quickened method id out of range")) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      // The quickened static call still honors the JVMS 5.5 first-use trigger
      // (the class of a program method is the program class; quickened
      // streams carry no MethodRef to name another).
      const InitFlow inited = initClassIfNeeded(programClass);
      if (inited == InitFlow::UnwoundEmpty) {
        return RunStatus::Threw;
      }
      if (inited == InitFlow::PushedClinit) {
        B2_NEXT; // re-execute after <clinit>
      }
      const std::uint32_t mIdx = methodIndex(fr);
      const std::uint32_t site = fr.pc; // profile site key (Interp.h)
      if (!pushCall(ins, MethodId{ins.imm})) {
        return RunStatus::Threw;
      }
      recordDispatch(mIdx, site, DispatchSiteKind::Static,
                     kDispatchNoRecvClass, ins.imm);
      B2_NEXT;
    }

    // ------------------------------------------------------------- returns
    B2_TARGET(Return)
    B2_TARGET(Ireturn)
    B2_TARGET(Lreturn)
    B2_TARGET(Freturn)
    B2_TARGET(Dreturn)
    B2_TARGET(Areturn) {
      // CALL PROTOCOL step 4 (normal return).
      const bool isVoid = ins.opcode() == rbc::Op::Return;
      Frame& callee = frames.back();
      const rbc::Method& calleeMethod = *callee.method;
      const Value retVal =
          isVoid ? Value::bottom() : callee.regs[ins.a];
      releaseMonitorsLIFO(callee); // remaining monitors, best-effort
      // AMBIGUITY RESOLUTION A3: a <clinit> frame's caller pc is NOT
      // advanced - the triggering instruction (new/getstatic/putstatic/
      // invokestatic/ldc-of-Class) re-executes with the class initialized
      // (JVMS 5.5, Interp.h "the original instruction re-executes"). Every
      // other callee advances the caller past the invoke. KNOWN EDGE: an
      // explicit invokestatic of <clinit> from bytecode (JVMS forbids
      // producers from emitting it; the v0 verifier does not reject it)
      // therefore re-executes forever - documented, reported as a verifier
      // gap, never a crash.
      const bool wasClinit = calleeMethod.name == kClinitName &&
                             calleeMethod.descriptor == kClinitDesc;
      frames.pop_back();
      if (frames.empty()) {
        // Entry frame returned: void methods report Bottom (RunStatus pin).
        out.result = retVal;
        return RunStatus::Returned;
      }
      Frame& caller = frames.back();
      if (!isVoid &&
          rbc::parseReturn(calleeMethod.descriptor) != rbc::RType::Bottom) {
        // The invoke sits AT the caller's pc (never advanced during the
        // call); its dst receives the value iff the descriptor is non-void.
        const rbc::Ins& call = caller.method->code[caller.pc];
        if (call.dst < caller.regs.size()) { // defensive (verified streams)
          caller.regs[call.dst] = retVal;
        }
      }
      if (!wasClinit) {
        caller.pc = caller.pc + 1;
      }
      B2_NEXT;
    }

    // ----------------------------------------------------------- exceptions
    B2_TARGET(Athrow) {
      const Value& v = fr.regs[ins.a];
      if (v.isNull()) {
        // JLS 14.18: athrow null raises NPE.
        if (!raiseNpe()) {
          return RunStatus::Threw;
        }
        B2_NEXT;
      }
      if (!throwException(v.ref())) {
        return RunStatus::Threw;
      }
      B2_NEXT; // caught: fr.pc is the handler entry
    }

    // --------------------------------------------------- deopt/tiering hooks
    B2_TARGET(GuardNonNull)
      // T0 holds no speculative state, so the guard's failure condition
      // cannot arise here: a NO-OP. Not a path terminator, so pc + 1 exists
      // by verification (rbc_spec.md SS5.4).
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(GuardClass)
      // Same pin as guard_non_null (v0: verifier-only; no tier emits it).
      ++fr.pc;
      B2_NEXT;

    B2_TARGET(DeoptTrap)
      // A path terminator whose resume point lives in deopt metadata no
      // tier provides yet. Honest refusal (never silent): deopting TO T0 is
      // the fallback, but deopt_trap IN T0 without metadata is a contract
      // violation.
      if (!raise(kClsInternalError, kMsgDeoptTrap)) {
        return RunStatus::Threw;
      }
      B2_NEXT;
#if !B2_INTERP_COMPUTED_GOTO

    // ------------------------------------------------- unreachable guard ---
    // No default clause: -Wswitch must flag any opcode added to Op without
    // a handler here (the switch is the exhaustiveness proof). _Count is
    // the enum's end pseudo-value; the verifier rejects op >= _Count, so
    // reaching this case means corrupt stream state - total, never silent.
    B2_TARGET(_Count)
      if (!raise(kClsInternalError, "unreachable opcode dispatched")) {
        return RunStatus::Threw;
      }
      B2_NEXT;
#endif // !B2_INTERP_COMPUTED_GOTO (the _Count exhaustiveness guard)
#if !B2_INTERP_COMPUTED_GOTO
    } // switch (exhaustive over Op; no default: -Wswitch is the proof)
  } // for (;;)
#endif // !B2_INTERP_COMPUTED_GOTO
#undef fr
#if B2_INTERP_COMPUTED_GOTO
  return RunStatus::Threw; // unreachable: every handler dispatches or returns
#endif
} // dispatchLoop

} // namespace

// ============================================================================
// Safepoint-request statics (Rule 111; see safepointFlag() above).
// ============================================================================

bool Interpreter::safepointRequested() noexcept {
  return safepointFlag().load(std::memory_order_relaxed) != 0;
}

void Interpreter::requestSafepoint() noexcept {
  safepointFlag().store(1, std::memory_order_relaxed);
}

void Interpreter::clearSafepointRequest() noexcept {
  safepointFlag().store(0, std::memory_order_relaxed);
}

// ============================================================================
// Construction.
// ============================================================================

Interpreter::Interpreter(const rbc::Program& program,
                         const InterpConfig& config)
    : program_(program), cfg_(config), rt_(program) {
  // siteICs_ stays unsized until the first IC miss (Rule 7: lazily sized per
  // method on first miss only). frames_ is empty outside run()/resume().
}

// ============================================================================
// run() - the normal entry point (THE DISPATCH LOOP ALGORITHM step 2).
// ============================================================================

RunResult Interpreter::run(std::string_view name,
                           std::string_view descriptor,
                           std::span<const Value> args) {
  RunResult result;
  result_ = &result;

  // One shared exit tail: every path fills the allocation census (there is
  // no GC in v0, so objectCount() is the cumulative allocation total) and
  // clears the active-result pointer.
  const auto finish = [this](RunResult&& r) -> RunResult {
    r.stats.allocations = rt_.heap().objectCount();
    result_ = nullptr;
    return r;
  };

  // --- HARD GATE: verify every method (verifier before execution) -----------
  // The interpreter never executes unverified RBC (Interp.h; rbc_spec.md
  // SS5 ordering law). Any diagnostic fails the whole program.
  for (const rbc::Method& m : program_.methods) {
    const rbc::VerifyResult v = rbc::verify(m);
    if (!v.ok) {
      result.status = RunStatus::VerifyFailed;
      result.verifyDiags = std::move(v.diags);
      return finish(std::move(result));
    }
  }

  // --- entry resolution -------------------------------------------------------
  const rbc::Method* entry = program_.find(name, descriptor);
  if (entry == nullptr) {
    // NoSuchMethodError with the message "<name><descriptor>" (e.g.
    // "main()V"). No frames were pushed; the exception is the run result.
    result.status = RunStatus::Threw;
    result.exception =
        rt_.makeException(kClsNoSuchMethod, std::string(name) +
                                                std::string(descriptor));
    if (cfg_.collectStats) {
      ++result.stats.exceptions;
    }
    return finish(std::move(result));
  }

  // --- entry frame -------------------------------------------------------------
  // Static entries take the parameters in order; instance entries take the
  // receiver first. The caller must supply at least paramCount (+ receiver)
  // values; fewer is caller misuse and becomes a Java-visible InternalError
  // (total, never a crash - the run() precondition is not a C++ contract).
  const std::uint32_t params = rbc::paramCount(descriptor);
  const std::size_t required =
      params + (entry->isStatic() ? 0u : 1u);
  if (args.size() < required) {
    result.status = RunStatus::Threw;
    result.exception = rt_.makeException(
        kClsInternalError, "entry arguments insufficient for " +
                               std::string(name) + std::string(descriptor));
    if (cfg_.collectStats) {
      ++result.stats.exceptions;
    }
    return finish(std::move(result));
  }

  // run()/resume() re-callable contract: reset the frame stack; the Runtime
  // (heap, statics, inline caches) persists - a JVM that stays up.
  frames_.clear();

  Frame entryFrame;
  entryFrame.method = entry;
  entryFrame.pc = 0;
  entryFrame.locals.assign(args.begin(), args.end());
  if (entryFrame.locals.size() < entry->numLocals) {
    entryFrame.locals.resize(entry->numLocals, Value::bottom());
  }
  entryFrame.regs.assign(entry->numRegs, Value::bottom());

  // --- synchronized-method entry (JVMS 8.4.3.6) ---------------------------------
  // The receiver for instance entries, the class object for static ones.
  // A null receiver traps BEFORE the method is entered (Java raises at the
  // call site, so the callee's own handlers must not see it): the exception
  // becomes the run result directly, without handler search.
  if (entry->isSynchronized()) {
    ObjRef lock{};
    bool entryTrap = false;
    std::string_view trapCls{};
    if (entry->isStatic()) {
      lock = rt_.classObject(rt_.classId(program_.className));
    } else if (args.empty() || args.front().isNull()) {
      entryTrap = true;
      trapCls = kClsNullPointer;
    } else {
      lock = args.front().ref();
    }
    if (!entryTrap && rt_.heap().monitorenter(lock) != Heap::MonFail::None) {
      entryTrap = true; // NotOwner is unreachable on a fresh monitor
      trapCls = kClsIllegalMonitor;
    }
    if (entryTrap) {
      result.status = RunStatus::Threw;
      result.exception = rt_.makeException(trapCls, "");
      if (cfg_.collectStats) {
        ++result.stats.exceptions;
      }
      return finish(std::move(result));
    }
    // First (and initially only) monitor of the frame; released by the
    // return/unwind paths like any monitorenter.
    entryFrame.monitors.push_back(lock);
  }

  frames_.push_back(std::move(entryFrame));
  rt_.bumpInvocations(MethodId{
      static_cast<std::uint32_t>(entry - program_.methods.data())});

  result.status =
      dispatchLoop(program_, cfg_, rt_, frames_, siteICs_, dispatchProfiles_,
                   result);
  return finish(std::move(result));
}

// ============================================================================
// resume() - the deopt entry point (Rule 4; docs/deopt_backend.md Part A).
// ============================================================================

RunResult Interpreter::resume(Frame frame) {
  RunResult result;
  result_ = &result;

  const auto finish = [this](RunResult&& r) -> RunResult {
    r.stats.allocations = rt_.heap().objectCount();
    result_ = nullptr;
    return r;
  };

  // Same HARD GATE as run(): never execute unverified RBC.
  for (const rbc::Method& m : program_.methods) {
    const rbc::VerifyResult v = rbc::verify(m);
    if (!v.ok) {
      result.status = RunStatus::VerifyFailed;
      result.verifyDiags = std::move(v.diags);
      return finish(std::move(result));
    }
  }

  // Defensive validation of the externally reconstructed frame (the deopt
  // contract says callers build it from the T0 contract; this keeps hostile
  // frames from reaching pointer arithmetic on program_.methods).
  const bool methodInRange =
      frame.method != nullptr &&
      frame.method >= program_.methods.data() &&
      frame.method < program_.methods.data() + program_.methods.size();
  if (!methodInRange || frame.pc >= frame.method->code.size()) {
    result.status = RunStatus::Threw;
    result.exception = rt_.makeException(
        kClsInternalError, "resume frame does not match the program");
    if (cfg_.collectStats) {
      ++result.stats.exceptions;
    }
    return finish(std::move(result));
  }

  // Reset the frame stack; Runtime state (heap/statics/ICs) persists. The
  // caller materialized the frame from the T0 contract: synchronized-method
  // entry state is the caller's responsibility - the frame's monitors list
  // IS the held-monitor state (Interp.h).
  frames_.clear();

  // Defensive padding (total, never UB): short locals/regs vectors grow
  // Bottom-filled; overlong ones are left alone (extra slots are inert).
  if (frame.locals.size() < frame.method->numLocals) {
    frame.locals.resize(frame.method->numLocals, Value::bottom());
  }
  if (frame.regs.size() < frame.method->numRegs) {
    frame.regs.resize(frame.method->numRegs, Value::bottom());
  }

  frames_.push_back(std::move(frame));

  // No invocation bump: the resumed frame's invocation was counted by
  // whoever started it; resume() is mid-method (deopt), not a fresh entry.

  // dispatchLoop consumes a pendingException (exception deopt, Part A SS2.3)
  // by starting INSIDE the exception algorithm at the given pc.
  result.status =
      dispatchLoop(program_, cfg_, rt_, frames_, siteICs_, dispatchProfiles_,
                   result);
  return finish(std::move(result));
}

} // namespace b2::interp
