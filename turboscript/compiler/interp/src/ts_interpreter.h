// TurboScript — Tier 0 interpreter (direct-threaded register machine).
// Part I Tier 0 + Rules 4, 6, 7, 8, 9, 16, 26, 41, 47, 72, 74, 83, 90, 96,
// 114, 119, 120, 124. Dispatch: computed goto (GNU) or exhaustive switch.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ts_bytecode.h"
#include "ts_core.h"
#include "ts_module.h"
#include "ts_object.h"
#include "ts_value.h"

namespace ts {

// Feedback slot (bytecode_spec.md Section 8). Saturating counters (Rule 114).
struct FeedbackSlot {
  FeedbackKind kind = FeedbackKind::Binary;
  // Property: distinct shapes seen (<= kMegamorphicThreshold), then mega.
  uint32_t shapeIds[kMegamorphicThreshold] = {0, 0, 0, 0};
  uint32_t shapeCounts[kMegamorphicThreshold] = {0, 0, 0, 0};
  uint32_t distinctShapes = 0;
  uint32_t propertyHits = 0;
  // Monomorphic IC (v0.3, bytecode_spec.md 8.1): valid when icShape is a
  // concrete shape id (shape ids start at 1; 0 = no IC installed) and the
  // receiver's shape id matches. icSlot indexes holder->slots; icAttrs is
  // the cached property's attribute byte (accessor sites never install).
  uint32_t icShape = 0;
  uint32_t icSlot = 0;
  uint8_t icAttrs = 0;
  // Shape-transition IC (v0.3, store sites): when the receiver's shape is
  // exactly icTransFrom and the key matches, the store is a fresh own
  // property whose transition result is icTransTo (verified when installed:
  // no accessor anywhere on the prototype chain claims the key). Shape
  // pointers are stable (deque-backed, ts_object.h).
  Shape* icTransFrom = nullptr;
  Shape* icTransTo = nullptr;
  uint32_t icTransKey = 0;  // SymbolId of the transition key
  // Binary: per-class histogram (left and right operands both counted).
  uint32_t classCounts[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  // Branch.
  uint32_t takenCount = 0;
  uint32_t notTakenCount = 0;
  // Call: ring of callee function indices (-1 = non-TS callee).
  int32_t callees[kCallProfileRing] = {-1, -1, -1, -1};
  uint32_t calleeCounts[kCallProfileRing] = {0, 0, 0, 0};
  uint32_t unknownCallees = 0;
  uint32_t callCount = 0;
};

// Value class buckets for binary-op histograms.
enum class ValueClass : uint8_t {
  Smi = 0,
  HeapNumber = 1,
  String = 2,
  BigInt = 3,
  Object = 4,  // includes closures
  Other = 5,   // undefined/null/bool
};

// Property descriptor (v0.3: Object.defineProperty /
// getOwnPropertyDescriptor; reused by the Proxy trap protocol). Field
// presence flags follow ES: absent fields take their default (value
// undefined; writable/enumerable/configurable false).
struct PropertyDescriptor {
  bool hasValue = false;
  bool hasWritable = false;
  bool hasEnumerable = false;
  bool hasConfigurable = false;
  bool hasGet = false;
  bool hasSet = false;
  Value value;
  bool writable = false;
  bool enumerable = false;
  bool configurable = false;
  Value getter = Value::undefined();
  Value setter = Value::undefined();
  [[nodiscard]] bool isAccessor() const { return hasGet || hasSet; }
};

// Descriptor materializer (ts_builtins.cpp): fresh plain object with the
// ES descriptor fields.
[[nodiscard]] Object* makeDescriptorObject(Isolate& iso,
                                           const PropertyDescriptor& d);

struct Frame {
  const Function* fn = nullptr;
  Context* context = nullptr;
  std::vector<Value> regs;
  uint32_t pc = 0;
  bool hasPendingException = false;
  Value pendingException;
};

// Relational comparison outcome (Abstract Relational Comparison).
enum class Relational : uint8_t { Less, Greater, Equal, Unordered };

// Host-provided native builtin (tsrun registers `print`).
using NativeFn = JsResult<Value> (*)(Isolate& isolate, Value thisVal,
                                     const Value* args, uint32_t argc);

struct NativeEntry {
  const char* name;
  NativeFn fn;
};

class Isolate {
 public:
  Isolate();

