#pragma once
// B-2 Interpreter - the canonical T0 frame and its observable state format.
//
// WHY THIS FILE EXISTS:
// The T0 frame is the deoptimization unit of the whole system (Rule 4, Rule
// 75, Rule 96; Amendment B.1). Every compiled tier must reconstruct EXACTLY
// this state at an RBC instruction boundary, and the interpreter must be able
// to RESUME from exactly this state (docs/deopt_backend.md Part A SS1/SS3).
// Making the frame a plain, dumpable, copyable value (not a clever linked
// runtime structure) is what keeps "deopt = memcpy into a new T0 frame" true
// (rbc_spec.md SS1.4) and gives every tier a machine-checkable fixture
// format for their deopt tests (charter testing responsibility).
//
// KEY INVARIANTS:
// - locals and regs hold one Value per slot (numLocals / numRegs from the
//   Method). long/double occupy ONE slot (RBC divergence from the JVM,
//   rbc_spec.md SS1.2). At entry, locals hold the parameters (receiver at
//   l0 for instance methods) and every register is Bottom.
// - pc is an RBC instruction index (rbc_spec.md SS2.2), never a byte offset.
// - The monitor record lists the objects this frame entered (monitorenter
//   plus a synchronized method entry) most-recent-first; unwinding releases
//   them in that order (JVMS monitorexit-on-throw).
// - pendingException is set only between "trap occurred" and "handler entry
//   or unwind step"; at any safepoint poll it is always unset (a poll never
//   runs with an exception in flight).
//
// STATE DUMP FORMAT (v1, pinned; docs/interp_contract.md):
//   frame <depth> method=<name><descriptor> pc=<pc>
//     local l0:<kind>=<payload> ...        (one line, slots in order)
//     reg r0:<kind>=<payload> ...          (one line, slots in order)
//     monitors=<n>[,<objid>...]
// Payloads: Int decimal; Long decimal; Float/Double javaFloatString/
// javaDoubleString; Null the literal null; Ref "class@id"; a String Ref
// additionally appends "str=<payload>". Empty slots (Bottom) print as
// "bot". The dump is byte-deterministic (Rule 124) and is the golden format
// for deopt fixtures.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "b2/interp/Value.h"
#include "b2/rbc/Rbc.h"

namespace b2::interp {

struct Frame {
  const rbc::Method* method = nullptr; // owning method (stable: program is
                                       // const during a run)
  std::uint32_t pc = 0;                // RBC instruction index
  std::vector<Value> locals;           // numLocals slots, params first
  std::vector<Value> regs;             // numRegs slots, Bottom at entry
  std::vector<ObjRef> monitors;        // entered by this frame, LIFO order
  ObjRef pendingException{};           // in-flight exception, else invalid

  // Call linkage: the pc of the invoke that pushed this frame. Used by
  // exception unwinding to re-check handler coverage in the CALLER at the
  // call site (the call site is the throwing location, JVMS semantics).
  std::uint32_t callerPc = 0;
};

// Serializes one frame in the pinned state-dump format (above). `classes`
// supplies class names for Ref payloads; `depth` is the frame's stack depth
// (0 = innermost). Deterministic; appends to out.
void dumpFrame(const Frame& f, std::uint32_t depth, const class Runtime& rt,
               std::string& out);

// Serializes a whole frame stack, innermost first, one blank line between
// frames. This is the T0 state fixture generator (deopt tests of every tier
// consume its output; charter testing responsibilities).
void dumpFrames(std::span<const Frame> frames, const class Runtime& rt,
                std::string& out);

} // namespace b2::interp
