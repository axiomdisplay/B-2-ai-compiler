// B-2 Interpreter - T0 frame state serialization (Task BE-2).
//
// WHY THIS FILE EXISTS:
// The T0 frame is the deoptimization unit of the whole system (Rule 4, Rule
// 75, Rule 96; Amendment B.1): every compiled tier must reconstruct EXACTLY
// this state at an RBC instruction boundary, and the interpreter must resume
// from it (docs/deopt_backend.md Part A SS1/SS3). dumpFrame/dumpFrames
// produce the PINNED, byte-deterministic text fixture (Frame.h state-dump
// block; docs/interp_contract.md) that every tier's deopt tests consume -
// golden state is a testing responsibility of each charter.
//
// FORMAT (v1 pin, Frame.h):
//   frame <depth> method=<name><descriptor> pc=<pc>
//     local l0:<kind>=<payload> ...        (one line; ALWAYS emitted)
//     reg r0:<kind>=<payload> ...          (one line; ALWAYS emitted)
//     monitors=<n>[,<objid>...]
// Kind spellings: int/long/float/double/null/ref via rbc::typeName, EXCEPT
// Bottom which the format pins as "bot" (typeName says "bottom" - documented
// divergence between the two frozen spellings; the dump spelling wins here).
// Payloads: Int/Long decimal; Float/Double javaFloatString/javaDoubleString;
// Null "null"; Bottom "bot"; Ref "internalclass@<id>" with " str=<payload>"
// appended for String instances (RAW UTF-8, no quotes, no escaping - keeps
// fixtures grep-able and byte-stable, Rule 124).
//
// FROZEN-HEADER GAP WORKAROUND (reported to the integrator):
// dumpFrame/dumpFrames take `const Runtime&` (Frame.h) but Runtime::heap()
// has no const overload, and the pinned format needs READ-ONLY heap access
// (String payloads). We const_cast and call only const Heap members
// (peek/isString/stringOf): well-defined even for a genuinely const Runtime
// because nothing is written through the cast. v1 fix: add
// `const Heap& heap() const noexcept` to Runtime.h.

#include "b2/interp/Frame.h"

#include <string>
#include <string_view>

#include "b2/interp/Runtime.h"

namespace b2::interp {
namespace {

// See the file header: the const Runtime& -> const Heap& bridge.
[[nodiscard]] const Heap& readOnlyHeap(const Runtime& rt) noexcept {
  return const_cast<Runtime&>(rt).heap();
}

// The pinned dump spelling of a Value tag: rbc::typeName() except Bottom,
// which the state-dump format spells "bot" (Frame.h) while Type.h spells
// "bottom". The dump format is the normative one here.
[[nodiscard]] std::string_view kindSpelling(rbc::RType t) noexcept {
  if (t == rbc::RType::Bottom) {
    return "bot";
  }
  return rbc::typeName(t);
}

// Appends one slot payload in the pinned format (see the file header).
void appendPayload(std::string& out, const Value& v, const Runtime& rt) {
  switch (v.type) {
  case rbc::RType::Int:
    out += std::to_string(v.as.i);
    break;
  case rbc::RType::Long:
    out += std::to_string(v.as.l);
    break;
  case rbc::RType::Float:
    out += javaFloatString(v.as.f);
    break;
  case rbc::RType::Double:
    out += javaDoubleString(v.as.d);
    break;
  case rbc::RType::Null:
    out += "null";
    break;
  case rbc::RType::Bottom:
    out += "bot";
    break;
  case rbc::RType::Ref: {
    const ObjRef r = v.ref();
    // Internal class name ("" for dead ids - defensive; the dispatch core
    // never dumps dead refs), then the STABLE object id (Rule 15).
    out += rt.classNameOf(r);
    out.push_back('@');
    out += std::to_string(r.id);
    // String instances append the raw payload: no quotes, no escapes (pin).
    const Heap& heap = readOnlyHeap(rt);
    if (heap.isString(r)) {
      out += " str=";
      out += heap.stringOf(r);
    }
    break;
  }
  }
}

// One "local"/"reg" line: the label word is ALWAYS emitted (pin), even with
// zero slots ("  local" / "  reg" alone), then every slot in order,
// space-separated as " <l|l><i>:<kind>=<payload>".
template <typename Slots>
void appendSlotLine(std::string& out, std::string_view label, char slotKind,
                     const Slots& slots, const Runtime& rt) {
  out += "  ";
  out += label;
  for (std::size_t i = 0; i < slots.size(); ++i) {
    out.push_back(' ');
    out.push_back(slotKind);
    out += std::to_string(i);
    out.push_back(':');
    out += kindSpelling(slots[i].type);
    out.push_back('=');
    appendPayload(out, slots[i], rt);
  }
  out.push_back('\n');
}

} // namespace

void dumpFrame(const Frame& f, std::uint32_t depth, const Runtime& rt,
               std::string& out) {
  // Line 1: header. A methodless frame (defensive - the dispatch core always
  // sets one) prints the "<none>" placeholder so the dump stays total and
  // deterministic instead of crashing.
  out += "frame ";
  out += std::to_string(depth);
  out += " method=";
  out += (f.method != nullptr) ? f.method->name : std::string("<none>");
  out += (f.method != nullptr) ? f.method->descriptor : std::string();
  out += " pc=";
  out += std::to_string(f.pc);
  out.push_back('\n');

  // Lines 2-3: locals (params first) then registers (Bottom at entry).
  appendSlotLine(out, "local", 'l', f.locals, rt);
  appendSlotLine(out, "reg", 'r', f.regs, rt);

  // Line 4: monitor record, most-recent-first. Frame.h pins the vector as
  // LIFO order (entered-most-recently first), so front-to-back IS
  // most-recent-first; unwinding releases them in exactly this order.
  out += "  monitors=";
  out += std::to_string(f.monitors.size());
  for (const ObjRef m : f.monitors) {
    out.push_back(',');
    out += std::to_string(m.id);
  }
  out.push_back('\n');
}

void dumpFrames(std::span<const Frame> frames, const Runtime& rt,
                std::string& out) {
  // Innermost (back()) first at depth 0, walking to the outermost (front())
  // at depth size-1; exactly ONE blank line BETWEEN frames, none trailing.
  // Appends to `out` so a caller can prefix tool headers.
  for (std::size_t i = frames.size(); i-- > 0;) {
    if (i + 1 != frames.size()) {
      out.push_back('\n');
    }
    dumpFrame(frames[i], static_cast<std::uint32_t>(frames.size() - 1 - i), rt,
              out);
  }
}

} // namespace b2::interp