  void registerNative(const char* name, NativeFn fn);

  // Load a module: materialize constants, size feedback vectors, and resolve
  // the module's symbol ids (interned against the assembler's table) into
  // this Isolate's table (Rule 16: canonical interning per isolate).
  [[nodiscard]] TsResult<bool> loadModule(const Module& module,
                                          const SymbolTable& sourceSymbols);

  // Run the module entry function.
  [[nodiscard]] JsResult<Value> run();

  // Deopt entry (Rules 4/83): rebuild a Tier 0 frame at exactly `pc` with
  // exactly `registers` and run it.
  [[nodiscard]] JsResult<Value> enterAt(const Closure* closure, uint32_t pc,
                                        const std::vector<Value>& registers);

  // ---- Conversions that may invoke user code (Rule 67 fidelity). ----
  [[nodiscard]] JsResult<Value> toPrimitive(const Value& v, bool hintString);
  [[nodiscard]] JsResult<Value> toNumberValue(const Value& v);
  [[nodiscard]] JsResult<Value> toNumericValue(const Value& v);
  [[nodiscard]] JsResult<Value> toStringValue(const Value& v);
  [[nodiscard]] JsResult<Value> toBigIntValue(const Value& v);
  [[nodiscard]] JsResult<SymbolId> toPropertyKey(const Value& v);

  // ---- Property operations. ----
  [[nodiscard]] JsResult<Value> getProperty(const Value& recv, SymbolId key);
  [[nodiscard]] JsResult<bool> setProperty(const Value& recv, SymbolId key,
                                           const Value& val);
  [[nodiscard]] JsResult<bool> defineProperty(Object* obj, SymbolId key,
                                              const Value& val,
                                              PropertyAttrs attrs);
  [[nodiscard]] JsResult<bool> deletePropertyImpl(const Value& recv,
                                                  SymbolId key);
  [[nodiscard]] JsResult<bool> hasPropertyImpl(const Value& recv,
                                               SymbolId key);

  // ---- Element access keyed by VALUE (v0.3 fast path, Rule 96). ----
  // Identical semantics to toPropertyKey(key) + getProperty/setProperty;
  // canonical Smi keys on array-index receivers hit element storage without
  // interning a fresh key string per access.
  [[nodiscard]] JsResult<Value> getElementValue(const Value& recv,
                                                const Value& key);
  [[nodiscard]] JsResult<bool> setElementValue(const Value& recv,
                                               const Value& key,
                                               const Value& val);
  // Interned canonical decimal text of an index, cached for hot indices
  // (Rule 16: one interned symbol per distinct text; Rule 23: bounded).
  [[nodiscard]] SymbolId cachedIndexSymbol(uint32_t idx);

  // ---- Array operations (v0.2: bytecode_spec.md Section 5.12). ----
  // Exotic "length" as a Number (Smi when it fits, HeapNumber above 2^31-1).
  [[nodiscard]] Value arrayLengthValue(const Object* arr) const;
  // ArraySetLength: ToUint32/ToNumber equality check (RangeError otherwise),
  // shrink hole-ifies dense slots and drops sparse entries >= new length.
  [[nodiscard]] JsResult<bool> arraySetLength(Object* arr,
                                              const Value& newLen);
  [[nodiscard]] JsResult<bool> setArrayElement(Object* arr, uint32_t idx,
                                               const Value& val);
  // Intern the canonical decimal key text of an element index (Rule 16).
  [[nodiscard]] SymbolId internIndexKey(uint32_t idx);
  [[nodiscard]] SymbolId lengthSymbol() const { return sym_length_; }

