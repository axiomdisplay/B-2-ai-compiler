// B-2 Interpreter - the v0 reference runtime services (Task BE-2).
//
// WHY THIS FILE EXISTS:
// The frozen contract is include/b2/interp/Runtime.h: the dispatch core
// (Interp.cpp) must not know how classes, fields, methods, statics, builtins,
// monitors, traps, output, or profiling are implemented. This file fronts the
// reference object model (Heap.cpp) plus the class registry, lazy field
// resolution, static storage, the println/print builtin seam, trap-object
// construction, and saturating profile counters. When the real runtime/
// arrives it replaces this implementation behind the same header shape and
// the dispatch core does not change (charter: services "used, never
// modified").
//
// KEY DECISIONS AND FROZEN-HEADER GAPS (full stories at each site):
// - classByName_ is a DUAL-ROLE registry (classes AND the field-name index).
//   The frozen header has no (class,name)->FieldId storage, but
//   resolveField MUST be idempotent (getfield+putfield of one field across
//   two IC misses must agree or statics/instance slots split). Field keys
//   are "\x01<class>\x01<name>" and values are parity-encoded
//   (class -> 2*id, field -> 2*id+1). See classId()/resolveField().
// - ClassId{0} is BOTH Object's id and ClassInfo::superclass's "unset"
//   value. v0 pins the Java-correct reading: v==0 IS Object and Object's
//   own default superclass self-loops, terminating the chain walk. See
//   isAssignableFrom().
// - needsInit() is gated on the class being the PROGRAM class: only the
//   program's class can own RBC methods in the v0 one-class world. The
//   letter of the BE-2 task pin ("search program_.methods") without that
//   guard would re-run the program's <clinit> under a foreign class's
//   initialized flag (double execution). The frozen header comment ("the
//   class has an RBC <clinit>()V") is normative and matches the guard.
// - execBuiltin() formats by RUNTIME VALUE TAG because the frozen signature
//   cannot carry the call-site descriptor; (Z)/(C) print as decimal ints -
//   a documented v0 divergence from Java. See execBuiltin().
// - findClass() reports "not found" as the one-past-the-end sentinel;
//   classInfo() maps out-of-range ids to a static empty ClassInfo (empty
//   name == absent). See findClass().
//
// CROSS-REFERENCES:
// - docs/rbc_spec.md SS3.13-SS3.15 (arrays, objects, calls), SS4 (types)
// - docs/laws.md Rules 15, 16, 72, 74, 114; docs/cpp26_standards.md Part A

#include "b2/interp/Runtime.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <utility>

namespace b2::interp {

// ============================================================================
// Java Double.toString / Float.toString formatting (JDK algorithm).
//
// The contract (Runtime.h, pinned with an exact reference table by BE-2):
//   NaN -> "NaN"; +inf -> "Infinity"; -inf -> "-Infinity"; zero keeps its
//   sign ("0.0" / "-0.0"); otherwise the SHORTEST decimal that round-trips,
//   plain notation iff 10^-3 <= |v| < 10^7 (exponent E of the first digit
//   within -3 <= E < 7), else scientific "d.dddE<x>": one digit before the
//   point, always >= 1 fraction digit, no '+' and no leading zeros in the
//   exponent.
//
// WHY std::to_chars(..., chars_format::scientific): it renders the shortest
// round-tripping digits with the exponent already equal to the power of ten
// of the first digit - exactly the JDK's digit selection (libstdc++ ships
// Ry u). Only the LAYOUT differs ("1e+07" -> "1.0E7"), which is what the
// code below does. KNOWN DIVERGENCE (documented, pinned): a few subnormal
// extremes pick different-but-round-tripping digit strings than the JDK's
// renderer (e.g. double MIN_VALUE: to_chars "5e-324" vs JDK "4.9E-324").
// The BE-2 reference table pins to_chars as the digit source and every
// listed value matches exactly (verified by scripts/be2_smoke.cpp).
// ============================================================================
namespace {

// Rendered-shortest workspace: sign + mantissa digits (no '.') + exponent of
// the first digit. Parsing is total: any shape mismatch (impossible for
// to_chars output) yields ok == false and the caller degrades to "NaN"
// instead of crashing (verifier-totality discipline, Rule 47).
struct ShortestDecimal {
  bool negative = false;
  char digits[32];      // mantissa digits; shortest is <= 17 for double
  std::size_t numDigits = 0;
  int exponent = 0;     // power of ten of digits[0]
  bool ok = false;
};

template <typename Float>
[[nodiscard]] ShortestDecimal parseShortestScientific(const char* buf,
                                                      std::size_t len) {
  ShortestDecimal out;
  const std::string_view s(buf, len);
  std::size_t i = 0;
  if (!s.empty() && s.front() == '-') {
    out.negative = true;
    i = 1;
  }
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c == 'e' || c == 'E') {
      ++i; // consume 'e'
      bool expNeg = false;
      if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        expNeg = (s[i] == '-');
        ++i;
      }
      for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') {
          return out; // shape mismatch (unreachable)
        }
        out.exponent = out.exponent * 10 + (s[i] - '0');
      }
      if (expNeg) {
        out.exponent = -out.exponent;
      }
      out.ok = out.numDigits > 0; // at least one mantissa digit was seen
      return out;
    }
    if (c >= '0' && c <= '9') {
      if (out.numDigits >= sizeof out.digits) {
        return out; // shape mismatch (unreachable: shortest <= 17 digits)
      }
      out.digits[out.numDigits++] = c;
    } else if (c != '.') {
      return out; // shape mismatch (unreachable)
    }
  }
  return out; // no exponent found (unreachable for scientific format)
}

