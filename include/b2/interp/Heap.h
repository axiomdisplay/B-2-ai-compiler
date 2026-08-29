#pragma once
// B-2 Interpreter - the v0 reference object model (heap storage).
//
// WHY THIS FILE EXISTS:
// T0 must execute verified RBC end to end (Rule 96), and RBC's object-plane
// opcodes (new/getfield/putfield/monitorenter/invokevirtual/...) need storage
// and layout behind them. The real runtime (runtime/: class loading, GC,
// monitors, JNI) does not exist yet, so the interpreter team ships this
// REFERENCE object model behind a narrow service seam (Runtime.h). It is
// Java-semantics-honest (identity, interning, monitor reentrancy, trap
// classes with the JVM's messages) and deliberately NOT a performance heap.
// When runtime/ lands, this file is replaced by call-outs to it; nothing in
// the dispatch core (Interp.cpp) changes because the core only talks to
// Runtime.h.
//
// KEY INVARIANTS:
// - Object ids are stable indices (Rule 15): HeapObj storage may move on
//   vector growth, ids never do; ObjRef is safe across reallocation.
// - One uniform slot array per object: instances store fields in slot order,
//   arrays store elements; 16 bytes per slot (Value). Correctness first;
//   the real heap uses typed storage + GC maps (docs/stencils.md SS8.2).
// - Field layout is lazy (v0): a (class,name) pair gets a slot the first time
//   it is resolved; instances allocated earlier grow to fit on first access.
//   The real runtime computes layouts at class-load time.
// - Monitors are per-object, reentrant, single-owner in v0 (single-threaded
//   T0; the concurrency contract arrives with the real runtime, Rule 118).
// - String literals are interned per Runtime (JVMS 5.1: the same string
//   constant is the same object), which if_acmpeq depends on.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "b2/interp/Value.h"
#include "b2/rbc/Rbc.h"

namespace b2::interp {

// --- ids -----------------------------------------------------------------
//
// All ids are dense, zero-based-into-their-table, and stable for the lifetime
// of the owning Runtime. They exist so the hot paths pass small integers
// instead of strings (Rule 16: no strings in the hot path).
struct ClassId {
  std::uint32_t v = 0;
  [[nodiscard]] constexpr bool operator==(const ClassId&) const noexcept = default;
};
struct FieldId {
  std::uint32_t v = 0;
  [[nodiscard]] constexpr bool operator==(const FieldId&) const noexcept = default;
};

// v0 interim pin (docs/interp_contract.md): a MethodId is the index of the
// Method in the Program's method table. The quickener will allocate real
// global ids; that change is an RFC to this team (contract version bump).
struct MethodId {
  std::uint32_t v = 0;
  [[nodiscard]] constexpr bool operator==(const MethodId&) const noexcept = default;
};

// --- class registry --------------------------------------------------------
//
// v0 world model: a flat registry of classes by internal name. The standard
// library exception classes carry a hard-coded mini-hierarchy so realistic
// Java catch chains work (catch Exception catching ArithmeticException);
// user classes have no superclass until the class model arrives with the
// loader. Array classes are regular registry entries named "[I", "[D",
// "[Ljava/lang/String;" (JVM internal descriptors).
struct ClassInfo {
  std::string name;            // internal name, "java/lang/String" or "[I"
  ClassId superclass;          // valid() only for the builtin hierarchy
  bool initialized = false;    // <clinit> run (or started; JVMS 5.5 marks
                               // before running so recursion terminates)
  std::vector<FieldId> fields; // instance layout, slot order (lazy growth)
  bool isExceptionClass = false;
};

// --- heap objects ----------------------------------------------------------
//
// Kind::Instance: `slots` holds the instance fields of class `cls` in
//   ClassInfo::fields order; `str` holds the UTF-8 payload iff the class is
//   java/lang/String (v0 keeps String contents out of band so ldc/println
//   need no char[] machinery).
// Kind::Array: `slots` holds the elements; `elem` is the RType of the
//   elements (Int for byte/char/short/boolean arrays too - narrowing lives in
//   the *aload/*astore opcodes, rbc_spec.md SS3.13); `length` is the element
//   count; `cls` is the array class ("[I" etc).
struct MonitorState {
  std::uint32_t owner = 0;  // 0 = free; else the owning "thread" id (v0: 1)
  std::uint32_t count = 0;  // reentrancy depth
};

struct HeapObj {
  enum class Kind : std::uint8_t { Instance, Array };