  // ---- Builtins (v0.3: bytecode_spec.md Section 11 — prototype layer). ----
  // Standard prototype objects, wired at Isolate construction:
  //   Object.prototype  <- Function.prototype, Array.prototype
  // Fresh plain objects chain to Object.prototype; arrays to Array.prototype;
  // closures' function objects to Function.prototype (and their .prototype
  // instances to Object.prototype).
  [[nodiscard]] Object* objectPrototype() const { return objectPrototype_; }
  [[nodiscard]] Object* functionPrototype() const { return functionPrototype_; }
  [[nodiscard]] Object* arrayPrototype() const { return arrayPrototype_; }
  // Attach a native builtin as a data property of `holder` (used for the
  // prototype objects and the Object/Array global namespaces). Returns the
  // native's closure (namespaces like Symbol attach sub-natives to it).
  Closure* defineNativeOn(Object* holder, const char* name, NativeFn fn);
  // Construct-time wiring (called by the Isolate constructor).
  void installStandardPrototypes();
  // Fresh plain object with the standard prototype (NewObject/Construct).
  [[nodiscard]] Object* newPlainObject();
  // ---- Descriptors (v0.3: Object builtins; Proxy trap protocol). ----
  [[nodiscard]] JsResult<bool> definePropertyDescriptor(
      Object* obj, SymbolId key, const PropertyDescriptor& d);
  // Own-property descriptor value (a fresh descriptor object, or undefined
  // when absent). Arrays consult the exotic length/element machinery.
  [[nodiscard]] JsResult<Value> getOwnPropertyDescriptorValue(
      Object* obj, SymbolId key);
  // Own string/symbol keys as VALUES in ES order (indices ascending, then
  // named insertion order); user symbols come back as Symbol values.
  [[nodiscard]] std::vector<Value> ownKeysValues(Object* obj,
                                                 bool includeLength);

  // ---- Symbols (v0.3). ----
  // Create a fresh unique symbol; registry for Symbol.for / keyFor.
  [[nodiscard]] Value makeSymbolValue(std::u16string desc);
  [[nodiscard]] SymbolObj* symbolForRegistry(const std::u16string& key) const;
  void symbolForRegister(const std::u16string& key, SymbolObj* sym);
  // Key text (error messages): interned text for string keys, the symbol
  // description for user-symbol keys (never touches symbols_.text OOB).
  [[nodiscard]] std::u16string keyText(SymbolId key) const;
  // Re-widen an interned key id to its KEY VALUE (string or Symbol) for
  // the trap argument tuples.
  [[nodiscard]] Value keyToValue(SymbolId key);

  // ---- Proxy: the 13 internal methods (ES ch. 10.5), ts_proxy.cpp. ----
  [[nodiscard]] JsResult<Value> proxyGet(Value proxy, const Value& key);
  [[nodiscard]] JsResult<bool> proxySet(Value proxy, const Value& key,
                                        const Value& val);
  [[nodiscard]] JsResult<bool> proxyHas(Value proxy, const Value& key);
  [[nodiscard]] JsResult<bool> proxyDelete(Value proxy, const Value& key);
  [[nodiscard]] JsResult<Value> proxyGetPrototype(Value proxy);
  [[nodiscard]] JsResult<bool> proxySetPrototype(Value proxy,
                                                 const Value& proto);
  [[nodiscard]] JsResult<Value> proxyGetOwnPropertyDescriptor(
      Value proxy, const Value& key);
  [[nodiscard]] JsResult<bool> proxyDefineOwnProperty(
      Value proxy, const Value& key, const PropertyDescriptor& d);
  [[nodiscard]] JsResult<bool> proxyIsExtensible(Value proxy);
  [[nodiscard]] JsResult<bool> proxyPreventExtensions(Value proxy);
  [[nodiscard]] JsResult<std::vector<Value>> proxyOwnKeys(Value proxy);
  [[nodiscard]] JsResult<Value> proxyApply(Value proxy, Value thisVal,
                                           const Value* args, uint32_t argc);
  [[nodiscard]] JsResult<Value> proxyConstruct(Value proxy, const Value* args,
                                               uint32_t argc);
  // Trap resolution shared by the methods above (GetMethod semantics).
  [[nodiscard]] JsResult<Value> proxyTrap(Object* handler, const char* name);
  // ---- Array element helpers (public in v0.3: the builtins layer
  // (ts_builtins.cpp) composes them). ----
  [[nodiscard]] bool arrayHasOwnElement(const Object* arr, uint32_t idx) const;
  // Own element value or Hole when absent (never walks the chain).
  [[nodiscard]] Value ownElementValue(const Object* arr, uint32_t idx) const;
  // Raise the storage kind so `val` satisfies the kind invariant; creates no
  // holes (widenFor layout contract in ts_object.h).
  void widenElementsFor(Object* arr, const Value& val) const;