template <typename Float>
[[nodiscard]] std::string javaFloatStringImpl(Float v) {
  // Specials FIRST: to_chars would spell them "nan"/"inf"/"-0".
  if (std::isnan(v)) {
    return "NaN";
  }
  if (std::isinf(v)) {
    return v < 0 ? "-Infinity" : "Infinity";
  }
  if (v == 0) {
    // signbit BEFORE anything else: -0.0 == 0 numerically but Java prints
    // the sign, and to_chars shortest would lose it into "-0e+00".
    return std::signbit(v) ? "-0.0" : "0.0";
  }

  char buf[48]; // provably sufficient: <= 17 digits + '.' + 'e' + sign + exp
  const std::to_chars_result res =
      std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
  if (res.ec != std::errc{}) {
    return "NaN"; // unreachable (buffer bound above); total, never crash
  }
  const ShortestDecimal sd =
      parseShortestScientific<Float>(buf, static_cast<std::size_t>(res.ptr - buf));
  if (!sd.ok) {
    return "NaN"; // unreachable defensive path
  }
  const std::string_view d(sd.digits, sd.numDigits);

  std::string out;
  out.reserve(d.size() + 8);
  if (sd.negative) {
    out.push_back('-');
  }
  if (sd.exponent >= -3 && sd.exponent < 7) {
    // Plain notation (JLS: 10^-3 <= |v| < 10^7).
    if (sd.exponent >= 0) {
      const std::size_t intDigits =
          static_cast<std::size_t>(sd.exponent) + 1; // digits before '.'
      if (d.size() <= intDigits) {
        out.append(d);
        out.append(intDigits - d.size(), '0'); // pad exhausted digits
        out.append(".0");                       // >= 1 fraction digit ("100.0")
      } else {
        out.append(d.substr(0, intDigits));
        out.push_back('.');
        out.append(d.substr(intDigits));
      }
    } else { // -3 <= E <= -1: "0." + (-E-1 zeros) + digits
      out.append("0.");
      out.append(static_cast<std::size_t>(-sd.exponent) - 1, '0');
      out.append(d);
    }
  } else {
    // Scientific: first digit, '.', rest (or "0" when single-digit), 'E',
    // decimal exponent with NO '+' and NO leading zeros ("1.0E7", "1.0E-4").
    out.push_back(d.front());
    out.push_back('.');
    if (d.size() > 1) {
      out.append(d.substr(1));
    } else {
      out.push_back('0');
    }
    out.push_back('E');
    out += std::to_string(sd.exponent);
  }
  return out;
}

} // namespace

std::string javaDoubleString(double v) { return javaFloatStringImpl(v); }

std::string javaFloatString(float v) { return javaFloatStringImpl(v); }

// ============================================================================
// File-local helpers: registry key encoding, field descriptors, builtins.
// ============================================================================
namespace {

// --- the dual-role classByName_ registry -----------------------------------
//
// WHY this exists (frozen-header gap, see the file header): the frozen
// private section has NO (class,name)->FieldId index, but resolveField must
// be idempotent. The only lawful home for extra name->id entries is
// classByName_ (its declaration carries no constraining comment). Entries:
//   class internal name          -> 2 * ClassId.v      (even)
//   "\x01<class>\x01<field>"     -> 2 * FieldId.v + 1  (odd)
// Parity keeps the two entry kinds distinguishable even when keys collide
// under adversarial input (see classId() for the degradation story).
[[nodiscard]] constexpr bool isFieldRegistryEntry(std::uint32_t v) noexcept {
  return (v & 1u) != 0;
}

// Field-registry key. WHY \x01 separators: internal class names (JVMS 4.2.1
// charset, '/' separators) and Java field names (identifiers) can never
// contain ASCII control characters, so a composed key never equals a
// class-name key for any frontend-produced program. Only a hand-written
// hostile RBC file could spell a "\x01"-containing class name.
[[nodiscard]] std::string fieldRegistryKey(std::string_view cls,
                                           std::string_view name) {
  std::string key;
  key.reserve(cls.size() + name.size() + 2);
  key.push_back('\x01');
  key.append(cls);
  key.push_back('\x01');
  key.append(name);
  return key;
}

// One FIELD type descriptor -> RType ("I" -> Int, "Ljava/lang/String;" ->
// Ref, "[[I" -> Ref ...), Bottom when malformed.
//
// WHY a local parser instead of rbc::parseReturn (the BE-2 task pin said
// parseReturn): parseReturn is strictly a METHOD-descriptor parser - it
// requires a leading '(' and returns Bottom for every field descriptor
// (compiler/rbc/src/Rbc.cpp, verified against the real implementation).
// Following the pin's letter would make ResolvedField::type Bottom for ALL
// fields, contradicting the frozen header ("type from the field
// descriptor"). This parser implements the header's actual contract.
// Sub-integral descriptors (Z/B/C/S) fold into Int (Type.h: booleans,
// bytes, chars, shorts live in Int registers); field type metadata is
// ADVISORY in v0 - storage is uniform Value slots and per-type narrowing
// arrives with the real runtime.
[[nodiscard]] rbc::RType fieldDescriptorType(std::string_view d) noexcept {
  if (d.empty()) {
    return rbc::RType::Bottom;
  }
  switch (d.front()) {
  case 'I':
  case 'Z':
  case 'B':
  case 'C':
  case 'S':
    // Int plus the sub-integral folds (Type.h: booleans/bytes/chars/shorts
    // live in Int registers).
    return d.size() == 1 ? rbc::RType::Int
                         : rbc::RType::Bottom; // "Ix" is malformed
  case 'J':
    return d.size() == 1 ? rbc::RType::Long : rbc::RType::Bottom;
  case 'F':
    return d.size() == 1 ? rbc::RType::Float : rbc::RType::Bottom;
  case 'D':
    return d.size() == 1 ? rbc::RType::Double : rbc::RType::Bottom;
  case 'L': {
    // L <internal-name> ; - non-empty, no descriptor-structure characters.
    if (d.size() < 3 || d.back() != ';') {
      return rbc::RType::Bottom;
    }
    for (std::size_t i = 1; i + 1 < d.size(); ++i) {
      const char c = d[i];
      if (c == '(' || c == ')' || c == '[' || c == ';') {
        return rbc::RType::Bottom;
      }
    }
    return rbc::RType::Ref;
  }
  case '[': {
    // Any array depth is Ref; the element is one primitive char or an
    // L...; tail (sub-integral elements are legal in array position).
    std::size_t i = 0;
    while (i < d.size() && d[i] == '[') {
      ++i;
    }
    if (i >= d.size()) {
      return rbc::RType::Bottom; // bare "["
    }
    if (d[i] == 'L') {
      return (d.back() == ';' && i + 2 <= d.size() - 1) ? rbc::RType::Ref
                                                        : rbc::RType::Bottom;
    }
    if (i + 1 != d.size()) {
      return rbc::RType::Bottom;
    }
    switch (d[i]) {
    case 'I':
    case 'J':
    case 'F':
    case 'D':
    case 'Z':
    case 'B':
    case 'C':
    case 'S':
      return rbc::RType::Ref;
    default:
      return rbc::RType::Bottom;
    }
  }
  default:
    return rbc::RType::Bottom; // 'V', garbage, ...
  }
}

// v0 interim quickened-field pin (Interp.h): getfield_quick/putfield_quick
// carry BYTE offsets; the reference layout is Value slots.
constexpr std::uint32_t kValueSlotBytes = sizeof(Value); // == 16 (Value.h)

// The v0 println/print overload table (Runtime.h): receiver java/io/PrintStream.
constexpr std::string_view kPrintOverloads[] = {
    "()V",         "(I)V",         "(J)V",         "(F)V",
    "(D)V",        "(Z)V",         "(C)V",         "(Ljava/lang/String;)V",
    "(Ljava/lang/Object;)V"};

// Appends one Value in the execBuiltin value-tag format (see execBuiltin for
// the binding decision and documented divergences). Public-API-only so it
// needs no private access.
void appendJavaValue(std::string& out, const Value& v, const Heap& heap,
                     const Runtime& rt) {
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
  case rbc::RType::Ref: {
    const ObjRef r = v.ref();
    if (heap.isString(r)) {
      out += heap.stringOf(r);
      break;
    }
    // v0 deterministic toString pin: DottedClassName@<decimal objid>. Java
    // prints Class@hexHashCode; v0 has no hashCode, so the stable object id
    // (Rule 15) stands in - deterministic across runs (Rule 124), which the
    // golden-output corpus requires. DOCUMENTED DIVERGENCE from Java.
    std::string cls(rt.classNameOf(r));
    for (char& c : cls) {
      if (c == '/') {
        c = '.';
      }
    }
    out += cls;
    out.push_back('@');
    out += std::to_string(r.id);
    break;
  }
  case rbc::RType::Bottom:
    break; // defensive: never on verified streams; print nothing
  }
}

