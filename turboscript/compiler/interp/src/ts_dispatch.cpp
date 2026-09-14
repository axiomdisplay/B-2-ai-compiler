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
  FeedbackSlot* fb = feedback_[fnIndex].empty()
                         ? nullptr
                         : feedback_[fnIndex].data();
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

  // Field decoders for the current word w (and suffix s).
  uint32_t s = 0;
#define TS_FIELDS()                          \
  do {                                       \
    a_ = (w >> 8) & 0xFF;                    \
    b_ = (w >> 16) & 0xFF;                   \
    if (formatWordCount(opcodeInfo(          \
            static_cast<Opcode>(w & 0xFF))   \
            .format) == 2) {                 \
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
    TS_GET(getProperty(Value::raw(ValueKind::Object, global_), nameSym),
           regs[a_]);
  }
  pc += 2;
  TS_DISPATCH();
}
L_StoreGlobal : {
  TS_FIELDS();
  {
    SymbolId nameSym = globalNames_[c_ | (d_ << 8) | (e_ << 16)];
    TS_TAKE(setProperty(Value::raw(ValueKind::Object, global_), nameSym,
                        regs[a_]));
  }
  pc += 2;
  TS_DISPATCH();
}
L_GetProperty : {
  TS_FIELDS();
  {
    // W2_RR_D layout: a_=dst, b_=receiver, c_=key.
    Value recv = regs[b_];
    SymbolId keySym;
    TS_GET(toPropertyKey(regs[c_]), keySym);
    if (fb != nullptr) recordPropertySite(fn->slotOfPc[pc], recv);
    TS_GET(getProperty(recv, keySym), regs[a_]);
  }
  pc += 2;
  TS_DISPATCH();
}
L_SetProperty : {
  TS_FIELDS();
  {
    Value recv = regs[a_];
    SymbolId keySym;
    TS_GET(toPropertyKey(regs[b_]), keySym);
    if (fb != nullptr) recordPropertySite(fn->slotOfPc[pc], recv);
    TS_TAKE(setProperty(recv, keySym, regs[c_]));
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
    if (!regs[b_].isObjectLike()) {
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
    if (!obj.isObjectLike()) {
      regs[a_] = Value::boolean(false);
    } else if (!proto.isObjectLike() && !proto.isNull()) {
      regs[a_] = Value::boolean(false);
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
    if (fb != nullptr) recordCallSite(fn->slotOfPc[pc], regs[a_]);
    TS_GET(callValue(regs[a_], Value::undefined(), &regs[b_], c_),
           regs[e_]);
  }
  pc += 2;
  TS_DISPATCH();
}
L_CallMethod : {
  TS_FIELDS();
  {
    if (fb != nullptr) recordCallSite(fn->slotOfPc[pc], regs[a_]);
    TS_GET(callValue(regs[a_], regs[d_], &regs[b_], c_), regs[e_]);
  }
  pc += 2;
  TS_DISPATCH();
}
L_Construct : {
  TS_FIELDS();
  {
    if (fb != nullptr) recordCallSite(fn->slotOfPc[pc], regs[a_]);
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
      parent = static_cast<Context*>(regs[b_].ptr);
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
    Context* ctx = static_cast<Context*>(regs[b_].ptr);
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
    Context* ctx = static_cast<Context*>(regs[a_].ptr);
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
    // Plain object with Object.prototype pending builtins (bytecode_spec 11);
    // shape tree assignment happens on first store (interp_contract 3).
    Object* obj = heap_.makeObject();
    regs[a_] = Value::raw(ValueKind::Object, obj);
  }
  pc += 1;
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
  TS_GET(addValues(regs[a_], regs[b_], fn->slotOfPc[pc]), regs[a_]);
  pc += 1;
  TS_DISPATCH();
}
L_Sub :
L_Mul :
L_Div :
L_Mod :
L_Pow : {
  TS_FIELDS();
  TS_GET(arithValues(regs[a_], regs[b_], static_cast<Opcode>(w & 0xFF)),
         regs[a_]);
  pc += 1;
  TS_DISPATCH();
}
L_BitAnd :
L_BitOr :
L_BitXor :
L_Shl :
L_Shr :
L_UShr : {
  TS_FIELDS();
  TS_GET(bitwiseValues(regs[a_], regs[b_], static_cast<Opcode>(w & 0xFF)),
         regs[a_]);
  pc += 1;
  TS_DISPATCH();
}
L_BitNot : {
  TS_FIELDS();
  {
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
L_Lt :
L_Le :
L_Gt :
L_Ge : {
  TS_FIELDS();
  {
    Relational rel = Relational::Unordered;
    TS_GET(relationalCompare(regs[a_], regs[b_]), rel);
    bool result;
    switch (static_cast<Opcode>(w & 0xFF)) {
      case Opcode::kLt: result = rel == Relational::Less; break;
      case Opcode::kLe: result = rel != Relational::Greater; break;
      case Opcode::kGt: result = rel == Relational::Greater; break;
      default: result = rel != Relational::Less; break;  // Ge
    }
    regs[a_] = Value::boolean(result);
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
  if (fb != nullptr) recordBinarySite(fn->slotOfPc[pc], regs[a_], regs[b_]);
  regs[a_] = Value::boolean(pure::strictEquals(regs[a_], regs[b_]));
  pc += 1;
  TS_DISPATCH();
}
L_SameValue : {
  TS_FIELDS();
  if (fb != nullptr) recordBinarySite(fn->slotOfPc[pc], regs[a_], regs[b_]);
  regs[a_] = Value::boolean(pure::sameValue(regs[a_], regs[b_]));
  pc += 1;
  TS_DISPATCH();
}
L_SameValueZero : {
  TS_FIELDS();
  if (fb != nullptr) recordBinarySite(fn->slotOfPc[pc], regs[a_], regs[b_]);
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
    Value ls;
    TS_GET(toStringValue(regs[a_]), ls);
    Value rs;
    TS_GET(toStringValue(regs[b_]), rs);
    regs[a_] =
        Value::string(heap_.makeString(ls.str->data + rs.str->data));
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
    regs[a_] = Value::smi(static_cast<int32_t>(regs[a_].str->data.size()));
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
    const std::u16string& str = regs[b_].str->data;
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
    bool cond = pure::toBoolean(regs[a_]);
    bool jumps = (op == Opcode::kJmpTrue || op == Opcode::kJmpTrueWide)
                     ? cond
                     : !cond;
    if (fb != nullptr) recordBranchSite(fn->slotOfPc[pc], jumps);
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