  // ---- Call machinery. ----
  [[nodiscard]] JsResult<Value> callValue(const Value& callee, Value thisVal,
                                          const Value* args, uint32_t argc);
  [[nodiscard]] JsResult<Value> callClosure(const Closure* closure,
                                            Value thisVal, const Value* args,
                                            uint32_t argc);

  // ---- Semantic helpers used by opcode handlers (ts_dispatch.cpp). ----
  [[nodiscard]] JsResult<Value> addValues(const Value& l, const Value& r,
                                          FeedbackSlot* fs);
  [[nodiscard]] JsResult<Value> arithValues(const Value& l, const Value& r,
                                            Opcode op);
  // Bitwise family under ToInt32/ToUint32 (Rule 72); BigInt -> TypeError.
  [[nodiscard]] JsResult<Value> bitwiseValues(const Value& l, const Value& r,
                                              Opcode op);
  [[nodiscard]] JsResult<Value> bigintArith(const Value& l, const Value& r,
                                            Opcode op);
  [[nodiscard]] JsResult<Relational> relationalCompare(const Value& l,
                                                       const Value& r);
  [[nodiscard]] JsResult<bool> abstractEquals(const Value& l, const Value& r);
  [[nodiscard]] JsResult<bool> instanceofImpl(const Value& obj,
                                              const Value& ctor);
  [[nodiscard]] JsResult<Value> constructImpl(const Value& ctor,
                                              const Value* args,
                                              uint32_t argc);
  [[nodiscard]] Closure* makeClosureFor(uint32_t funcIndex, Context* context);
  [[nodiscard]] Object* protoOfObject(Object* obj) const;

  // ---- Error factories (runtime JS errors are plain objects with
  // name/message; stack traces captured per Rule 75 diagnostics). ----
  [[nodiscard]] JsException typeError(const std::string& message);
  [[nodiscard]] JsException rangeError(const std::string& message);
  [[nodiscard]] JsException referenceError(const std::string& message);
  [[nodiscard]] JsException syntaxError(const std::string& message);

  // Human-readable rendering used by `print` (ToString semantics).
  [[nodiscard]] JsResult<std::u16string> displayString(const Value& v);
  // Uncaught-exception rendering (driver-facing): "Name: message" for error
  // objects, ToString otherwise.
  [[nodiscard]] std::u16string formatUncaught(const Value& thrown) const;

  void setCountOpcodes(bool on) { countOpcodes_ = on; }
  // Measurement toggle (benchmarks_v0.2.md Section 5 #2): disables feedback
  // recording to quantify the recording tax. Not a semantic mode (Rule 124:
  // deterministic either way); driver flag `--no-record`.
  void setRecordFeedback(bool on) { recordFeedback_ = on; }

  [[nodiscard]] SymbolTable& symbols() { return symbols_; }
  // Access the current top frame's function index (used by error paths that
  // must attribute diagnostics; dispatch uses resolved pointers instead).
  [[nodiscard]] bool recordFeedbackEnabled() const { return recordFeedback_; }
  [[nodiscard]] Heap& heap() { return heap_; }
  [[nodiscard]] ShapeTree& shapes() { return shapes_; }
  [[nodiscard]] Object* globalObject() { return global_; }
  [[nodiscard]] const Module* module() const { return module_; }
  [[nodiscard]] const std::vector<Value>& constants() const {
    return constants_;
  }
  [[nodiscard]] const std::vector<NativeEntry>& natives() const {
    return natives_;
  }
  [[nodiscard]] static constexpr uint32_t nativeBase() {
    return kNativeFuncIndexBase;
  }
  [[nodiscard]] const std::vector<FeedbackSlot>& feedbackFor(
      const Function* fn) const {
    return feedback_[fn->index];
  }
  [[nodiscard]] const std::vector<uint64_t>& opcodeCounts() const {
    return opcodeCounts_;
  }
  [[nodiscard]] std::vector<std::string> currentTrace() const;

  static constexpr uint32_t kNativeFuncIndexBase = 0x80000000u;