// --- multianewarray nest construction ---------------------------------------
//
// Builds one level of the nest: an array of dims[level] elements (missing
// dims default to 0 - the v0 pin that trailing unspecified dimensions are
// LENGTH-0 ARRAYS, not Java's nulls, rbc_spec.md SS3.13) whose per-level
// class is the full array descriptor with one fewer leading '[' at each
// nesting step, and whose innermost elements have RType baseElem.
[[nodiscard]] ObjRef buildMultiLevel(Runtime& rt,
                                     std::string_view levelClsName,
                                     std::size_t depthLeft, rbc::RType baseElem,
                                     std::span<const std::int32_t> dims,
                                     std::size_t level) {
  const std::int32_t rawLen = level < dims.size() ? dims[level] : 0;
  // Negative lengths are the CALLER's trap (NegativeArraySizeException is
  // raised before allocation, Heap.h); here they clamp to 0 defensively.
  const std::uint32_t len = rawLen > 0 ? static_cast<std::uint32_t>(rawLen) : 0;
  const ClassId cls = rt.classId(levelClsName);
  if (depthLeft <= 1) {
    return rt.heap().allocArray(cls, baseElem, len);
  }
  const ObjRef arr = rt.heap().allocArray(cls, rbc::RType::Ref, len);
  std::string_view childName = levelClsName;
  if (!childName.empty() && childName.front() == '[') {
    childName.remove_prefix(1); // one '[' shallower for the element class
  }
  for (std::uint32_t i = 0; i < len; ++i) {
    const ObjRef sub =
        buildMultiLevel(rt, childName, depthLeft - 1, baseElem, dims, level + 1);
    // Cannot fail (fresh array, i < len == slots.size()); the capture only
    // documents that the failure channel is deliberately ignored.
    [[maybe_unused]] const bool stored =
        rt.heap().storeElem(arr, i, Value::refVal(sub));
  }
  return arr;
}

} // namespace

// ============================================================================
// Construction / builtin class registry.
// ============================================================================

void Runtime::registerBuiltinClasses() {
  // The EXACT v0 hierarchy (BE-2 pin): internal names + superclass ids.
  // Registration order IS id order and is part of the observable surface:
  // java/lang/Object is ClassId{0}, which the chain walk in
  // isAssignableFrom() relies on (see the ClassId-0 note there). Throwable
  // and every class under it carry isExceptionClass = true.
  const auto addClass = [this](std::string_view name, ClassId super,
                               bool isExc) -> ClassId {
    ClassId cid{static_cast<std::uint32_t>(classes_.size())};
    ClassInfo ci;
    ci.name.assign(name);
    ci.superclass = super;
    ci.initialized = false;
    ci.isExceptionClass = isExc;
    classByName_.emplace(std::string(name), cid.v * 2u);
    classes_.push_back(std::move(ci));
    return cid;
  };

  // Root first: Object's own default superclass (ClassId{0} == itself)
  // self-loops and terminates every chain walk.
  objectClass_ = addClass("java/lang/Object", ClassId{}, false);
  stringClass_ = addClass("java/lang/String", objectClass_, false);
  classClass_ = addClass("java/lang/Class", objectClass_, false);
  addClass("java/io/PrintStream", objectClass_, false);
  addClass("java/lang/System", objectClass_, false);
  throwableClass_ = addClass("java/lang/Throwable", objectClass_, true);
  const ClassId exceptionCls =
      addClass("java/lang/Exception", throwableClass_, true);
  const ClassId errorCls = addClass("java/lang/Error", throwableClass_, true);
  const ClassId runtimeExCls =
      addClass("java/lang/RuntimeException", exceptionCls, true);
  addClass("java/lang/ArithmeticException", runtimeExCls, true);
  addClass("java/lang/NullPointerException", runtimeExCls, true);
  addClass("java/lang/ArrayStoreException", runtimeExCls, true);
  addClass("java/lang/ClassCastException", runtimeExCls, true);
  addClass("java/lang/IllegalMonitorStateException", runtimeExCls, true);
  const ClassId indexOutOfBoundsCls =
      addClass("java/lang/IndexOutOfBoundsException", runtimeExCls, true);
  addClass("java/lang/NegativeArraySizeException", runtimeExCls, true);
  addClass("java/lang/UnsupportedOperationException", runtimeExCls, true);
  addClass("java/lang/ArrayIndexOutOfBoundsException", indexOutOfBoundsCls,
           true);
  const ClassId vmErrorCls =
      addClass("java/lang/VirtualMachineError", errorCls, true);
  addClass("java/lang/InternalError", vmErrorCls, true);
  addClass("java/lang/StackOverflowError", vmErrorCls, true);
  const ClassId linkageErrorCls =
      addClass("java/lang/LinkageError", errorCls, true);
  addClass("java/lang/NoSuchMethodError", linkageErrorCls, true);
  addClass("java/lang/BootstrapMethodError", linkageErrorCls, true);
}

