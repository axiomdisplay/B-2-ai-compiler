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

  // ---- Call machinery. ----
  [[nodiscard]] JsResult<Value> callValue(const Value& callee, Value thisVal,
                                          const Value* args, uint32_t argc);
  [[nodiscard]] JsResult<Value> callClosure(const Closure* closure,
                                            Value thisVal, const Value* args,
                                            uint32_t argc);

  // ---- Semantic helpers used by opcode handlers (ts_dispatch.cpp). ----
  [[nodiscard]] JsResult<Value> addValues(const Value& l, const Value& r,
                                          uint32_t slot);
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

  [[nodiscard]] SymbolTable& symbols() { return symbols_; }
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

  void recordPropertySite(uint32_t slot, const Value& recv);
  void recordBinarySite(uint32_t slot, const Value& l, const Value& r);
  void recordBranchSite(uint32_t slot, bool taken);
  void recordCallSite(uint32_t slot, const Value& callee);

  SymbolTable symbols_;
  Heap heap_;
  ShapeTree shapes_;
  Object* global_ = nullptr;
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

  const Module* module_ = nullptr;
  std::vector<Value> constants_;  // materialized
  std::vector<std::vector<FeedbackSlot>> feedback_;
  std::vector<uint64_t> opcodeCounts_;
  bool countOpcodes_ = false;

  std::vector<Frame*> frameStack_;  // for stack traces (Rule 75)
  uint32_t callDepth_ = 0;

  // Stack traces for error objects (diagnostics; not JS-visible in v0.1).
  std::unordered_map<const Object*, std::vector<std::string>> stackTraces_;

  friend class DispatchAccess;  // runFrame implementation file
};

[[nodiscard]] std::string joinTrace(const std::vector<std::string>& frames);

// UTF-16 -> UTF-8 (exported for driver output).
[[nodiscard]] std::string utf16ToUtf8(const std::u16string& s);

}  // namespace ts