 private:
  [[nodiscard]] JsResult<Value> runFrame(Frame& frame);  // ts_dispatch.cpp
  [[nodiscard]] JsException makeError(const char* name,
                                      const std::string& message);

  [[nodiscard]] JsResult<Value> getAlongChain(Object* start,
                                              const Value& recv,
                                              SymbolId key, bool isIndex,
                                              uint32_t idx);
  [[nodiscard]] JsResult<bool> setAlongChain(Object* start,
                                             const Value& recv, SymbolId key,
                                             const Value& val);
  [[nodiscard]] bool hasAlongChain(Object* start, SymbolId key, bool isIndex,
                                   uint32_t idx);

  // Feedback recording takes the resolved slot pointer directly (v0.3: the
  // dispatch handlers hold `fb` + slotOfPc; re-deriving frameStack_.back()
  // per recorded op is measurable overhead).
  static void recordPropertySite(FeedbackSlot* s, const Value& recv);
  static void recordElementSite(FeedbackSlot* s, const Value& recv);
  static void recordBinarySite(FeedbackSlot* s, const Value& l,
                               const Value& r);
  static void recordBranchSite(FeedbackSlot* s, bool taken);
  static void recordCallSite(FeedbackSlot* s, const Value& callee);
  // Monomorphic IC installation (v0.3). Installs only when the property is
  // an own DATA property of `obj` with a published shape; accessors, holes
  // and shapeless fresh objects never install (conservative).
  static void installPropertyIc(FeedbackSlot* s, Object* obj, SymbolId key);
  // True when a prototype-chain node of `obj` exposes an ACCESSOR named
  // `key` (data properties do not block define-fresh-own; accessors do).
  [[nodiscard]] bool chainHasAccessor(Object* obj, SymbolId key) const;

  SymbolTable symbols_;
  Heap heap_;
  ShapeTree shapes_;
  Object* global_ = nullptr;
  Object* objectPrototype_ = nullptr;
  Object* functionPrototype_ = nullptr;
  Object* arrayPrototype_ = nullptr;
  std::vector<NativeEntry> natives_;
  std::vector<Closure*> nativeClosures_;
  // Module global names resolved into `symbols_` at loadModule time.
  std::vector<SymbolId> globalNames_;
  // Function names likewise resolved (index-aligned with the function table).
  std::vector<SymbolId> fnNames_;

  // Interned well-known keys (Rule 16).
  SymbolId sym_prototype_ = kInvalidSymbol;
  SymbolId sym_constructor_ = kInvalidSymbol;
  SymbolId sym_name_ = kInvalidSymbol;
  SymbolId sym_message_ = kInvalidSymbol;
  SymbolId sym_valueOf_ = kInvalidSymbol;
  SymbolId sym_toString_ = kInvalidSymbol;
  SymbolId sym_length_ = kInvalidSymbol;

  const Module* module_ = nullptr;
  std::vector<Value> constants_;  // materialized
  std::vector<std::vector<FeedbackSlot>> feedback_;
  std::vector<uint64_t> opcodeCounts_;
  bool countOpcodes_ = false;
  bool recordFeedback_ = true;
  // Cache of interned decimal index-key symbols (index == value).
  std::vector<SymbolId> indexSymbolCache_;
  // User symbols: index == uniqueId; registry for Symbol.for (Rule 124:
  // insertion-ordered vector, unordered_map only for registry lookup).
  std::vector<SymbolObj*> userSymbols_;
  std::unordered_map<std::u16string, SymbolObj*> symbolRegistry_;

  std::vector<Frame*> frameStack_;  // for stack traces (Rule 75)
  uint32_t callDepth_ = 0;
  // v0.4 (benchmarks_v0.3.md Section 5 #2): freed frame register files,
  // reused LIFO by callClosure to avoid a heap allocation per call.
  std::vector<std::vector<Value>> regPool_;

  // Stack traces for error objects (diagnostics; not JS-visible in v0.1).
  std::unordered_map<const Object*, std::vector<std::string>> stackTraces_;

  friend class DispatchAccess;  // runFrame implementation file
};

[[nodiscard]] std::string joinTrace(const std::vector<std::string>& frames);

// UTF-16 -> UTF-8 (exported for driver output).
[[nodiscard]] std::string utf16ToUtf8(const std::u16string& s);

}  // namespace ts