Runtime::Runtime(const rbc::Program& program) : program_(program) {
  registerBuiltinClasses();

  // System.out / System.err singletons, allocated eagerly so builtinStatic()
  // stays a const accessor and getstatic java/lang/System out|err always
  // hands out the same PrintStream identity (one System.out per VM). The
  // class id comes back by name because the frozen header has no
  // printStreamClass_ member (reported gap); registration just ran, so the
  // lookup cannot miss.
  const ClassId printStream = findClass("java/io/PrintStream");
  systemOut_ = heap_.allocInstance(printStream, 0);
  systemErr_ = heap_.allocInstance(printStream, 0);

  // Profiles cover EVERY program method from construction so the const
  // profiles() accessor is total (the frozen header makes it const, so it
  // cannot lazily size). MethodIds are indices into program_.methods.
  profiles_.resize(program_.methods.size());
}

// ============================================================================
// Class registry.
// ============================================================================

ClassId Runtime::classId(std::string_view internalName) {
  // Intern-or-create. Idempotence for legal names is the load-bearing
  // invariant (catch matching, assignability, field slots all key on it).
  if (auto it = classByName_.find(std::string(internalName));
      it != classByName_.end() && !isFieldRegistryEntry(it->second)) {
    return ClassId{it->second >> 1};
  }
  // PATHOLOGICAL branch (documented residual): the key exists but holds a
  // FIELD entry - possible only when a hand-written RBC file names a class
  // with an ASCII \x01 (frontends cannot produce it; see fieldRegistryKey).
  // The class WINS the key: registering below overwrites the field's
  // name->FieldId mapping, so that one field re-resolves to a fresh FieldId
  // (its statics split). No crash, no OOB access; class identity stays
  // intact. Verified streams never reach this branch.
  ClassId cid{static_cast<std::uint32_t>(classes_.size())};
  ClassInfo ci;
  ci.name.assign(internalName);
  // superclass stays ClassId{}: under the ClassId-0 pin (isAssignableFrom)
  // every user class walks up to Object and has no mid-hierarchy supers -
  // exactly the documented "no user hierarchy yet" limitation.
  ci.initialized = false;
  ci.isExceptionClass = false;
  classByName_[ci.name] = cid.v * 2u; // also the pathological overwrite
  classes_.push_back(std::move(ci));
  return cid;
}

ClassId Runtime::findClass(std::string_view internalName) const {
  // NOT-FOUND CONTRACT (BE-2 pin - one representation, picked and pinned
  // here): absent classes return the ONE-PAST-THE-END sentinel
  // ClassId{classes_.size()}. Callers probe validity through classInfo():
  // out-of-range ids (including this sentinel) map to the static EMPTY
  // ClassInfo and no real class has an empty name, so
  // classInfo(findClass(n)).name.empty() is the "not found" test.
  if (auto it = classByName_.find(std::string(internalName));
      it != classByName_.end() && !isFieldRegistryEntry(it->second)) {
    return ClassId{it->second >> 1};
  }
  return ClassId{static_cast<std::uint32_t>(classes_.size())};
}

const ClassInfo& Runtime::classInfo(ClassId id) const {
  // Defensive totality: out-of-range ids (including findClass's sentinel)
  // map to a static empty ClassInfo; the empty name doubles as the
  // "absent" probe (see findClass). Read-only magic static, thread-safe.
  static const ClassInfo kEmpty;
  if (id.v >= classes_.size()) {
    return kEmpty;
  }
  return classes_[id.v];
}

// ============================================================================
// Class initialization state (JVMS 5.5 first-use semantics).
// ============================================================================

bool Runtime::needsInit(ClassId id) const noexcept {
  // FROZEN-HEADER SEMANTICS (normative comment: "true iff THE CLASS has an
  // RBC <clinit>()V that has not been started"): in the v0 one-class world
  // only the program's own class can own RBC methods, so every other class
  // is vacuously clinit-less. The BE-2 task pin's letter ("search
  // program_.methods" with no class guard) would re-run the program's
  // <clinit> under a foreign class's initialized flag - double execution
  // of static initializers; the guard is required for correctness and
  // matches the frozen header. (Reported as a task-pin discrepancy.)
  if (id.v >= classes_.size() || classes_[id.v].initialized) {
    return false;
  }
  if (classes_[id.v].name != program_.className) {
    return false;
  }
  return program_.find("<clinit>", "()V") != nullptr;
}

MethodId Runtime::findClinit(ClassId id) const noexcept {
  // CONTRACT SUBTLETY (frozen header says "invalid if none", but MethodId{0}
  // is a VALID index - method 0 can be <clinit>): 'invalid' is expressed by
  // needsInit() == false. findClinit returns a usable MethodId IFF
  // needsInit(id) held; callers MUST gate on needsInit() FIRST. The final
  // null-check is defensive totality (needsInit() == true implies the find
  // succeeds).
  if (!needsInit(id)) {
    return MethodId{};
  }
  const rbc::Method* m = program_.find("<clinit>", "()V");
  return m != nullptr
             ? MethodId{static_cast<std::uint32_t>(m - program_.methods.data())}
             : MethodId{};
}

