#pragma once
// B-2 Interpreter - the v0 reference runtime services.
//
// WHY THIS FILE EXISTS:
// The dispatch core (Interp.cpp) must not know how classes, fields, methods,
// monitors, or output are implemented. It consumes THIS seam, which fronts
// the reference object model (Heap.h) plus resolution, statics, builtins,
// trap-object construction, and profiling. When the real runtime/ arrives
// (class loading, GC, JNI, real monitors), it replaces this implementation
// behind the same header shape and the dispatch core does not change. This
// is the interpreter team's side of the future runtime boundary (charter:
// runtime services "used, never modified").
//
// KEY INVARIANTS:
// - Resolution and string work happen on the MISS path only; the hot path
//   consumes integer ids (Rules 7, 16). Interned names become ClassId/
//   FieldId/MethodId; nothing on the dispatch path compares strings.
// - Every fallible operation reports via return value; no C++ exceptions
//   cross this API (Rule 6). Trap conditions become exception ObjRefs the
//   CALLER dispatches (Rule 74: exceptions are values).
// - The builtin method table is the seed of the native/JNI seam: virtual
//   calls first probe it by (receiver class, name, descriptor), then fall
//   back to normal resolution.
// - Profiling counters are saturating and overflow-safe (Rule 114); v0 is
//   single-threaded, the racy-but-bounded concurrent form is documented in
//   docs/interp_contract.md as the future contract.
// - Statics: getstatic/putstatic/invokestatic/new trigger class
//   initialization on first use (JVMS 5.5); the class is marked initialized
//   BEFORE <clinit> runs so recursive first-use terminates (same JVMS rule).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "b2/interp/Heap.h"
#include "b2/interp/Value.h"
#include "b2/rbc/Rbc.h"

namespace b2::interp {

// --- builtins ---------------------------------------------------------------
//
// The v0 builtin method table. Recognized receivers/methods (executed by the
// runtime itself, never by RBC):
//
//   getstatic java/lang/System out : Ljava/io/PrintStream;   -> singleton
//   getstatic java/lang/System err : Ljava/io/PrintStream;   -> singleton
//   virtual  java/io/PrintStream println/print:
//            ()V (I)V (J)V (F)V (D)V (Z)V (C)V (Ljava/lang/String;)V
//            (Ljava/lang/Object;)V   [Object prints "Class@id" or the string]
//
// println/print output uses Java's exact formatting rules (JDK
// Double.toString/Float.toString semantics); see javaDoubleString below.
// System.out appends to stdout(), System.err to stderr(); b2run forwards the
// buffers. This table is the v0 native-call seam and is pinned in
// docs/interp_contract.md.

// Java-format a double exactly like java.lang.Double.toString(double):
// "NaN", "Infinity", "-Infinity", "-0.0"; otherwise the shortest decimal
// that round-trips, plain notation when 10^-3 <= |v| < 10^7, else
// scientific "d.dddE±x" with one digit before the point, no '+' and no
// leading zeros in the exponent, and always >= 1 fraction digit.
[[nodiscard]] std::string javaDoubleString(double v);

// Java-format a float exactly like java.lang.Float.toString(float): same
// rules as javaDoubleString with single-precision shortest digits.
[[nodiscard]] std::string javaFloatString(float v);

// --- resolved field (what the field IC memoizes) -----------------------------
struct ResolvedField {
  FieldId id;               // global field id (statics storage key)
  ClassId cls;              // declaring class (matches the FieldRef's class)
  std::uint32_t slot = 0;   // slot index within instances of cls
  rbc::RType type{};        // from the field descriptor
};

// --- profiling (Rule 114: saturating, overflow-safe) -------------------------
struct MethodProfile {
  std::uint32_t invocations = 0; // method entries (saturating add)
  std::uint32_t backedges = 0;   // backward branches + polls (saturating add)
};

// --- runtime -----------------------------------------------------------------
class Runtime {
public:
  explicit Runtime(const rbc::Program& program);