  Kind kind = Kind::Instance;
  ClassId cls;                  // instance class or array class
  std::string str;              // String payload (java/lang/String only)
  std::vector<Value> slots;     // fields (Instance) or elements (Array)
  rbc::RType elem = rbc::RType::Bottom; // Array only
  std::uint32_t length = 0;     // Array only
  MonitorState mon;
};

// --- the heap --------------------------------------------------------------
//
// Total: every accessor returns a status (std::optional-like) instead of
// throwing; the CALLER (dispatch core) turns failures into Java traps.
// C++ exceptions never cross this API (Rule 6).
class Heap {
public:
  Heap() = default;
  Heap(const Heap&) = delete;
  Heap& operator=(const Heap&) = delete;

  // --- allocation (never fails in v0; size is bounded by the interpreter's
  //     NegativeArraySize checks happening before we get here) --------------
  [[nodiscard]] ObjRef allocInstance(ClassId cls, std::uint32_t slotHint);
  [[nodiscard]] ObjRef allocArray(ClassId arrayCls, rbc::RType elem,
                                  std::uint32_t length); // zero-filled
  [[nodiscard]] ObjRef allocString(std::string_view utf8, ClassId stringCls);

  // --- object access (defensive: id 0 / wrong kind -> nullopt-ish result) --
  [[nodiscard]] bool isInstance(ObjRef r) const noexcept;
  [[nodiscard]] bool isArray(ObjRef r) const noexcept;
  [[nodiscard]] bool isString(ObjRef r) const noexcept; // class java/lang/String
  [[nodiscard]] ClassId classOf(ObjRef r) const noexcept;
  [[nodiscard]] std::uint32_t arrayLength(ObjRef r) const noexcept; // 0 if not array

  // Instance field slot access. The caller passes the slot index within the
  // object's slot array (resolved by Runtime). Grows the object if the lazy
  // layout extended past its allocation (rare, first-touch after late
  // resolution). Returns nullptr if r is not a live instance.
  [[nodiscard]] Value loadField(ObjRef r, std::uint32_t slot);
  [[nodiscard]] bool storeField(ObjRef r, std::uint32_t slot, Value v);

  // Array element access, bounds-checked by the CALLER (the dispatch core
  // raises ArrayIndexOutOfBoundsException with the JVM's message; the heap
  // only defends against non-array access). Returns nullptr on kind misuse.
  [[nodiscard]] Value loadElem(ObjRef r, std::uint32_t index);
  [[nodiscard]] bool storeElem(ObjRef r, std::uint32_t index, Value v);

  // String payload of a java/lang/String instance ("" otherwise).
  [[nodiscard]] std::string_view stringOf(ObjRef r) const noexcept;

  // --- monitors (reentrant, single-threaded v0) -----------------------------
  // Returns false + sets *why when the operation is illegal; the caller
  // raises NullPointerException / IllegalMonitorStateException.
  enum class MonFail : std::uint8_t { None, Null, NotOwner };
  [[nodiscard]] MonFail monitorenter(ObjRef r);
  [[nodiscard]] MonFail monitorexit(ObjRef r);

  // --- interning (JVMS 5.1: equal string constants are one object) ----------
  // Returns the existing object for utf8 if one was interned before.
  [[nodiscard]] ObjRef internString(std::string_view utf8, ClassId stringCls);

  // --- diagnostics -----------------------------------------------------------
  [[nodiscard]] std::uint32_t objectCount() const noexcept { return static_cast<std::uint32_t>(objs_.size()); }

  // Direct object inspection for TESTS and the frame dumper only (cold path).
  [[nodiscard]] const HeapObj* peek(ObjRef r) const noexcept;

private:
  [[nodiscard]] HeapObj* obj(ObjRef r) noexcept;
  [[nodiscard]] const HeapObj* obj(ObjRef r) const noexcept;

  std::vector<HeapObj> objs_;                 // id = index + 1 (0 reserved)
  std::unordered_map<std::string, ObjRef> interned_;
};

} // namespace b2::interp