void Runtime::markInitialized(ClassId id) noexcept {
  // Call BEFORE executing <clinit> (JVMS 5.5) so recursive first-use
  // terminates; defensive no-op on out-of-range ids.
  if (id.v < classes_.size()) {
    classes_[id.v].initialized = true;
  }
}

// ============================================================================
// Assignability and names.
// ============================================================================

bool Runtime::isAssignableFrom(ClassId target, ClassId value) const noexcept {
  if (target.v >= classes_.size() || value.v >= classes_.size()) {
    return false;
  }
  // v0 ARRAY RULE (BE-2 pin): array classes (names starting with '[') match
  // by EXACT descriptor equality only. DOCUMENTED DIVERGENCE from Java: no
  // covariance ([Sub] is not [Super]) and not even array-to-Object ([I] is
  // not java/lang/Object) - checkcast/instanceof against Object on an array
  // throws in v0. The real class model adds JLS 4.10.3 subtyping.
  const std::string& targetName = classes_[target.v].name;
  const std::string& valueName = classes_[value.v].name;
  const bool targetIsArray = !targetName.empty() && targetName.front() == '[';
  const bool valueIsArray = !valueName.empty() && valueName.front() == '[';
  if (targetIsArray || valueIsArray) {
    return target == value;
  }
  // ClassId-0 OVERLOAD RESOLUTION (frozen-header bug, documented): "no
  // superclass" is ClassInfo::superclass's default ClassId{} (v == 0) and
  // java/lang/Object is ALSO the first registered class (id 0). The two
  // readings are indistinguishable in stored state, so v0 pins the
  // Java-correct one: v == 0 IS Object, and Object's own default superclass
  // therefore SELF-LOOPS and terminates the walk. Consequences (all
  // Java-correct): every class walks up to Object - so
  // isAssignableFrom(Object, x) is true for every non-array x, which
  // checkcast/instanceof and aastore-into-Object[] need - while user
  // classes still have no MID-hierarchy supers (their chain is just
  // [self, Object]), exactly the documented "no user hierarchy yet" v0
  // limitation.
  ClassId cur = value;
  for (std::size_t guard = 0; guard <= classes_.size(); ++guard) {
    if (cur == target) {
      return true;
    }
    const ClassId sup = classes_[cur.v].superclass;
    if (sup == cur || sup.v >= classes_.size()) {
      break; // root self-loop (Object) / defensive: dangling superclass
    }
    cur = sup;
  }
  return false; // also reached when the guard bounds a corrupt cycle
}

std::string_view Runtime::classNameOf(ObjRef r) const noexcept {
  // WHY peek() rather than heap().classOf(): ClassId{0} is BOTH Object's id
  // and classOf's dead-reference result (frozen-header overload bug), so a
  // dead ref would report "java/lang/Object". peek() distinguishes dead
  // from live; dead refs yield the empty name (defensive path - verified
  // streams NPE before reaching here).
  // VALIDITY CAVEAT: the view points into ClassInfo::name; a later class
  // registration can reallocate classes_ and move short (SSO) names. Cold
  // callers must copy immediately (b2run does); nothing on the dispatch
  // path holds this view across resolution.
  const HeapObj* o = heap_.peek(r);
  if (o == nullptr) {
    return {};
  }
  return classInfo(o->cls).name;
}

std::string Runtime::dottedClassName(ClassId id) const {
  // Message form: "java.lang.ArithmeticException" (Runtime.h). By value (a
  // copy), so it is immune to the classNameOf reallocation caveat.
  std::string out(classInfo(id).name);
  for (char& c : out) {
    if (c == '/') {
      c = '.';
    }
  }
  return out;
}

// ============================================================================
// Field resolution, quickened offsets, statics.
// ============================================================================

std::optional<ResolvedField> Runtime::resolveField(const rbc::Const& fieldRef) {
  if (fieldRef.kind != rbc::Const::Kind::FieldRef) {
    return std::nullopt; // malformed input (verifier-checked upstream)
  }
  // Intern-or-create the declaring class (cold path).
  const ClassId cid = classId(fieldRef.str);

  // Idempotence lookup: (class, name) -> FieldId through the dual-role
  // registry (see the encoding note at the top of this file).
  const std::string key = fieldRegistryKey(fieldRef.str, fieldRef.str2);
  const auto it = classByName_.find(key);
  if (it != classByName_.end() && isFieldRegistryEntry(it->second)) {
    const FieldId fid{it->second >> 1};
    if (fid.v < fieldIndex_.size() && fieldIndex_[fid.v].first == cid.v) {
      // Already registered: same FieldId, same slot (slots are append-only
      // so the stored slot stays valid), type re-derived from THIS const's
      // descriptor (the only descriptor source - field descriptors are not
      // stored in the frozen private state).
      return ResolvedField{fid, cid, fieldIndex_[fid.v].second,
                           fieldDescriptorType(fieldRef.str3)};
    }
    // Corrupt entry (unreachable): fall through and reclaim the key below.
  }

  // Register a new field: fresh FieldId, slot = position in the class's
  // layout, a statics slot, and a registry entry.
  const FieldId fid{static_cast<std::uint32_t>(fieldIndex_.size())};
  const std::uint32_t slot =
      static_cast<std::uint32_t>(classes_[cid.v].fields.size());
  classes_[cid.v].fields.push_back(fid);
  fieldIndex_.emplace_back(cid.v, slot); // FieldId -> (classIdx, slotIdx)
  statics_.push_back(Value::bottom());   // static storage for this field
  if (it == classByName_.end() || isFieldRegistryEntry(it->second)) {
    // Free key, or a corrupt field entry to reclaim. NOT taken when the key
    // holds a CLASS entry (the pathological \x01 class name; the class wins
    // - see classId()); that field stays unmapped and re-resolutions
    // allocate fresh FieldIds (documented residual, no crash).
    classByName_.insert_or_assign(std::move(key), fid.v * 2u + 1u);
  }
  return ResolvedField{fid, cid, slot, fieldDescriptorType(fieldRef.str3)};
}

std::uint32_t Runtime::fieldOffsetOf(std::uint32_t slot) const noexcept {
  // v0 interim pin (Interp.h): quickened field offsets are slot * 16.
  return slot * kValueSlotBytes;
}