  // --- class registry (cold path; ids are interned) -------------------------
  // Interns the class (creating it if unknown) and returns its id.
  [[nodiscard]] ClassId classId(std::string_view internalName);
  [[nodiscard]] ClassId findClass(std::string_view internalName) const; // no create
  [[nodiscard]] const ClassInfo& classInfo(ClassId id) const;

  // Builtin class ids (always valid; registered in the constructor).
  [[nodiscard]] ClassId objectClass() const noexcept { return objectClass_; }
  [[nodiscard]] ClassId stringClass() const noexcept { return stringClass_; }
  [[nodiscard]] ClassId throwableClass() const noexcept { return throwableClass_; }
  [[nodiscard]] ClassId classClass() const noexcept { return classClass_; }

  // The java.lang.Class instance for a class (synchronized-static lock
  // target, JVMS 8.4.3.6; also what ldc-of-Class materializes). Cached: one
  // class object per class per Runtime.
  [[nodiscard]] ObjRef classObject(ClassId id);

  // Builtin getstatic probe: returns the System.out / System.err singleton
  // PrintStream for those exact FieldRefs, nullopt for everything else.
  [[nodiscard]] std::optional<ObjRef> builtinStatic(const rbc::Const& fieldRef) const;

  // Class initialization state (JVMS 5.5 first-use semantics). needsInit is
  // true iff the class has an RBC <clinit>()V that has not been started;
  // markInitialized flips the state (call BEFORE executing <clinit>).
  [[nodiscard]] bool needsInit(ClassId id) const noexcept;
  [[nodiscard]] MethodId findClinit(ClassId id) const noexcept; // invalid if none
  void markInitialized(ClassId id) noexcept;

  // Assignability for catch matching, instanceof, checkcast, aastore. v0:
  // exact class match, the builtin exception hierarchy, null-passes-cast,
  // and array classes by exact descriptor; no user hierarchy yet (documented
  // limitation, docs/interp_contract.md).
  [[nodiscard]] bool isAssignableFrom(ClassId target, ClassId value) const noexcept;
  // Class name of an object, as printed in diagnostics / ClassCastException.
  [[nodiscard]] std::string_view classNameOf(ObjRef r) const noexcept;
  // Dot form for messages: "java.lang.ArithmeticException".
  [[nodiscard]] std::string dottedClassName(ClassId id) const;

  // --- resolution (cold; memoized by the interpreter's ICs, not here) --------
  // FieldRef (class, name, descriptor) -> slot. Registers the field lazily.
  // nullopt when the const is not a FieldRef or the descriptor is malformed.
  [[nodiscard]] std::optional<ResolvedField> resolveField(const rbc::Const& fieldRef);
  // Resolve the quickened getfield_quick/putfield_quick byte offset back to
  // a slot (v0 interim pin: offset = slot * sizeof(Value)).
  [[nodiscard]] std::uint32_t fieldOffsetOf(std::uint32_t slot) const noexcept;
  [[nodiscard]] std::uint32_t slotOfFieldOffset(std::uint32_t offset) const noexcept;
  // FieldId of a lazily-resolved field for static access (identity: the
  // ResolvedField::id the resolver handed out).
  [[nodiscard]] FieldId fieldIdOf(const ResolvedField& rf) const noexcept {
    return rf.id;
  }

  // MethodRef / InterfaceMethodRef -> program method. v0 world: methods are
  // resolved by (name, descriptor) within the program; the ref's class must
  // equal the program class (or be a builtin). nullopt = NoSuchMethodError.
  [[nodiscard]] std::optional<MethodId> resolveMethod(const rbc::Const& methodRef) const;
  [[nodiscard]] const rbc::Method& method(MethodId id) const noexcept;
  [[nodiscard]] MethodId invalidMethod() const noexcept { return MethodId{}; }

