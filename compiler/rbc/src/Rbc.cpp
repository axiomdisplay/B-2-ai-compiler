// B-2 RBC - constant-pool kind names, program lookup, descriptor parsing.
//
// WHY THIS FILE EXISTS:
// Rbc.h freezes the in-memory structures plus the four JVM-descriptor
// helpers every consumer needs (verifier entry states, builder call typing,
// text tools, diagnostics). Descriptor strings are UNTRUSTED input — they
// arrive from source lowering today and from classfiles tomorrow — so all
// parsing here is defensive and total: malformed input returns
// false/Bottom/"?", never a crash, never an exception, never an unbounded
// loop, never an unbounded recursion.

#include <cstddef>

#include "b2/rbc/Rbc.h"

namespace b2::rbc {
namespace {

// Result of parsing one descriptor type starting at `i`.
struct TypeResult {
  RType type;       // parsed type; Bottom when !ok, or void when ok
  bool ok;          // false = malformed at/after `i`
  std::size_t next; // index one past the consumed text (valid only when ok)
};

// Parses one descriptor type at d[i].
//
// WHY two flags instead of separate parsers: the legal alphabet depends on
// the position the type occupies.
// - `voidOk`: 'V' is legal only as a method RETURN type (JVMS 4.3.3); in
//   parameter position it is malformed.
// - `subIntOk`: 'Z','B','C','S' are legal only as ARRAY ELEMENT types.
//   boolean[]/byte[]/char[]/short[] are real reference types whose
//   descriptors ("[Z", "[B", ...) must parse; as BARE parameters they are
//   malformed because B-2 source lowering folds those primitive values into
//   Int (only 'I' ever appears for them at the top level).
//
// WHY iterative brackets: a hostile descriptor of N million '[' characters
// must not overflow the stack. The '[' case consumes ALL brackets in a loop
// and then performs a single non-array element parse, so recursion depth is
// bounded at 2 regardless of input.
TypeResult parseTypeAt(std::string_view d, std::size_t i, bool voidOk,
                       bool subIntOk) noexcept {
  if (i >= d.size()) {
    return {RType::Bottom, false, i};
  }
  switch (d[i]) {
  case 'V':
    if (voidOk) {
      return {RType::Bottom, true, i + 1};
    }
    return {RType::Bottom, false, i};
  case 'I':
    return {RType::Int, true, i + 1};
  case 'J':
    return {RType::Long, true, i + 1};
  case 'F':
    return {RType::Float, true, i + 1};
  case 'D':
    return {RType::Double, true, i + 1};
  case 'L': {
    // L <internal-name> ; — the name must be non-empty and must not contain
    // characters that would break descriptor structure.
    std::size_t j = i + 1;
    while (j < d.size() && d[j] != ';') {
      const char c = d[j];
      if (c == '(' || c == ')' || c == '[') {
        return {RType::Bottom, false, i};
      }
      ++j;
    }
    if (j >= d.size()) {
      return {RType::Bottom, false, i}; // truncated: "Lfoo"
    }
    if (j == i + 1) {
      return {RType::Bottom, false, i}; // empty name: "L;"
    }
    return {RType::Ref, true, j + 1};
  }
  case '[': {
    std::size_t j = i;
    while (j < d.size() && d[j] == '[') {
      ++j;
    }
    // Any depth of array is Ref; the element may be a sub-int primitive
    // (subIntOk=true) but never void ("[V" is malformed).
    const TypeResult elem = parseTypeAt(d, j, false, true);
    if (!elem.ok) {
      return {RType::Bottom, false, i};
    }
    return {RType::Ref, true, elem.next};
  }
  case 'Z':
  case 'B':
  case 'C':
  case 'S':
    if (subIntOk) {
      // Only reachable as an array element; the enclosing '[' case folds
      // the result to Ref and ignores this type.
      return {RType::Int, true, i + 1};
    }
    return {RType::Bottom, false, i};
  default:
    return {RType::Bottom, false, i};
  }
}

// Walks the parameter section. `i` must point one past '('. On success
// returns true with `i` positioned ON the ')' and `count` = number of
// parameters; on failure returns false (truncated input or a malformed
// type). Allocation-free so the failure path never allocates.
bool walkParams(std::string_view d, std::size_t& i,
                std::size_t& count) noexcept {
  count = 0;
  while (i < d.size() && d[i] != ')') {
    const TypeResult t = parseTypeAt(d, i, false, false);
    if (!t.ok) {
      return false;
    }
    ++count;
    i = t.next;
  }
  return i < d.size(); // false => input ended before ')'
}

} // namespace

const char* Const::kindName() const noexcept {
  // These strings are the text-format keywords (RbcText.h grammar):
  // the text parser of another agent looks them up verbatim.
  switch (kind) {
  case Kind::Int32:
    return "int";
  case Kind::Int64:
    return "long";
  case Kind::Float:
    return "float";
  case Kind::Double:
    return "double";
  case Kind::Utf8:
    return "utf8";
  case Kind::String:
    return "string";
  case Kind::Class:
    return "class";
  case Kind::NameType:
    return "nametype";
  case Kind::FieldRef:
    return "field";
  case Kind::MethodRef:
    return "method";
  case Kind::InterfaceMethodRef:
    return "imethod";
  case Kind::MethodType:
    return "methodtype";
  case Kind::MethodHandle:
    return "methodhandle";
  case Kind::InvokeDynamic:
    return "indy";
  case Kind::SwitchTable:
    return "switch";
  }
  return "?"; // corrupted kind byte: report, never crash
}

const Method* Program::find(std::string_view name,
                            std::string_view descriptor) const noexcept {
  // WHY linear scan: v0 programs are small; a hash index would add
  // determinism risk (iteration order, Rule 124) for no measurable win.
  for (const Method& m : methods) {
    if (m.name == name && m.descriptor == descriptor) {
      return &m;
    }
  }
  return nullptr;
}

bool parseParams(std::string_view descriptor,
                 std::vector<RType>& out) noexcept {
  // Contract (Rbc.h + ME-B task): parameter types are APPENDED to `out` on
  // success; malformed input returns false with `out` cleared.
  //
  // WHY validate-then-collect: the validation pass (walkParams) allocates
  // nothing, so a malformed untrusted descriptor never allocates before
  // failing; the collecting pass then cannot fail mid-way and leave partial
  // state. The only allocation on the success path is bounded by the
  // descriptor length (one RType byte per input character) — OOM follows
  // the codebase-wide terminate convention (no exception handling in B-2).
  if (descriptor.empty() || descriptor.front() != '(') {
    out.clear();
    return false;
  }
  std::size_t i = 1;
  std::size_t count = 0;
  if (!walkParams(descriptor, i, count)) {
    out.clear();
    return false;
  }
  i = 1; // collecting pass: the same walk, already proven to succeed
  while (i < descriptor.size() && descriptor[i] != ')') {
    const TypeResult t = parseTypeAt(descriptor, i, false, false);
    if (!t.ok) {
      // Unreachable after validation; kept so a logic regression can never
      // spin or read out of bounds.
      out.clear();
      return false;
    }
    out.push_back(t.type);
    i = t.next;
  }
  return true;
}

RType parseReturn(std::string_view descriptor) noexcept {
  // CONTRACT (documented here per the ME-B task because the frozen
  // signature cannot express it): RType::Bottom means VOID when the
  // descriptor is well-formed, and ALSO means "malformed" when it is not —
  // the two are indistinguishable through this API. Callers that must
  // distinguish validate the whole descriptor first: parseParams covers the
  // parameter section, and this function re-walks the parameters itself so
  // a malformed parameter list also yields Bottom (defensive: the return
  // type of a broken descriptor is meaningless). The return type must
  // consume the ENTIRE remainder — trailing garbage is malformed.
  if (descriptor.empty() || descriptor.front() != '(') {
    return RType::Bottom;
  }
  std::size_t i = 1;
  std::size_t count = 0;
  if (!walkParams(descriptor, i, count)) {
    return RType::Bottom;
  }
  // i is on ')'; the return type starts at i + 1 and must be the whole rest.
  const TypeResult r =
      parseTypeAt(descriptor, i + 1, /*voidOk=*/true, /*subIntOk=*/false);
  if (!r.ok || r.next != descriptor.size()) {
    return RType::Bottom;
  }
  return r.type; // Bottom iff 'V' (void)
}

std::uint32_t paramCount(std::string_view descriptor) noexcept {
  // RBC gives every parameter ONE slot regardless of category (longs and
  // doubles do not take two slots — Type.h); the JVM's two-slot rule does
  // not apply. Malformed input returns 0.
  if (descriptor.empty() || descriptor.front() != '(') {
    return 0;
  }
  std::size_t i = 1;
  std::size_t count = 0;
  if (!walkParams(descriptor, i, count)) {
    return 0;
  }
  return static_cast<std::uint32_t>(count);
}

std::string describeType(std::string_view descriptorType) {
  // Human-readable form of ONE descriptor type. Primitives map to their
  // names; refs and arrays keep the full descriptor after a "ref " prefix
  // so diagnostics stay unambiguous ("ref Ljava/lang/String;", "ref [I").
  // Bare 'Z'/'B'/'C'/'S' are not described (they fold into Int in B-2);
  // as array elements they are covered by the "ref [Z" form. Malformed
  // input (including the empty string) returns "?".
  if (descriptorType.empty()) {
    return "?";
  }
  const TypeResult t =
      parseTypeAt(descriptorType, 0, /*voidOk=*/true, /*subIntOk=*/false);
  if (!t.ok || t.next != descriptorType.size()) {
    return "?";
  }
  if (t.type == RType::Bottom) {
    return "void";
  }
  if (t.type == RType::Ref) {
    return "ref " + std::string(descriptorType);
  }
  return typeName(t.type);
}

} // namespace b2::rbc