std::uint32_t Runtime::slotOfFieldOffset(std::uint32_t offset) const noexcept {
  return offset / kValueSlotBytes;
}

// ============================================================================
// Method resolution and the builtin virtual seam.
// ============================================================================

std::optional<MethodId> Runtime::resolveMethod(const rbc::Const& methodRef) const {
  if (methodRef.kind != rbc::Const::Kind::MethodRef &&
      methodRef.kind != rbc::Const::Kind::InterfaceMethodRef) {
    return std::nullopt;
  }
  // v0 world: one Program models ONE class; the ref's class must equal
  // program_.className EXACTLY (cold-path string compare). Builtin
  // receivers (PrintStream println/print) never get this far - the call
  // protocol probes lookupBuiltinVirtual first (Interp.h); anything else
  // resolves to nullopt and the caller raises NoSuchMethodError.
  if (methodRef.str != program_.className) {
    return std::nullopt;
  }
  const rbc::Method* m = program_.find(methodRef.str2, methodRef.str3);
  if (m == nullptr) {
    return std::nullopt;
  }
  return MethodId{static_cast<std::uint32_t>(m - program_.methods.data())};
}

const rbc::Method& Runtime::method(MethodId id) const noexcept {
  // Defensive totality (no exceptions, Rule 6): out-of-range ids return a
  // static dummy with a recognizable name. The dispatch core only passes
  // ids resolveMethod produced (those index program_.methods); anything
  // else is adversarial and gets total behavior, never a crash.
  static const rbc::Method kDummy = [] {
    rbc::Method m;
    m.name = "<b2-bad-method-id>";
    m.descriptor = "()V";
    return m;
  }();
  if (id.v >= program_.methods.size()) {
    return kDummy;
  }
  return program_.methods[id.v];
}

Runtime::Builtin Runtime::lookupBuiltinVirtual(
    ClassId receiverClass, std::string_view name,
    std::string_view descriptor) const {
  // WHY a class-NAME compare instead of a stored class id: the frozen
  // header has no printStreamClass_ member (reported gap), and classInfo()
  // is defensive, so an out-of-range receiverClass safely misses. This
  // probe runs on the IC-MISS path only (Interp.h call protocol), keeping
  // the string compares off the hot path (Rule 16).
  if (name != "println" && name != "print") {
    return Builtin::None;
  }
  if (classInfo(receiverClass).name != "java/io/PrintStream") {
    return Builtin::None;
  }
  for (const std::string_view overload : kPrintOverloads) {
    if (overload == descriptor) {
      return name == "println" ? Builtin::Println : Builtin::Print;
    }
  }
  return Builtin::None;
}

bool Runtime::execBuiltin(Builtin b, std::span<const Value> args) {
  // ==========================================================================
  // FROZEN-SIGNATURE GAP - BINDING v0 DECISION (reported to the integrator):
  // the signature execBuiltin(Builtin, span<const Value>) cannot carry the
  // call-site DESCRIPTER, and a Value alone cannot distinguish (I)V from
  // (Z)V or (C)V. BINDING PIN: formatting is by RUNTIME VALUE TAG - Int
  // decimal, Long decimal, Float/Double via javaFloatString/javaDoubleString,
  // Null "null", Ref the String payload for String receivers else
  // "DottedClassName@<objid>". DOCUMENTED DIVERGENCES from Java:
  //   println(Z) prints "0"/"1" (Java: "true"/"false"),
  //   println(C) prints the code point decimal (Java: the character),
  //   println(Object) prints "@<decimal objid>" (Java: "@<hex hashCode>";
  //   v0 has no hashCode - the pin keeps output deterministic, Rule 124).
  // Output-parity corpus and tests must use (I)/(J)/(F)/(D)/
  // (Ljava/lang/String;) descriptors. v1 proposal: pass the descriptor
  // (execBuiltin(Builtin, std::string_view, span<const Value>)).
  // ==========================================================================
  if (b == Builtin::None) {
    return false;
  }
  if (args.empty() || args.size() > 2) {
    // Arity mismatch: receiver (args[0]) plus at most one value argument.
    // Verified streams never produce this; the caller raises
    // NoSuchMethodError.
    return false;
  }
  // Sink: the System.err singleton writes to stderr; everything else
  // (including the defensive non-Ref receiver, which NPE'd upstream anyway)
  // writes to stdout. b2run forwards both buffers.
  const bool toErr = args[0].isRef() && args[0].as.obj == systemErr_.id;
  std::string& sink = toErr ? err_ : out_;
  if (args.size() == 2) {
    appendJavaValue(sink, args[1], heap_, *this);
  }
  if (b == Builtin::Println) {
    sink.push_back('\n');
  }
  return true;
}

// ============================================================================
// Allocation.
// ============================================================================

ObjRef Runtime::newInstance(ClassId cls) {
  // slotHint = the class's CURRENT layout size; fields resolved after this
  // allocation grow the instance on first access (Heap lazy layout).
  return heap_.allocInstance(
      cls, static_cast<std::uint32_t>(classInfo(cls).fields.size()));
}

ObjRef Runtime::newArray(rbc::Atype atype, std::uint32_t length) {
  // Atype -> (element RType, array class descriptor). WHY sub-int arrays use
  // elem Int: RBC folds byte/char/short/boolean into Int (Type.h); the
  // narrowing lives in the *aload/*astore opcodes (rbc_spec.md SS3.13).
  rbc::RType elem = rbc::RType::Int;
  const char* clsName = "[I";
  switch (atype) {
  case rbc::Atype::Boolean:
    clsName = "[Z";
    break;
  case rbc::Atype::Char:
    clsName = "[C";
    break;
  case rbc::Atype::Float:
    elem = rbc::RType::Float;
    clsName = "[F";
    break;
  case rbc::Atype::Double:
    elem = rbc::RType::Double;
    clsName = "[D";
    break;
  case rbc::Atype::Byte:
    clsName = "[B";
    break;
  case rbc::Atype::Short:
    clsName = "[S";
    break;
  case rbc::Atype::Int:
    break; // defaults above
  case rbc::Atype::Long:
    elem = rbc::RType::Long;
    clsName = "[J";
    break;
  }
  // Unknown atype codes (defensive; the verifier range-checks 4..11)
  // degrade to int[] semantics instead of crashing.
  return heap_.allocArray(classId(clsName), elem, length);
}