  // Builtin virtual call probe (println/print). nullopt = not a builtin;
  // otherwise the call executes via execBuiltin below.
  enum class Builtin : std::uint8_t { None, Println, Print };
  [[nodiscard]] Builtin lookupBuiltinVirtual(ClassId receiverClass,
                                             std::string_view name,
                                             std::string_view descriptor) const;
  // Executes a builtin against `args` (receiver included, RBC call layout).
  // Returns false on a descriptor arity mismatch (caller raises
  // NoSuchMethodError - cannot happen on verified streams).
  [[nodiscard]] bool execBuiltin(Builtin b, std::span<const Value> args);

  // --- allocation (hot; thin wrappers over Heap) ------------------------------
  [[nodiscard]] ObjRef newInstance(ClassId cls);           // slots from layout
  [[nodiscard]] ObjRef newArray(rbc::Atype atype, std::uint32_t length);
  [[nodiscard]] ObjRef newRefArray(ClassId elementClass, std::uint32_t length);
  [[nodiscard]] ObjRef newMultiArray(ClassId elementClass,
                                     std::span<const std::int32_t> dims);
  [[nodiscard]] ObjRef internString(std::string_view utf8); // ldc materializer
  [[nodiscard]] Heap& heap() noexcept { return heap_; }

  // --- statics -----------------------------------------------------------------
  [[nodiscard]] Value loadStatic(FieldId f) const noexcept; // Bottom if unset
  void storeStatic(FieldId f, Value v);
  // Static-field storage id for a resolved field (fields and statics share
  // the FieldId space in v0; instances use (cls,slot), statics use FieldId).
  [[nodiscard]] FieldId staticOf(const ResolvedField& rf) const noexcept;

  // --- exception construction (JVM messages pinned, see rbc_spec.md SS3) -------
  // Classes: ArithmeticException "/ by zero", NullPointerException ""
  // (Java's helpful NPE messages need bytecode context; v0 pins the classic
  // empty message), ArrayIndexOutOfBoundsException "Index N out of bounds
  // for length L", NegativeArraySizeException "N", ClassCastException
  // "class X cannot be cast to class Y" (dotted names), ArrayStoreException
  // "class X", IllegalMonitorStateException "" (JVM: no message),
  // StackOverflowError "", NoSuchMethodError "C.n(D)",
  // BootstrapMethodError "invokedynamic not supported in v0",
  // InternalError "msg" (defensive: invariant violations become Java-visible
  // errors, never crashes - the verifier totality discipline).
  [[nodiscard]] ObjRef makeException(std::string_view internalName,
                                     std::string_view message);
  // The message of a Throwable instance (its "detailMessage" field), "" if none.
  [[nodiscard]] std::string_view exceptionMessage(ObjRef r) const noexcept;

  // --- output ---------------------------------------------------------------------
  [[nodiscard]] std::string& stdout() noexcept { return out_; }
  [[nodiscard]] std::string& stderr() noexcept { return err_; }

  // --- profiling --------------------------------------------------------------------
  [[nodiscard]] const std::vector<MethodProfile>& profiles() const noexcept;
  void bumpInvocations(MethodId id) noexcept;  // saturating
  void bumpBackedge(MethodId id) noexcept;     // saturating

private:
  void registerBuiltinClasses();

  const rbc::Program& program_;
  Heap heap_;
  std::vector<ClassInfo> classes_;
  std::unordered_map<std::string, std::uint32_t> classByName_;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> fieldIndex_; // FieldId -> (classIdx, slotIdx)
  std::vector<Value> statics_;      // indexed by FieldId.v
  std::vector<MethodProfile> profiles_; // indexed by MethodId.v
  std::vector<ObjRef> classObjects_;    // indexed by ClassId.v (0 = none yet)
  ObjRef systemOut_{};
  ObjRef systemErr_{};
  ClassId objectClass_{}, stringClass_{}, throwableClass_{}, classClass_{};
  std::string out_, err_;
};

} // namespace b2::interp
