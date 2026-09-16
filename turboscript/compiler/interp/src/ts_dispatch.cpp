// TurboScript — Tier 0 dispatch loop (ts_dispatch.cpp).
// Direct-threaded computed-goto dispatch (user directive; Part I Tier 0)
// with an exhaustive switch fallback selected by TS_NO_COMPUTED_GOTO.
// Both paths share the same handler bodies and are corpus-tested.

// &&label / goto* are a deliberate GNU extension (interp_contract.md §1).
// Pedantic noise from this translation unit only is suppressed.
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#include "ts_interpreter.h"

#include <cmath>
#include <cstring>

#if defined(__GNUC__) && !defined(TS_NO_COMPUTED_GOTO)
#define TS_COMPUTED_GOTO 1
#else
#define TS_COMPUTED_GOTO 0
#endif

namespace ts {

// TS_RAISE accepts either a Value or a JsException (carries .thrown).
inline Value excValue(const Value& v) { return v; }
inline Value excValue(const JsException& e) { return e.thrown; }

// ---------------------------------------------------------------------------
// v0.4 fast-path helpers (benchmarks_v0.3.md Section 5 #1/#3). The guards are
// conservative: every input the fast path does not claim falls through to the
// unchanged semantic helper (Rule 96: one semantic source — a fast path is a
// guard in front of it, never a replacement).
// ---------------------------------------------------------------------------
inline bool tsBothSmi(const Value& l, const Value& r) {
  return l.isSmi() && r.isSmi();
}

// Inline Smi normalization for fast paths: same contract as normalizeNumber
// (integral, in Smi range, never -0 — Rule 72), without the call.
inline Value tsSmiOrNumber(double v) {
  if (v >= static_cast<double>(kSmiMin) && v <= static_cast<double>(kSmiMax)) {
    if (static_cast<double>(static_cast<int32_t>(v)) == v &&
        !(v == 0.0 && std::signbit(v))) {
      return Value::smi(static_cast<int32_t>(v));
    }
  }
  return Value::heapNumber(v);
}

// Inline ToBoolean fast lane for branch handlers; pure::toBoolean remains the
// semantic source for every kind the fast lane does not cover.
inline bool tsQuickTruthy(const Value& v) {
  switch (v.kind()) {
    case ValueKind::Boolean:
      return v.asBool();
    case ValueKind::Smi:
      return v.asSmi() != 0;
    case ValueKind::Undefined:
    case ValueKind::Null:
      return false;
    default:
      return pure::toBoolean(v);
  }
}

// String concatenation backend for the L_StringConcat fast lane: one exact
// allocation, both operands appended (kept out of the handler so handler
// scopes stay trivially destructible across computed-goto targets).
inline StringObj* tsConcatStrings(const StringObj* l, const StringObj* r,
                                  Heap& heap) {
  std::u16string out;
  out.reserve(l->data.size() + r->data.size());
  out.append(l->data);
  out.append(r->data);
  return heap.makeString(std::move(out));
}

JsResult<Value> Isolate::enterAt(const Closure* closure, uint32_t pc,
                                 const std::vector<Value>& registers) {
  // Rule 105: untrusted entry inputs are validated before execution.
  if (module_ == nullptr || closure == nullptr ||
      closure->funcIndex >= module_->functions.size()) {
    return std::unexpected(
        typeError("enterAt: no module loaded or invalid closure"));
  }
  const Function& fn = *module_->functions[closure->funcIndex];
  if (pc >= fn.code.size() || !instructionStarts(fn)[pc]) {
    return std::unexpected(typeError(
        "enterAt: pc " + std::to_string(pc) +
        " is not an instruction boundary (expected: word index of an "
        "instruction start)"));
  }
  if (registers.size() != fn.registerCount) {
    return std::unexpected(typeError(
        "enterAt: register file size mismatch (expected " +
        std::to_string(fn.registerCount) + ", got " +
        std::to_string(registers.size()) + ")"));
  }
  if (callDepth_ >= kMaxCallDepth) {
    return std::unexpected(rangeError("Maximum call stack size exceeded"));
  }
  Frame frame;
  frame.fn = &fn;
  frame.context = closure->context;
  frame.regs = registers;
  frame.pc = pc;
  frameStack_.push_back(&frame);
  callDepth_++;
  JsResult<Value> result = runFrame(frame);
  callDepth_--;
  frameStack_.pop_back();
  return result;
}

JsResult<Value> Isolate::runFrame(Frame& frame) {
  const Function* fn = frame.fn;
  const uint32_t* code = fn->code.data();
  // In-bounds access is guaranteed by the verifier (Rule 105): no codeLen
  // fence on the hot path (Rule 7).
  Value* regs = frame.regs.data();
  const uint32_t fnIndex = fn->index;
  // v0.4: pc -> slot map hoisted to a raw pointer (one dependent load per
  // recorded op instead of two). slotMap is non-null exactly when fb is;
  // both derive from the same verifier layout (1:1 with slotOfPc).
  FeedbackSlot* fb =
      (recordFeedback_ && !feedback_[fnIndex].empty() &&
       !fn->slotOfPc.empty())
          ? feedback_[fnIndex].data()
          : nullptr;
  const uint32_t* slotMap =
      fb != nullptr ? fn->slotOfPc.data() : nullptr;
  const Value* consts = constants_.data();
  const uint64_t codeHashIgnored = 0;  // (Rule 124: no hidden state)
  (void)codeHashIgnored;

  uint32_t pc = frame.pc;
  uint32_t w = 0;
  uint32_t faultPc = 0;
  Value excVal;

#if TS_COMPUTED_GOTO
  // Build the dispatch table once (single-threaded T0, Rule 119).
  static const void* kDispatch[kOpcodeSpace];
  static bool kDispatchReady = false;
  if (!kDispatchReady) {
    const void** t = kDispatch;
    (void)t;
#define TS_OP(name, fmt, effect, ic, roles_expr) \
    kDispatch[static_cast<uint32_t>(Opcode::k##name)] = &&L_##name;
#include "ts_opcodes.inc"
#undef TS_OP
    kDispatchReady = true;
  }
#define TS_DISPATCH()               \
  do {                              \
    w = code[pc];                   \
    uint32_t opb = w & 0xFF;        \
    if (countOpcodes_) {            \
      opcodeCounts_[opb]++;         \
    }                               \
    goto *kDispatch[opb];           \
  } while (0)
#else
#define TS_DISPATCH()               \
  do {                              \
    w = code[pc];                   \
    uint32_t opb = w & 0xFF;        \
    if (countOpcodes_) {            \
      opcodeCounts_[opb]++;         \
    }                               \
    goto dispatchSwitch;            \
  } while (0)
#endif

  // v0.4: static format table (opcode byte -> word width), built once like
  // the dispatch table. Replaces the per-instruction opcodeInfo() switch in
  // TS_FIELDS (benchmarks_v0.3.md Section 5: dispatch overhead).
  static uint8_t kFmtWords[kOpcodeSpace] = {0};
  static bool kFmtWordsReady = false;
  if (!kFmtWordsReady) {
#define TS_OP(name, fmt, effect, ic, roles_expr)                        \
    kFmtWords[static_cast<uint32_t>(Opcode::k##name)] =                 \
        static_cast<uint8_t>(formatWordCount(OpFormat::fmt));
#include "ts_opcodes.inc"
#undef TS_OP
    kFmtWordsReady = true;
  }

  // Field decoders for the current word w (and suffix s).
  uint32_t s = 0;
#define TS_FIELDS()                          \
  do {                                       \
    a_ = (w >> 8) & 0xFF;                    \
    b_ = (w >> 16) & 0xFF;                   \
    if (kFmtWords[w & 0xFF] == 2) {          \
      s = code[pc + 1];                      \
      c_ = s & 0xFF;                         \
      d_ = (s >> 8) & 0xFF;                  \
      e_ = (s >> 16) & 0xFF;                 \
    }                                        \
  } while (0)
  uint32_t a_ = 0, b_ = 0, c_ = 0, d_ = 0, e_ = 0;

  // Exception path macros (Rule 74: JS exceptions are values in flight).
#define TS_RAISE(v)                          \
  do {                                       \
    excVal = excValue(v);                    \
    faultPc = pc;                            \
    goto raise_in_frame;                     \
  } while (0)
#define TS_TAKE(expr)                                  \
  do {                                                 \
    auto _r = (expr);                                  \
    if (!_r) {                                         \
      excVal = _r.error().thrown;                      \
      faultPc = pc;                                    \
      goto raise_in_frame;                             \
    }                                                  \
  } while (0)
#define TS_GET(expr, out)                              \
  do {                                                 \
    auto _r = (expr);                                  \
    if (!_r) {                                         \
      excVal = _r.error().thrown;                      \
      faultPc = pc;                                    \
      goto raise_in_frame;                             \
    }                                                  \
    out = std::move(*_r);                              \
  } while (0)

  TS_DISPATCH();

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
L_Nop : {
  pc += 1;
  TS_DISPATCH();
}
L_Mov : {
  TS_FIELDS();
  regs[a_] = regs[b_];
  pc += 1;
  TS_DISPATCH();
}
L_LoadConst : {
  TS_FIELDS();
  regs[a_] = consts[c_ | (d_ << 8) | (e_ << 16)];
  pc += 2;
  TS_DISPATCH();
}
L_LoadConstS : {
  TS_FIELDS();
  regs[a_] = Value::smi(static_cast<int8_t>(b_));
  pc += 1;
  TS_DISPATCH();
}
L_LoadUndefined : {
  TS_FIELDS();
  regs[a_] = Value::undefined();
  pc += 1;
  TS_DISPATCH();
}
L_LoadNull : {
  TS_FIELDS();
  regs[a_] = Value::null();
  pc += 1;
  TS_DISPATCH();
}
L_LoadTrue : {
  TS_FIELDS();
  regs[a_] = Value::boolean(true);
  pc += 1;
  TS_DISPATCH();
}
L_LoadFalse : {
  TS_FIELDS();
  regs[a_] = Value::boolean(false);
  pc += 1;
  TS_DISPATCH();
}
L_DefineGlobalVar : {
  TS_FIELDS();
  {
    SymbolId nameSym = globalNames_[c_ | (d_ << 8) | (e_ << 16)];
    LookupResult lr = lookupProperty(global_, nameSym);
    if (!lr.found) {
      TS_TAKE(defineProperty(global_, nameSym, Value::undefined(),
                             kDefaultDataAttrs));
    }
  }
  pc += 2;
  TS_DISPATCH();
}
L_LoadGlobal : {
  TS_FIELDS();
  {
    SymbolId nameSym = globalNames_[c_ | (d_ << 8) | (e_ << 16)];
    FeedbackSlot* fs = fb != nullptr ? &fb[slotMap[pc]] : nullptr;
    // Monomorphic global IC (v0.3): own data property of the global object
    // with a stable shape. Same guard/invariants as the property ICs.
    if (fs != nullptr) {
      Shape* gshape = global_->shape;
      if (gshape != nullptr && fs->icShape == gshape->id &&
          (fs->icAttrs & static_cast<uint8_t>(PropAttr::IsAccessor)) == 0) {
        Value& cached = global_->slots[fs->icSlot];
        if (!cached.isHole()) {
          regs[a_] = cached;
          pc += 2;
          TS_DISPATCH();
        }
      }
      TS_GET(getProperty(Value::raw(ValueKind::Object, global_), nameSym),
             regs[a_]);
      installPropertyIc(fs, global_, nameSym);
    } else {
      TS_GET(getProperty(Value::raw(ValueKind::Object, global_), nameSym),
             regs[a_]);
    }
  }
  pc += 2;
  TS_DISPATCH();
}
L_StoreGlobal : {
  TS_FIELDS();
  {
    SymbolId nameSym = globalNames_[c_ | (d_ << 8) | (e_ << 16)];
    FeedbackSlot* fs = fb != nullptr ? &fb[slotMap[pc]] : nullptr;
    Value recv = Value::raw(ValueKind::Object, global_);
    if (fs != nullptr) {
      Shape* gshape = global_->shape;
      if (gshape != nullptr && fs->icShape == gshape->id &&
          (fs->icAttrs & static_cast<uint8_t>(PropAttr::IsAccessor)) == 0) {
        Value& cached = global_->slots[fs->icSlot];
        if (!cached.isHole()) {
          cached = regs[a_];
          pc += 2;
          TS_DISPATCH();
        }
      }
      TS_TAKE(setProperty(recv, nameSym, regs[a_]));
      installPropertyIc(fs, global_, nameSym);
    } else {
      TS_TAKE(setProperty(recv, nameSym, regs[a_]));
    }
  }
  pc += 2;
  TS_DISPATCH();
}
L_GetProperty : {
  TS_FIELDS();
  {
    // W2_RR_D layout: a_=dst, b_=receiver, c_=key.
    Value recv = regs[b_];
    FeedbackSlot* fs = fb != nullptr ? &fb[slotMap[pc]] : nullptr;
    SymbolId keySym;
    const Value& kv = regs[c_];
    // v0.4: const-pool strings cache their interned key id after first use
    // (v0.3) — the hot lane reads it inline instead of calling toPropertyKey.
    if (kv.isString() && kv.asString()->cachedSymbol != kInvalidSymbol) {
      keySym = kv.asString()->cachedSymbol;
    } else {
      TS_GET(toPropertyKey(kv), keySym);
    }
    recordPropertySite(fs, recv);
    // Monomorphic IC (v0.3, bytecode_spec.md 8.1): own data property on a
    // stable shape. Guard: shape match, non-accessor, non-hole slot.
    if (fs != nullptr && recv.isObjectLike()) {
      Object* obj = objectOfValue(recv);
      if (obj->shape != nullptr && fs->icShape == obj->shape->id &&
          (fs->icAttrs & static_cast<uint8_t>(PropAttr::IsAccessor)) == 0) {
        Value& cached = obj->slots[fs->icSlot];
        if (!cached.isHole()) {
          regs[a_] = cached;
          pc += 2;
          TS_DISPATCH();
        }
      }
    }
    TS_GET(getProperty(recv, keySym), regs[a_]);
    installPropertyIc(fs, recv.isObjectLike() ? objectOfValue(recv) : nullptr,
                      keySym);
  }
  pc += 2;
  TS_DISPATCH();
}
L_SetProperty : {
  TS_FIELDS();
  {
    Value recv = regs[a_];
    FeedbackSlot* fs = fb != nullptr ? &fb[slotMap[pc]] : nullptr;
    SymbolId keySym;
    const Value& kv = regs[b_];
    // v0.4: cached interned-key fast lane (same as GetProperty's).
    if (kv.isString() && kv.asString()->cachedSymbol != kInvalidSymbol) {
      keySym = kv.asString()->cachedSymbol;
    } else {
      TS_GET(toPropertyKey(kv), keySym);
    }
    recordPropertySite(fs, recv);
    if (recv.isObjectLike()) {
      Object* obj = objectOfValue(recv);
      // Store IC (v0.3): receiver-own data property (matches setAlongChain's
      // own-data fast write; inherited accessors keep the slow path).
      if (fs != nullptr && obj->shape != nullptr &&
          fs->icShape == obj->shape->id &&
          (fs->icAttrs & static_cast<uint8_t>(PropAttr::IsAccessor)) == 0) {
        Value& cached = obj->slots[fs->icSlot];
        if (!cached.isHole()) {
          cached = regs[c_];
          pc += 2;
          TS_DISPATCH();
        }
      }
      // Transition IC (v0.3): fresh own property on a known prior shape.
      // Guards re-verified per hit (Rule 81: proto mutation must not be
      // observed through a stale IC): extensibility + no chain accessor.
      if (fs != nullptr && fs->icTransTo != nullptr &&
          obj->shape == fs->icTransFrom && keySym == fs->icTransKey &&
          obj->extensible && !chainHasAccessor(obj, keySym)) {
        obj->shape = fs->icTransTo;
        obj->slots.push_back(regs[c_]);
        pc += 2;
        TS_DISPATCH();
      }
    }
    Shape* beforeShape =
        recv.isObjectLike() ? objectOfValue(recv)->shape : nullptr;
    TS_TAKE(setProperty(recv, keySym, regs[c_]));
    if (fs != nullptr && recv.isObjectLike()) {
      Object* obj = objectOfValue(recv);
      installPropertyIc(fs, obj, keySym);
      // Transition install: the slow path performed exactly one shape
      // transition from the receiver's prior shape (its occurrence already
      // proves no chain accessor intercepted the store).
      if (obj->shape != nullptr && obj->shape->parent == beforeShape &&
          obj->shape != beforeShape) {
        fs->icTransFrom = beforeShape;
        fs->icTransTo = obj->shape;
        fs->icTransKey = keySym;
      }
    }
  }
  pc += 2;
  TS_DISPATCH();
}
L_DeleteProperty : {
  TS_FIELDS();
  {
    SymbolId keySym;
    TS_GET(toPropertyKey(regs[c_]), keySym);
    bool ok = false;
    TS_GET(deletePropertyImpl(regs[b_], keySym), ok);
    regs[a_] = Value::boolean(ok);
  }
  pc += 2;
  TS_DISPATCH();
}
L_HasProperty : {
  TS_FIELDS();
  {
    SymbolId keySym;
    TS_GET(toPropertyKey(regs[c_]), keySym);
    bool ok = false;
    TS_GET(hasPropertyImpl(regs[b_], keySym), ok);
    regs[a_] = Value::boolean(ok);
  }
  pc += 2;
  TS_DISPATCH();
}
L_GetPrototype : {
  TS_FIELDS();
  {
    if (regs[b_].isProxy()) {
      // Proxy [[GetPrototypeOf]] (v0.3).
      TS_GET(proxyGetPrototype(regs[b_]), regs[a_]);
    } else if (!regs[b_].isObjectLike()) {
      regs[a_] = Value::undefined();
    } else {
      regs[a_] = objectOfValue(regs[b_])->proto;
    }
  }
  pc += 1;
  TS_DISPATCH();
}
L_SetPrototype : {
  TS_FIELDS();
  {
    Value obj = regs[b_];
    Value proto = regs[c_];
    if (!obj.isObjectLike() && !obj.isProxy()) {
      regs[a_] = Value::boolean(false);
    } else if (!proto.isObjectLike() && !proto.isNull()) {
      regs[a_] = Value::boolean(false);
    } else if (obj.isProxy()) {
      // Proxy [[SetPrototypeOf]] (v0.3).
      bool ok = false;
      TS_GET(proxySetPrototype(obj, proto), ok);
      regs[a_] = Value::boolean(ok);
    } else {
      Object* objPtr = objectOfValue(obj);
      // [[SetPrototypeOf]]: reject cycles.
      Object* walk = proto.isObjectLike() ? objectOfValue(proto) : nullptr;
      bool cycle = false;
      while (walk != nullptr) {
        if (walk == objPtr) {
          cycle = true;
          break;
        }
        walk = protoOfObject(walk);
      }
      if (cycle) {
        regs[a_] = Value::boolean(false);
      } else {
        objPtr->proto = proto;
        regs[a_] = Value::boolean(true);
      }
    }
  }
  pc += 2;
  TS_DISPATCH();
}
L_Instanceof : {
  TS_FIELDS();
  {
    bool ok = false;
    TS_GET(instanceofImpl(regs[b_], regs[c_]), ok);
    regs[a_] = Value::boolean(ok);
  }
  pc += 2;
  TS_DISPATCH();
}
L_In : {
  TS_FIELDS();
  {
    SymbolId keySym;
    TS_GET(toPropertyKey(regs[c_]), keySym);
    bool ok = false;
    TS_GET(hasPropertyImpl(regs[b_], keySym), ok);
    regs[a_] = Value::boolean(ok);
  }
  pc += 2;
  TS_DISPATCH();
}
L_Call : {
  TS_FIELDS();
  {
    if (fb != nullptr) recordCallSite(&fb[slotMap[pc]], regs[a_]);
    // v0.4: direct closure hop — callValue's proxy/native routing only for
    // non-closure callees (identical semantics, one frame less of C++ calls).
    if (regs[a_].kind() == ValueKind::Closure &&
        static_cast<const Closure*>(regs[a_].asPtr())->funcIndex <
            kNativeFuncIndexBase) {
      TS_GET(callClosure(static_cast<const Closure*>(regs[a_].asPtr()),
                         Value::undefined(), &regs[b_], c_),
             regs[e_]);
    } else {
      TS_GET(callValue(regs[a_], Value::undefined(), &regs[b_], c_),
             regs[e_]);
    }
  }
  pc += 2;
  TS_DISPATCH();
}
L_CallMethod : {
  TS_FIELDS();
  {
    if (fb != nullptr) recordCallSite(&fb[slotMap[pc]], regs[a_]);
    if (regs[a_].kind() == ValueKind::Closure &&
        static_cast<const Closure*>(regs[a_].asPtr())->funcIndex <
            kNativeFuncIndexBase) {
      TS_GET(callClosure(static_cast<const Closure*>(regs[a_].asPtr()),
                         regs[d_], &regs[b_], c_),
             regs[e_]);
    } else {
      TS_GET(callValue(regs[a_], regs[d_], &regs[b_], c_), regs[e_]);
    }
  }
  pc += 2;
  TS_DISPATCH();
}
L_Construct : {
  TS_FIELDS();
  {
    if (fb != nullptr) recordCallSite(&fb[slotMap[pc]], regs[a_]);
    TS_GET(constructImpl(regs[a_], &regs[b_], c_), regs[e_]);
  }
  pc += 2;
  TS_DISPATCH();
}
L_Return : {
  TS_FIELDS();
  frame.pc = pc;
  return regs[a_];
}
L_NewClosure : {
  TS_FIELDS();
  {
    Closure* cl = makeClosureFor(c_ | (d_ << 8) | (e_ << 16), frame.context);
    if (cl == nullptr) {
      TS_RAISE(typeError("internal: closure function index out of range"));
    }
    regs[a_] = Value::raw(ValueKind::Closure, cl);
  }
  pc += 2;
  TS_DISPATCH();
}
L_NewContext : {
  TS_FIELDS();
  {
    Context* parent = nullptr;
    if (regs[b_].isContext()) {
      parent = static_cast<Context*>(regs[b_].asPtr());
    } else if (!regs[b_].isNull()) {
      TS_RAISE(typeError("NewContext parent must be a context or null"));
    }
    Context* ctx = heap_.makeContext(parent, c_);
    regs[a_] = Value::raw(ValueKind::Context, ctx);
    frame.context = ctx;  // documented second effect (bytecode_spec 5.4)
  }
  pc += 2;
  TS_DISPATCH();
}
L_LoadContext : {
  TS_FIELDS();
  {
    if (!regs[b_].isContext()) {
      TS_RAISE(typeError("LoadContext operand is not a context"));
    }
    Context* ctx = static_cast<Context*>(regs[b_].asPtr());
    if (c_ >= ctx->cells.size()) {
      TS_RAISE(referenceError("Context cell index out of range"));
    }
    Value v = ctx->cells[c_];
    if (v.isHole()) {
      TS_RAISE(referenceError(
          "Cannot access context cell before initialization"));
    }
    regs[a_] = v;
  }
  pc += 2;
  TS_DISPATCH();
}
L_StoreContext : {
  TS_FIELDS();
  {
    if (!regs[a_].isContext()) {
      TS_RAISE(typeError("StoreContext operand is not a context"));
    }
    Context* ctx = static_cast<Context*>(regs[a_].asPtr());
    if (b_ >= ctx->cells.size()) {
      TS_RAISE(referenceError("Context cell index out of range"));
    }
    ctx->cells[b_] = regs[c_];
  }
  pc += 2;
  TS_DISPATCH();
}
L_NewObject : {
  TS_FIELDS();
  {
    // v0.3: plain object chained to Object.prototype (builtins layer,
    // bytecode_spec 11); shape assignment happens on first store.
    Object* obj = newPlainObject();
    regs[a_] = Value::raw(ValueKind::Object, obj);
  }
  pc += 1;
  TS_DISPATCH();
}
L_NewArray : {
  TS_FIELDS();
  {
    // v0.3 array: length 0, PackedSmi, no elements, chained to
    // Array.prototype (builtins layer, bytecode_spec 11).
    Object* arr = heap_.makeObject();
    arr->isArray = true;
    arr->proto = Value::raw(ValueKind::Object, arrayPrototype_);
    regs[a_] = Value::raw(ValueKind::Object, arr);
  }
  pc += 1;
  TS_DISPATCH();
}
L_GetElement : {
  TS_FIELDS();
  {
    // W2_RR_D layout: a_=dst, b_=receiver, c_=key. Semantics are exactly
    // GetProperty's (Rule 96: one semantic source — getElementValue); only
    // the feedback class differs (Element-kind sites, bytecode_spec 8).
    // v0.4 in-handler element fast path (benchmarks_v0.3.md Section 5 #3):
    // Smi key on an array receiver with dense storage — bounds-checked
    // direct read; holes and every other shape of input fall through to
    // getElementValue unchanged.
    Value recv = regs[b_];
    FeedbackSlot* fs = fb != nullptr ? &fb[slotMap[pc]] : nullptr;
    recordElementSite(fs, recv);
    const Value& key = regs[c_];
    if (recv.kind() == ValueKind::Object && key.isSmi() && key.asSmi() >= 0) {
      Object* obj = static_cast<Object*>(recv.asPtr());
      if (obj->isArray) {
        uint32_t idx = static_cast<uint32_t>(key.asSmi());
        if (idx < obj->elements.size()) {
          Value v = obj->elements[idx];
          if (!v.isHole()) {
            regs[a_] = v;
            pc += 2;
            TS_DISPATCH();
          }
        }
      }
    }
    TS_GET(getElementValue(recv, key), regs[a_]);
  }
  pc += 2;
  TS_DISPATCH();
}
L_SetElement : {
  TS_FIELDS();
  {
    // W2_RR_V layout: a_=obj, b_=key, c_=val. v0.4 fast path: in-range
    // dense store over a non-hole slot on an array receiver — the exact
    // store setArrayElement performs (widen + store + length update), with
    // no user code reachable (array element slots carry no accessors).
    Value recv = regs[a_];
    FeedbackSlot* fs = fb != nullptr ? &fb[slotMap[pc]] : nullptr;
    recordElementSite(fs, recv);
    const Value& key = regs[b_];
    if (recv.kind() == ValueKind::Object && key.isSmi() && key.asSmi() >= 0) {
      Object* obj = static_cast<Object*>(recv.asPtr());
      if (obj->isArray) {
        uint32_t idx = static_cast<uint32_t>(key.asSmi());
        if (idx < obj->elements.size() && !obj->elements[idx].isHole()) {
          widenElementsFor(obj, regs[c_]);
          obj->elements[idx] = regs[c_];
          if (obj->length <= idx) obj->length = idx + 1;
          pc += 2;
          TS_DISPATCH();
        }
      }
    }
    TS_TAKE(setElementValue(recv, key, regs[c_]));
  }
  pc += 2;
  TS_DISPATCH();
}
L_GetContext : {
  TS_FIELDS();
  {
    // The frame's current context (initialized from closure->context at
    // callClosure). Null when the closure captured nothing.
    regs[a_] = frame.context != nullptr
                   ? Value::raw(ValueKind::Context, frame.context)
                   : Value::null();
  }
  pc += 1;
  TS_DISPATCH();
}
L_Add : {
  TS_FIELDS();
  {
    FeedbackSlot* fs = fb != nullptr ? &fb[slotMap[pc]] : nullptr;
    if (fs != nullptr) recordBinarySite(fs, regs[a_], regs[b_]);
    // v0.4 Smi fast path (benchmarks_v0.3.md Section 5 #1): Smi+Smi with an
    // int64 range guard. Everything else — overflow, doubles, strings,
    // BigInt, user hooks — falls through to addValues unchanged (Rule 96:
    // the fast path is a guard in front of the semantic source).
    if (tsBothSmi(regs[a_], regs[b_])) {
      int64_t sum = static_cast<int64_t>(regs[a_].asSmi()) + regs[b_].asSmi();
      if (sum >= kSmiMin && sum <= kSmiMax) {
        regs[a_] = Value::smi(static_cast<int32_t>(sum));
        pc += 1;
        TS_DISPATCH();
      }
    }
    // v0.4 number-pair lane: two numbers add as IEEE doubles (the exact
    // computation addValues performs after identity conversions).
    if (regs[a_].isNumber() && regs[b_].isNumber()) {
      regs[a_] = tsSmiOrNumber(regs[a_].asDouble() + regs[b_].asDouble());
      pc += 1;
      TS_DISPATCH();
    }
    TS_GET(addValues(regs[a_], regs[b_], nullptr), regs[a_]);
  }
  pc += 1;
  TS_DISPATCH();
}
L_Sub : {
  TS_FIELDS();
  {
    if (tsBothSmi(regs[a_], regs[b_])) {
      int64_t diff = static_cast<int64_t>(regs[a_].asSmi()) - regs[b_].asSmi();
      if (diff >= kSmiMin && diff <= kSmiMax) {
        regs[a_] = Value::smi(static_cast<int32_t>(diff));
        pc += 1;
        TS_DISPATCH();
      }
    }
    if (regs[a_].isNumber() && regs[b_].isNumber()) {
      regs[a_] = tsSmiOrNumber(regs[a_].asDouble() - regs[b_].asDouble());
      pc += 1;
      TS_DISPATCH();
    }
    TS_GET(arithValues(regs[a_], regs[b_], Opcode::kSub), regs[a_]);
  }
  pc += 1;
  TS_DISPATCH();
}
L_Mul : {
  TS_FIELDS();
  {
    if (tsBothSmi(regs[a_], regs[b_])) {
      int64_t prod = static_cast<int64_t>(regs[a_].asSmi()) *
                     static_cast<int64_t>(regs[b_].asSmi());
      if (prod >= kSmiMin && prod <= kSmiMax) {
        regs[a_] = Value::smi(static_cast<int32_t>(prod));
        pc += 1;
        TS_DISPATCH();
      }
    }
    if (regs[a_].isNumber() && regs[b_].isNumber()) {
      regs[a_] = tsSmiOrNumber(regs[a_].asDouble() * regs[b_].asDouble());
      pc += 1;
      TS_DISPATCH();
    }
    TS_GET(arithValues(regs[a_], regs[b_], Opcode::kMul), regs[a_]);
  }
  pc += 1;
  TS_DISPATCH();
}
L_Div : {
  TS_FIELDS();
  {
    // Smi/Smi and number-pair lanes: IEEE double arithmetic (exactly the
    // computation arithValues performs after identity conversions),
    // normalized inline. INT32_MIN / -1 is exact at 2^31 in double.
    if (regs[a_].isNumber() && regs[b_].isNumber()) {
      regs[a_] = tsSmiOrNumber(regs[a_].asDouble() / regs[b_].asDouble());
      pc += 1;
      TS_DISPATCH();
    }
    TS_GET(arithValues(regs[a_], regs[b_], Opcode::kDiv), regs[a_]);
  }
  pc += 1;
  TS_DISPATCH();
}
L_Mod : {
  TS_FIELDS();
  {
    // JS remainder: sign of the dividend; a zero result is -0 when the
    // dividend is negative (normalizeNumber keeps -0 a HeapNumber, Rule 72;
    // std::fmod's zero result already carries the dividend's sign).
    // x % 0 is NaN. INT32_MIN % -1 is UB for int -> the number lane handles
    // it as fmod(-2147483648.0, -1.0) = -0.
    if (regs[a_].isNumber() && regs[b_].isNumber()) {
      regs[a_] =
          tsSmiOrNumber(std::fmod(regs[a_].asDouble(), regs[b_].asDouble()));
      pc += 1;
      TS_DISPATCH();
    }
    if (tsBothSmi(regs[a_], regs[b_])) {
      int32_t l = regs[a_].asSmi();
      int32_t r = regs[b_].asSmi();
      if (!(l == kSmiMin && r == -1)) {
        int32_t m = l % r;
        if (m != 0) {
          regs[a_] = Value::smi(m);
        } else if (l < 0) {
          regs[a_] = Value::heapNumber(-0.0);
        } else {
          regs[a_] = Value::smi(0);
        }
        pc += 1;
        TS_DISPATCH();
      }
    }
    TS_GET(arithValues(regs[a_], regs[b_], Opcode::kMod), regs[a_]);
  }
  pc += 1;
  TS_DISPATCH();
}
L_Pow : {
  TS_FIELDS();
  TS_GET(arithValues(regs[a_], regs[b_], Opcode::kPow), regs[a_]);
  pc += 1;
  TS_DISPATCH();
}
L_BitAnd : {
  TS_FIELDS();
  {
    if (tsBothSmi(regs[a_], regs[b_])) {
      regs[a_] = Value::smi(regs[a_].asSmi() & regs[b_].asSmi());
      pc += 1;
      TS_DISPATCH();
    }
    TS_GET(bitwiseValues(regs[a_], regs[b_], Opcode::kBitAnd), regs[a_]);
  }
  pc += 1;
  TS_DISPATCH();
}
L_BitOr : {
  TS_FIELDS();
  {
    if (tsBothSmi(regs[a_], regs[b_])) {
      regs[a_] = Value::smi(regs[a_].asSmi() | regs[b_].asSmi());
      pc += 1;
      TS_DISPATCH();
    }
    TS_GET(bitwiseValues(regs[a_], regs[b_], Opcode::kBitOr), regs[a_]);
  }
  pc += 1;
  TS_DISPATCH();
}
L_BitXor : {
  TS_FIELDS();
  {
    if (tsBothSmi(regs[a_], regs[b_])) {
      regs[a_] = Value::smi(regs[a_].asSmi() ^ regs[b_].asSmi());
      pc += 1;
      TS_DISPATCH();
    }
    TS_GET(bitwiseValues(regs[a_], regs[b_], Opcode::kBitXor), regs[a_]);
  }
  pc += 1;
  TS_DISPATCH();
}
L_Shl : {
  TS_FIELDS();
  {
    if (tsBothSmi(regs[a_], regs[b_])) {
      // ToInt32(x) << (y & 31); Smis are already int32 (Rule 72).
      regs[a_] = Value::smi(regs[a_].asSmi() << (regs[b_].asSmi() & 31));
      pc += 1;
      TS_DISPATCH();
    }
    TS_GET(bitwiseValues(regs[a_], regs[b_], Opcode::kShl), regs[a_]);
  }
  pc += 1;
  TS_DISPATCH();
}
L_Shr : {
  TS_FIELDS();
  {
    if (tsBothSmi(regs[a_], regs[b_])) {
      // Arithmetic shift right of a negative value is value-defined for
      // int32 in C++20 and later (matches ToInt32(x) >> (y & 31)).
      regs[a_] = Value::smi(regs[a_].asSmi() >> (regs[b_].asSmi() & 31));
      pc += 1;
      TS_DISPATCH();
    }
    TS_GET(bitwiseValues(regs[a_], regs[b_], Opcode::kShr), regs[a_]);
  }
  pc += 1;
  TS_DISPATCH();
}
L_UShr : {
  TS_FIELDS();
  {
    if (tsBothSmi(regs[a_], regs[b_])) {
      uint32_t u = static_cast<uint32_t>(regs[a_].asSmi()) >>
                   (regs[b_].asSmi() & 31);
      regs[a_] = u <= static_cast<uint32_t>(kSmiMax)
                     ? Value::smi(static_cast<int32_t>(u))
                     : Value::heapNumber(static_cast<double>(u));
      pc += 1;
      TS_DISPATCH();
    }
    TS_GET(bitwiseValues(regs[a_], regs[b_], Opcode::kUShr), regs[a_]);
  }
  pc += 1;
  TS_DISPATCH();
}
L_BitNot : {
  TS_FIELDS();
  {
    if (regs[a_].isSmi()) {
      // ~ on int32 is exact and stays in range.
      regs[a_] = Value::smi(~regs[a_].asSmi());
      pc += 1;
      TS_DISPATCH();
    }
    Value n;
    TS_GET(toNumberValue(regs[a_]), n);
    regs[a_] = normalizeNumber(
        static_cast<double>(pure::toInt32(~pure::toInt32(n.asDouble()))));
  }
  pc += 1;
  TS_DISPATCH();
}
L_BigIntAdd :
L_BigIntSub :
L_BigIntMul :
L_BigIntDiv :
L_BigIntMod : {
  TS_FIELDS();
  TS_GET(bigintArith(regs[a_], regs[b_], static_cast<Opcode>(w & 0xFF)),
         regs[a_]);
  pc += 1;
  TS_DISPATCH();
}
L_BigIntNeg : {
  TS_FIELDS();
  TS_GET(bigintArith(regs[a_], regs[a_], Opcode::kBigIntNeg), regs[a_]);
  pc += 1;
  TS_DISPATCH();
}
L_Neg : {
  TS_FIELDS();
  {
    if (regs[a_].isSmi()) {
      // Smi negation: 0 -> -0 (HeapNumber, Rule 72), INT32_MIN -> 2^31.
      int32_t i = regs[a_].asSmi();
      regs[a_] = i == 0
                     ? Value::heapNumber(-0.0)
                     : i == kSmiMin
                           ? Value::heapNumber(-static_cast<double>(i))
                           : Value::smi(-i);
      pc += 1;
      TS_DISPATCH();
    }
    Value n;
    TS_GET(toNumericValue(regs[a_]), n);
    if (n.isBigInt()) {
      BigInt out = BigInt::negate(*n.asBigInt());
      regs[a_] = Value::raw(ValueKind::BigInt,
                            heap_.makeBigInt(std::move(out)));
    } else {
      regs[a_] = normalizeNumber(-n.asDouble());  // preserves -0
    }
  }
  pc += 1;
  TS_DISPATCH();
}
L_Inc : {
  TS_FIELDS();
  {
    if (regs[a_].isSmi()) {
      int32_t i = regs[a_].asSmi();
      regs[a_] = i == kSmiMax
                     ? Value::heapNumber(static_cast<double>(kSmiMax) + 1.0)
                     : Value::smi(i + 1);
      pc += 1;
      TS_DISPATCH();
    }
    if (regs[a_].isHeapNumber()) {
      regs[a_] = tsSmiOrNumber(regs[a_].asDouble() + 1.0);
      pc += 1;
      TS_DISPATCH();
    }
    Value n;
    TS_GET(toNumericValue(regs[a_]), n);
    if (n.isBigInt()) {
      BigInt one = BigInt::fromInt64(1);
      BigInt out = BigInt::add(*n.asBigInt(), one);
      regs[a_] = Value::raw(ValueKind::BigInt,
                            heap_.makeBigInt(std::move(out)));
    } else {
      regs[a_] = normalizeNumber(n.asDouble() + 1.0);
    }
  }
  pc += 1;
  TS_DISPATCH();
}
L_Dec : {
  TS_FIELDS();
  {
    if (regs[a_].isSmi()) {
      int32_t i = regs[a_].asSmi();
      regs[a_] = i == kSmiMin
                     ? Value::heapNumber(static_cast<double>(kSmiMin) - 1.0)
                     : Value::smi(i - 1);
      pc += 1;
      TS_DISPATCH();
    }
    if (regs[a_].isHeapNumber()) {
      regs[a_] = tsSmiOrNumber(regs[a_].asDouble() - 1.0);
      pc += 1;
      TS_DISPATCH();
    }
    Value n;
    TS_GET(toNumericValue(regs[a_]), n);
    if (n.isBigInt()) {
      BigInt one = BigInt::fromInt64(1);
      BigInt out = BigInt::sub(*n.asBigInt(), one);
      regs[a_] = Value::raw(ValueKind::BigInt,
                            heap_.makeBigInt(std::move(out)));
    } else {
      regs[a_] = normalizeNumber(n.asDouble() - 1.0);
    }
  }
  pc += 1;
  TS_DISPATCH();
}
L_Lt : {
  TS_FIELDS();
  {
    if (tsBothSmi(regs[a_], regs[b_])) {
      regs[a_] = Value::boolean(regs[a_].asSmi() < regs[b_].asSmi());
      pc += 1;
      TS_DISPATCH();
    }
    if (regs[a_].isNumber() && regs[b_].isNumber()) {
      regs[a_] = Value::boolean(regs[a_].asDouble() < regs[b_].asDouble());
      pc += 1;
      TS_DISPATCH();
    }
    Relational rel = Relational::Unordered;
    TS_GET(relationalCompare(regs[a_], regs[b_]), rel);
    regs[a_] = Value::boolean(rel == Relational::Less);
  }
  pc += 1;
  TS_DISPATCH();
}
L_Le : {
  TS_FIELDS();
  {
    if (tsBothSmi(regs[a_], regs[b_])) {
      regs[a_] = Value::boolean(regs[a_].asSmi() <= regs[b_].asSmi());
      pc += 1;
      TS_DISPATCH();
    }
    if (regs[a_].isNumber() && regs[b_].isNumber()) {
      regs[a_] = Value::boolean(regs[a_].asDouble() <= regs[b_].asDouble());
      pc += 1;
      TS_DISPATCH();
    }
    Relational rel = Relational::Unordered;
    TS_GET(relationalCompare(regs[a_], regs[b_]), rel);
    // x <= y is !(x > y): false when Unordered (NaN) — v0.4 fix, the prior
    // mapping (rel != Greater) returned true for NaN (Rule 96 defect).
    regs[a_] = Value::boolean(rel == Relational::Less ||
                              rel == Relational::Equal);
  }
  pc += 1;
  TS_DISPATCH();
}
L_Gt : {
  TS_FIELDS();
  {
    if (tsBothSmi(regs[a_], regs[b_])) {
      regs[a_] = Value::boolean(regs[a_].asSmi() > regs[b_].asSmi());
      pc += 1;
      TS_DISPATCH();
    }
    if (regs[a_].isNumber() && regs[b_].isNumber()) {
      regs[a_] = Value::boolean(regs[a_].asDouble() > regs[b_].asDouble());
      pc += 1;
      TS_DISPATCH();
    }
    Relational rel = Relational::Unordered;
    TS_GET(relationalCompare(regs[a_], regs[b_]), rel);
    regs[a_] = Value::boolean(rel == Relational::Greater);
  }
  pc += 1;
  TS_DISPATCH();
}
L_Ge : {
  TS_FIELDS();
  {
    if (tsBothSmi(regs[a_], regs[b_])) {
      regs[a_] = Value::boolean(regs[a_].asSmi() >= regs[b_].asSmi());
      pc += 1;
      TS_DISPATCH();
    }
    if (regs[a_].isNumber() && regs[b_].isNumber()) {
      regs[a_] = Value::boolean(regs[a_].asDouble() >= regs[b_].asDouble());
      pc += 1;
      TS_DISPATCH();
    }
    Relational rel = Relational::Unordered;
    TS_GET(relationalCompare(regs[a_], regs[b_]), rel);
    // x >= y is !(x < y): false when Unordered (NaN) — v0.4 fix, same
    // mapping defect as Le (Rule 96).
    regs[a_] = Value::boolean(rel == Relational::Greater ||
                              rel == Relational::Equal);
  }
  pc += 1;
  TS_DISPATCH();
}
L_AbstractEq : {
  TS_FIELDS();
  {
    bool eq = false;
    TS_GET(abstractEquals(regs[a_], regs[b_]), eq);
    regs[a_] = Value::boolean(eq);
  }
  pc += 1;
  TS_DISPATCH();
}
L_StrictEq : {
  TS_FIELDS();
  {
    if (fb != nullptr)
      recordBinarySite(&fb[slotMap[pc]], regs[a_], regs[b_]);
    // v0.4 fast lanes: Smi==Smi, and different kinds that cannot both be
    // numbers (5 === 5.0 is true across the Smi/HeapNumber split).
    if (tsBothSmi(regs[a_], regs[b_])) {
      regs[a_] = Value::boolean(regs[a_].asSmi() == regs[b_].asSmi());
      pc += 1;
      TS_DISPATCH();
    }
    if (regs[a_].kind() != regs[b_].kind() &&
        !(regs[a_].isNumber() && regs[b_].isNumber())) {
      regs[a_] = Value::boolean(false);
      pc += 1;
      TS_DISPATCH();
    }
    regs[a_] = Value::boolean(pure::strictEquals(regs[a_], regs[b_]));
  }
  pc += 1;
  TS_DISPATCH();
}
L_SameValue : {
  TS_FIELDS();
  if (fb != nullptr) recordBinarySite(&fb[slotMap[pc]], regs[a_], regs[b_]);
  regs[a_] = Value::boolean(pure::sameValue(regs[a_], regs[b_]));
  pc += 1;
  TS_DISPATCH();
}
L_SameValueZero : {
  TS_FIELDS();
  if (fb != nullptr) recordBinarySite(&fb[slotMap[pc]], regs[a_], regs[b_]);
  regs[a_] = Value::boolean(pure::sameValueZero(regs[a_], regs[b_]));
  pc += 1;
  TS_DISPATCH();
}
L_LogicalNot : {
  TS_FIELDS();
  regs[a_] = Value::boolean(!pure::toBoolean(regs[a_]));
  pc += 1;
  TS_DISPATCH();
}
L_ToBoolean : {
  TS_FIELDS();
  regs[a_] = Value::boolean(pure::toBoolean(regs[a_]));
  pc += 1;
  TS_DISPATCH();
}
L_TypeOf : {
  TS_FIELDS();
  {
    std::string k = pure::kindName(regs[a_]);
    regs[a_] = Value::string(
        heap_.makeString(std::u16string(k.begin(), k.end())));
  }
  pc += 1;
  TS_DISPATCH();
}
L_ToNumber : {
  TS_FIELDS();
  TS_GET(toNumberValue(regs[a_]), regs[a_]);
  pc += 1;
  TS_DISPATCH();
}
L_ToNumeric : {
  TS_FIELDS();
  TS_GET(toNumericValue(regs[a_]), regs[a_]);
  pc += 1;
  TS_DISPATCH();
}
L_ToString : {
  TS_FIELDS();
  TS_GET(toStringValue(regs[a_]), regs[a_]);
  pc += 1;
  TS_DISPATCH();
}
L_ToInt32 : {
  TS_FIELDS();
  {
    Value n;
    TS_GET(toNumberValue(regs[a_]), n);
    regs[a_] = Value::smi(pure::toInt32(n.asDouble()));
  }
  pc += 1;
  TS_DISPATCH();
}
L_ToUint32 : {
  TS_FIELDS();
  {
    Value n;
    TS_GET(toNumberValue(regs[a_]), n);
    regs[a_] = normalizeNumber(static_cast<double>(pure::toUint32(n.asDouble())));
  }
  pc += 1;
  TS_DISPATCH();
}
L_ToBigInt : {
  TS_FIELDS();
  TS_GET(toBigIntValue(regs[a_]), regs[a_]);
  pc += 1;
  TS_DISPATCH();
}
L_ToPrimitive : {
  TS_FIELDS();
  TS_GET(toPrimitive(regs[a_], false), regs[a_]);
  pc += 1;
  TS_DISPATCH();
}
L_StringConcat : {
  TS_FIELDS();
  {
    // v0.4: both-strings fast lane (no conversion, single exact allocation).
    if (regs[a_].isString() && regs[b_].isString()) {
      regs[a_] = Value::string(
          tsConcatStrings(regs[a_].asString(), regs[b_].asString(), heap_));
      pc += 1;
      TS_DISPATCH();
    }
    Value ls;
    TS_GET(toStringValue(regs[a_]), ls);
    Value rs;
    TS_GET(toStringValue(regs[b_]), rs);
    regs[a_] =
        Value::string(heap_.makeString(ls.asString()->data + rs.asString()->data));
  }
  pc += 1;
  TS_DISPATCH();
}
L_StringLength : {
  TS_FIELDS();
  {
    if (!regs[a_].isString()) {
      TS_RAISE(typeError("StringLength operand is not a string"));
    }
    regs[a_] = Value::smi(static_cast<int32_t>(regs[a_].asString()->data.size()));
  }
  pc += 1;
  TS_DISPATCH();
}
L_CharCodeAt : {
  TS_FIELDS();
  {
    if (!regs[b_].isString()) {
      TS_RAISE(typeError("CharCodeAt operand is not a string"));
    }
    const std::u16string& str = regs[b_].asString()->data;
    Value idxVal;
    TS_GET(toNumberValue(regs[c_]), idxVal);
    double d = idxVal.asDouble();
    int64_t idx;
    if (std::isnan(d)) {
      idx = 0;  // ToIntegerOrInfinity(NaN) = 0 (oracle-verified)
    } else if (std::isinf(d)) {
      idx = INT64_MAX;  // out of range either way
    } else {
      idx = static_cast<int64_t>(std::trunc(d));
    }
    if (idx < 0 || idx >= static_cast<int64_t>(str.size())) {
      regs[a_] =
          Value::heapNumber(std::numeric_limits<double>::quiet_NaN());
    } else {
      regs[a_] = Value::smi(static_cast<int32_t>(
          static_cast<uint16_t>(str[static_cast<size_t>(idx)])));
    }
  }
  pc += 2;
  TS_DISPATCH();
}
L_StringFromCharCode : {
  TS_FIELDS();
  {
    Value n;
    TS_GET(toNumberValue(regs[a_]), n);
    double d = n.asDouble();
    double m = std::fmod(std::trunc(d), 65536.0);
    if (m < 0) m += 65536.0;
    regs[a_] = Value::string(heap_.makeString(
        std::u16string(1, static_cast<char16_t>(static_cast<uint32_t>(m)))));
  }
  pc += 1;
  TS_DISPATCH();
}
L_Jmp : {
  int16_t off = static_cast<int16_t>((w >> 8) & 0xFFFF);
  pc = static_cast<uint32_t>(static_cast<int64_t>(pc) + off);
  TS_DISPATCH();
}
L_JmpWide : {
  TS_FIELDS();
  {
    int32_t off = static_cast<int32_t>(c_ | (d_ << 8) | (e_ << 16));
    if (off >= (1 << 23)) off -= (1 << 24);
    pc = static_cast<uint32_t>(static_cast<int64_t>(pc) + off);
  }
  TS_DISPATCH();
}
L_JmpTrue :
L_JmpFalse :
L_JmpTrueWide :
L_JmpFalseWide : {
  TS_FIELDS();
  {
    Opcode op = static_cast<Opcode>(w & 0xFF);
    bool cond = tsQuickTruthy(regs[a_]);  // v0.4: inlined fast lane
    bool jumps = (op == Opcode::kJmpTrue || op == Opcode::kJmpTrueWide)
                     ? cond
                     : !cond;
    if (fb != nullptr)
      recordBranchSite(&fb[slotMap[pc]], jumps);
    if (jumps) {
      if (op == Opcode::kJmpTrueWide || op == Opcode::kJmpFalseWide) {
        int32_t off = static_cast<int32_t>(c_ | (d_ << 8) | (e_ << 16));
        if (off >= (1 << 23)) off -= (1 << 24);
        pc = static_cast<uint32_t>(static_cast<int64_t>(pc) + off);
      } else {
        int8_t off = static_cast<int8_t>(b_);
        pc = static_cast<uint32_t>(static_cast<int64_t>(pc) + off);
      }
      TS_DISPATCH();
    }
    pc += (op == Opcode::kJmpTrueWide || op == Opcode::kJmpFalseWide) ? 2 : 1;
    TS_DISPATCH();
  }
}
L_Throw : {
  TS_FIELDS();
  TS_RAISE(regs[a_]);
}
L_Rethrow : {
  if (!frame.hasPendingException) {
    TS_RAISE(typeError("Rethrow outside of a catch handler"));
  }
  TS_RAISE(frame.pendingException);
}

// ---------------------------------------------------------------------------
// In-frame exception dispatch (Rule 74 / bytecode_spec.md Section 7).
// ---------------------------------------------------------------------------
raise_in_frame : {
  const Handler* found = nullptr;
  for (const Handler& h : fn->handlers) {
    if (faultPc >= h.startPc && faultPc < h.endPc) {
      if (found == nullptr || h.startPc >= found->startPc) {
        found = &h;  // innermost = latest start among containing ranges
      }
    }
  }
  if (found != nullptr) {
    frame.regs[found->catchReg] = excVal;
    frame.pendingException = excVal;
    frame.hasPendingException = true;
    pc = found->handlerPc;
    TS_DISPATCH();
  }
  frame.pc = pc;
  return std::unexpected(JsException{excVal});
}

#if !TS_COMPUTED_GOTO
dispatchSwitch : {
  uint32_t opb = w & 0xFF;
  switch (opb) {
#define TS_OP(name, fmt, effect, ic, roles_expr) \
  case static_cast<uint32_t>(Opcode::k##name): goto L_##name;
#include "ts_opcodes.inc"
#undef TS_OP
    default:
      excVal = excValue(typeError("internal: invalid opcode byte"));
      faultPc = pc;
      goto raise_in_frame;
  }
}
#endif

#undef TS_DISPATCH
#undef TS_FIELDS
#undef TS_RAISE
#undef TS_TAKE
#undef TS_GET
}

}  // namespace ts