ObjRef Runtime::newRefArray(ClassId elementClass, std::uint32_t length) {
  // Array-class descriptor: "[L<name>;" for class elements; an ARRAY
  // element class (name already '['-prefixed) composes by prepending one
  // more '[' (array-of-array, e.g. a "[Ljava/lang/String;" element gives
  // "[[Ljava/lang/String;").
  const ClassInfo& ec = classInfo(elementClass);
  std::string name;
  if (ec.name.empty()) {
    name = "[Ljava/lang/Object;"; // defensive: unknown class id
  } else if (ec.name.front() == '[') {
    name.assign(ec.name);
    name.insert(name.begin(), '[');
  } else {
    name = "[L";
    name += ec.name;
    name.push_back(';');
  }
  return heap_.allocArray(classId(name), rbc::RType::Ref, length);
}

ObjRef Runtime::newMultiArray(ClassId elementClass,
                              std::span<const std::int32_t> dims) {
  // CONTRACT (BE-2 pin): `elementClass` is the ARRAY CLASS named by the
  // multianewarray Class constant (e.g. "[[I"); its leading-'[' count is
  // the TOTAL nest depth; `dims` are the per-level lengths outer-to-inner,
  // trailing levels defaulting to 0 (v0 pin: length-0 sub-arrays, NOT
  // Java's nulls - rbc_spec.md SS3.13). Negative lengths are the caller's
  // trap (NegativeArraySizeException fires before allocation); they clamp
  // to 0 here.
  // COPY (not a reference): classId() calls inside buildMultiLevel intern the
  // per-level classes, which can GROW classes_ and move ClassInfo storage -
  // a reference/view into the registry would dangle mid-recursion.
  const std::string fullName = classInfo(elementClass).name;
  std::size_t depth = 0;
  while (depth < fullName.size() && fullName[depth] == '[') {
    ++depth;
  }
  if (depth == 0) {
    // Defensive: a non-array class name (the verifier's multianewarray cp is
    // a Class const; a non-'[' one is hostile). Degrade to a one-level
    // reference array over that class.
    std::string name = "[L";
    name += fullName.empty() ? "java/lang/Object" : fullName;
    name.push_back(';');
    const std::int32_t rawLen = dims.empty() ? 0 : dims.front();
    return heap_.allocArray(
        classId(name), rbc::RType::Ref,
        rawLen > 0 ? static_cast<std::uint32_t>(rawLen) : 0);
  }
  // JVMS 4.4.1 bounds array descriptors at 255 dimensions; hostile deeper
  // names clamp so the recursion in buildMultiLevel is stack-bounded
  // (never crash; verified streams never exceed 255 anyway).
  if (depth > 255) {
    depth = 255;
  }
  // Base element type from the descriptor suffix behind the brackets
  // ("I" -> Int, "Ljava/lang/String;" -> Ref, "Z" -> Int sub-int fold...).
  // Garbage suffixes degrade to Ref (null-filled innermost level) rather
  // than Bottom-filled (arrays must hold their element type, Heap.h).
  const std::string_view suffix(fullName.data() + depth, fullName.size() - depth);
  rbc::RType baseElem = fieldDescriptorType(suffix);
  if (baseElem == rbc::RType::Bottom) {
    baseElem = rbc::RType::Ref;
  }
  return buildMultiLevel(*this, fullName, depth, baseElem, dims, 0);
}

ObjRef Runtime::internString(std::string_view utf8) {
  // ldc materializer: JVMS 5.1 interning (equal constants are one object,
  // which if_acmpeq depends on).
  return heap_.internString(utf8, stringClass_);
}

// ============================================================================
// Statics.
// ============================================================================

FieldId Runtime::staticOf(const ResolvedField& rf) const noexcept {
  // v0 storage split (Runtime.h): instance fields key on (class, slot) and
  // statics key on FieldId - the SAME FieldId space the resolver handed out,
  // so the static storage id of a resolved field IS its identity. A thin
  // out-of-line twin of fieldIdOf() (the header defines that one inline).
  return rf.id;
}

Value Runtime::loadStatic(FieldId f) const noexcept {
  // Unset statics read as Bottom ("uninitialized"); so do beyond-size ids
  // (defensive; storeStatic is the only grower and it Bottom-fills).
  if (f.v >= statics_.size()) {
    return Value::bottom();
  }
  return statics_[f.v];
}

void Runtime::storeStatic(FieldId f, Value v) {
  // Static storage grows Bottom-filled so field ids stay stable (Rule 15)
  // and never-written statics read as Bottom.
  if (f.v >= statics_.size()) {
    statics_.resize(f.v + 1, Value::bottom());
  }
  statics_[f.v] = v;
}

// ============================================================================
// Exception construction (JVM messages are chosen by the CALLER; this is the
// object/message machinery, Rule 74: exceptions are values).
// ============================================================================

ObjRef Runtime::makeException(std::string_view internalName,
                              std::string_view message) {
  const ClassId cid = classId(internalName); // intern-or-create
  // Exception marking: builtin names were flagged at registration; an
  // UNKNOWN name registered here stays plain - exact-name catch matching
  // still works (isAssignableFrom is reflexive) but catch Throwable misses
  // it (documented v0 limitation: no user hierarchy). The chain walk below
  // re-marks any class that ends up under Throwable.
  if (cid.v < classes_.size() && !classes_[cid.v].isExceptionClass &&
      isAssignableFrom(throwableClass_, cid)) {
    classes_[cid.v].isExceptionClass = true;
  }
  const ObjRef obj = heap_.allocInstance(
      cid, static_cast<std::uint32_t>(classInfo(cid).fields.size()));

  // The message becomes Throwable's "detailMessage" String field, resolved
  // ON the java/lang/Throwable class: every subclass instance shares the
  // slot index (lazy instance growth covers subclasses whose own layout is
  // shorter - v0 has no field inheritance; resolving Throwable fields on
  // subclass names would alias slots and is rejected by convention).
  // PIN: even an EMPTY message stores an empty String object. Java's no-arg
  // constructors leave detailMessage null; v0 pins "" so exceptionMessage
  // is total and message identity is stable. No v0 observable difference
  // (getMessage does not exist yet); reported for the v1 message model.
  rbc::Const detail;
  detail.kind = rbc::Const::Kind::FieldRef;
  detail.str = "java/lang/Throwable";
  detail.str2 = "detailMessage";
  detail.str3 = "Ljava/lang/String;";
  const std::optional<ResolvedField> rf = resolveField(detail);
  if (rf) { // resolveField only rejects non-FieldRef kinds - never here
    // Cannot fail (live instance just allocated); the capture only
    // documents that the failure channel is deliberately ignored.
    [[maybe_unused]] const bool stored = heap_.storeField(
        obj, rf->slot, Value::refVal(heap_.internString(message, stringClass_)));
  }
  return obj;
}

std::string_view Runtime::exceptionMessage(ObjRef r) const noexcept {
  // noexcept + ALLOCATION-FREE discipline: building the registry key string
  // could terminate under OOM (bad_alloc inside noexcept), so the
  // detailMessage slot is found by a structural scan of classByName_ (the
  // key format is pinned in fieldRegistryKey). Cold path (trap reporting),
  // small map, unique match - deterministic despite unordered iteration.
  constexpr std::string_view kThrowable = "java/lang/Throwable";
  constexpr std::string_view kDetailMessage = "detailMessage";
  constexpr std::uint32_t kNoSlot = std::numeric_limits<std::uint32_t>::max();
  const std::size_t wantLen =
      kThrowable.size() + kDetailMessage.size() + 2; // two \x01 separators
  std::uint32_t slot = kNoSlot;
  for (const auto& entry : classByName_) {
    const std::string& key = entry.first;
    if (!isFieldRegistryEntry(entry.second) || key.size() != wantLen ||
        key.front() != '\x01') {
      continue;
    }
    const std::size_t sep = 1 + kThrowable.size();
    if (key[sep] != '\x01' ||
        std::string_view(key).substr(1, kThrowable.size()) != kThrowable ||
        std::string_view(key).substr(sep + 1) != kDetailMessage) {
      continue;
    }
    const FieldId fid{entry.second >> 1};
    if (fid.v < fieldIndex_.size()) {
      slot = fieldIndex_[fid.v].second;
    }
    break;
  }
  if (slot == kNoSlot) {
    return {}; // detailMessage was never resolved (no exception built yet)
  }
  // WHY peek() (documented borrow: Heap reserves it for tests/dumper, but
  // this is the same cold diagnostic family): exceptionMessage is const and
  // Heap::loadField is not (lazy growth). The read never needs growth -
  // only makeException stores this field and its storeField grew first.
  const HeapObj* o = heap_.peek(r);
  if (o == nullptr || o->kind != HeapObj::Kind::Instance ||
      slot >= o->slots.size()) {
    return {};
  }
  const Value& v = o->slots[slot];
  if (!v.isRef()) {
    return {}; // Bottom/absent field: no message
  }
  // VALIDITY CAVEAT: the view points into the String's payload; it is valid
  // until the next heap allocation (HeapObj vector growth can move short
  // strings). Cold callers copy immediately (b2run does).
  return heap_.stringOf(v.ref());
}

// ============================================================================
// Class objects and builtin statics.
// ============================================================================

ObjRef Runtime::classObject(ClassId id) {
  // One java/lang/Class instance per class per Runtime (JVMS 8.4.3.6
  // synchronized-static lock target; ldc-of-Class materialization).
  // classObjects_ is lazily sized by ClassId index (0 = none yet) because
  // user/array classes appear dynamically.
  if (id.v >= classObjects_.size()) {
    classObjects_.resize(id.v + 1);
  }
  if (!classObjects_[id.v].valid()) {
    classObjects_[id.v] = heap_.allocInstance(
        classClass_,
        static_cast<std::uint32_t>(classInfo(classClass_).fields.size()));
  }
  // Defensive note: an out-of-registry id still gets a Class instance - v0
  // Class objects carry no back-pointer (they are lock targets and
  // materializations), so this is total and harmless.
  return classObjects_[id.v];
}

std::optional<ObjRef> Runtime::builtinStatic(const rbc::Const& fieldRef) const {
  // The exact v0 builtin-static table (Runtime.h): System.out / System.err.
  // String compares are fine: getstatic resolves through its IC, so only
  // the first use of each site pays them.
  if (fieldRef.kind != rbc::Const::Kind::FieldRef) {
    return std::nullopt;
  }
  if (fieldRef.str != "java/lang/System") {
    return std::nullopt;
  }
  if (fieldRef.str3 != "Ljava/io/PrintStream;") {
    return std::nullopt;
  }
  if (fieldRef.str2 == "out") {
    return systemOut_;
  }
  if (fieldRef.str2 == "err") {
    return systemErr_;
  }
  return std::nullopt;
}

// ============================================================================
// Profiling (Rule 114: saturating, overflow-safe).
// ============================================================================

const std::vector<MethodProfile>& Runtime::profiles() const noexcept {
  // Sized to program_.methods.size() at construction (see the ctor), so
  // every valid MethodId has an entry and this const accessor is total.
  return profiles_;
}

void Runtime::bumpInvocations(MethodId id) noexcept {
  // Out-of-range ids are DROPPED (not grown): the ctor pre-sized the table
  // for every program method, so growth here would only serve adversarial
  // callers - and resize inside noexcept could terminate under OOM.
  if (id.v >= profiles_.size()) {
    return;
  }
  std::uint32_t& c = profiles_[id.v].invocations;
  if (c != std::numeric_limits<std::uint32_t>::max()) {
    ++c; // saturating (Rule 114)
  }
}

void Runtime::bumpBackedge(MethodId id) noexcept {
  if (id.v >= profiles_.size()) {
    return;
  }
  std::uint32_t& c = profiles_[id.v].backedges;
  if (c != std::numeric_limits<std::uint32_t>::max()) {
    ++c; // saturating (Rule 114)
  }
}

} // namespace b2::interp
