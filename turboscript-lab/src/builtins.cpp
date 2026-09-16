// TurboScript Tier 0 — builtins.cpp
#include "runtime.h"
#include "interp.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace ts {

// ------------------------------------------------------------- helpers
static void printUTF16(const String* s, FILE* out) {
    const char16_t* p = s->data();
    for (uint32_t i = 0; i < s->len; i++) {
        uint32_t c = p[i];
        if (c < 0x80) fputc((char)c, out);
        else if (c < 0x800) {
            fputc((char)(0xC0 | (c >> 6)), out);
            fputc((char)(0x80 | (c & 0x3F)), out);
        } else {
            fputc((char)(0xE0 | (c >> 12)), out);
            fputc((char)(0x80 | ((c >> 6) & 0x3F)), out);
            fputc((char)(0x80 | (c & 0x3F)), out);
        }
    }
}

static Value numResultB(double d) {
    if (d >= -2147483648.0 && d <= 2147483647.0 && d == (double)(int32_t)d)
        return Value::fromSmi((int32_t)d);
    return Value::fromDouble(d);
}

static void logArgs(Runtime* rt, Value* args, uint32_t argc, FILE* out, bool newline) {
    for (uint32_t i = 0; i < argc; i++) {
        if (i) fputc(' ', out);
        String* s = rt->toStringObj(args[i]);
        printUTF16(s, out);
    }
    if (newline) fputc('\n', out);
}

// ------------------------------------------------------------- console / global fns
static Value native_console_log(Runtime* rt, Value, Value* args, uint32_t argc) {
    logArgs(rt, args, argc, stdout, true);
    return Value::undef();
}
static Value native_console_error(Runtime* rt, Value, Value* args, uint32_t argc) {
    logArgs(rt, args, argc, stderr, true);
    return Value::undef();
}
static Value native_print(Runtime* rt, Value, Value* args, uint32_t argc) {
    logArgs(rt, args, argc, stdout, true);
    return Value::undef();
}
static Value native_parseInt(Runtime* rt, Value, Value* args, uint32_t argc) {
    if (!argc) return Value::nan();
    String* s = rt->toStringObj(args[0]);
    // skip ws, optional sign, parse integer prefix in radix 10 (radix arg: registered gap)
    uint32_t i = 0;
    const char16_t* p = s->data();
    while (i < s->len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) i++;
    bool neg = false;
    if (i < s->len && (p[i] == '+' || p[i] == '-')) { neg = p[i] == '-'; i++; }
    double v = 0; bool any = false;
    for (; i < s->len; i++) {
        if (p[i] < '0' || p[i] > '9') break;
        v = v * 10 + (p[i] - '0');
        any = true;
    }
    if (!any) return Value::nan();
    return Value::fromSmi(neg ? (int32_t)(-v) : (int32_t)v);
}
static Value native_parseFloat(Runtime* rt, Value, Value* args, uint32_t argc) {
    if (!argc) return Value::nan();
    String* s = rt->toStringObj(args[0]);
    // crude: reuse stringToNumber on the longest valid prefix via strtod
    char buf[64]; uint32_t n = s->len < 60 ? s->len : 60;
    for (uint32_t i = 0; i < n; i++) {
        char16_t c = s->data()[i];
        buf[i] = (c < 128) ? (char)c : ' ';
    }
    buf[n] = 0;
    char* end = nullptr;
    double v = std::strtod(buf, &end);
    if (end == buf) return Value::nan();
    return Value::fromDouble(v);
}
static Value native_isNaN(Runtime* rt, Value, Value* args, uint32_t argc) {
    if (!argc) return Value::true_();
    double d = args[0].isNumber() ? args[0].asNumber() : rt->toDoubleForArith(args[0]);
    return Value::boolean(std::isnan(d));
}
static Value native_Number(Runtime* rt, Value, Value* args, uint32_t argc) {
    if (!argc) return Value::fromSmi(0);
    return rt->toNumberValue(args[0]);
}
static Value native_String(Runtime* rt, Value, Value* args, uint32_t argc) {
    if (!argc) return Value::fromObj((HeapObj*)rt->intern(""));
    return Value::fromObj((HeapObj*)rt->toStringObj(args[0]));
}
static Value native_Boolean(Runtime* rt, Value, Value* args, uint32_t argc) {
    return Value::boolean(argc && args[0].toBoolean());
}
static Value native_Error(Runtime* rt, Value thisVal, Value* args, uint32_t argc) {
    Object* o;
    if (thisVal.isObject()) o = thisVal.asObj();
    else o = Object::create(rt, rt->objectProto, false);
    const char* msg = argc ? "" : "";
    rt->defineProperty(o, "name", Value::fromObj((HeapObj*)rt->intern("Error")));
    if (argc) {
        String* s = rt->toStringObj(args[0]);
        rt->defineProperty(o, "message", Value::fromObj((HeapObj*)s));
    } else {
        rt->defineProperty(o, "message", Value::fromObj((HeapObj*)rt->intern("")));
    }
    (void)msg;
    return Value::fromObj((HeapObj*)o);
}

// ------------------------------------------------------------- Math
static Value native_math_sqrt(Runtime*, Value, Value* a, uint32_t n) {
    return Value::fromDouble(n ? std::sqrt(a[0].asNumber()) : std::nan(""));
}
static Value native_math_abs(Runtime*, Value, Value* a, uint32_t n) {
    if (!n) return Value::nan();
    Value x = a[0];
    if (x.isSmi()) { int32_t v = x.asSmi(); return v < 0 ? Value::fromDouble(-(double)v) : x; }
    return Value::fromDouble(std::fabs(x.asNumber()));
}
static Value native_math_floor(Runtime*, Value, Value* a, uint32_t n) {
    if (!n) return Value::nan();
    Value x = a[0];
    if (x.isSmi()) return x;
    return numResultB(std::floor(x.asNumber()));
}
static Value native_math_ceil(Runtime*, Value, Value* a, uint32_t n) {
    if (!n) return Value::nan();
    Value x = a[0];
    if (x.isSmi()) return x;
    return numResultB(std::ceil(x.asNumber()));
}
static Value native_math_round(Runtime*, Value, Value* a, uint32_t n) {
    if (!n) return Value::nan();
    double d = a[0].asNumber();
    return numResultB(std::floor(d + 0.5)); // ES semantics: half-up
}
static Value native_math_min(Runtime*, Value, Value* a, uint32_t n) {
    double m = INFINITY;
    for (uint32_t i = 0; i < n; i++) {
        double v = a[i].asNumber();
        if (std::isnan(v)) return Value::nan();
        if (v < m) m = v;
    }
    return Value::fromDouble(m);
}
static Value native_math_max(Runtime*, Value, Value* a, uint32_t n) {
    double m = -INFINITY;
    for (uint32_t i = 0; i < n; i++) {
        double v = a[i].asNumber();
        if (std::isnan(v)) return Value::nan();
        if (v > m) m = v;
    }
    return Value::fromDouble(m);
}
static Value native_math_pow(Runtime*, Value, Value* a, uint32_t n) {
    double x = n ? a[0].asNumber() : std::nan("");
    double y = n > 1 ? a[1].asNumber() : std::nan("");
    return Value::fromDouble(std::pow(x, y));
}
static Value native_math_log(Runtime*, Value, Value* a, uint32_t n) {
    return Value::fromDouble(n ? std::log(a[0].asNumber()) : std::nan(""));
}
static uint64_t rngState = 0x9E3779B97F4A7C15ull;
static Value native_math_random(Runtime*, Value, Value*, uint32_t) {
    rngState ^= rngState << 13; rngState ^= rngState >> 7; rngState ^= rngState << 17;
    return Value::fromDouble((double)(rngState >> 11) / 9007199254740992.0);
}

// ------------------------------------------------------------- Object
static Value native_object_keys(Runtime* rt, Value, Value* a, uint32_t n) {
    Object* out = Object::create(rt, rt->arrayProto, true);
    std::vector<Value> keys;
    if (n && a[0].isObject()) {
        Object* o = a[0].asObj();
        if (!o->dictMode) {
            std::vector<String*> names;
            for (Shape* s = o->shape; s && s->key; s = s->parent) names.push_back(s->key);
            for (size_t i = names.size(); i-- > 0;) keys.push_back(Value::fromObj((HeapObj*)names[i]));
        } else {
            o->dict->forEach([&](String* k, Value) { keys.push_back(Value::fromObj((HeapObj*)k)); });
        }
    }
    if (!keys.empty()) {
        out->elements = (Value*)std::malloc(keys.size() * sizeof(Value));
        out->elemCap = (uint32_t)keys.size();
        out->elemLen = (uint32_t)keys.size();
        for (size_t i = 0; i < keys.size(); i++) out->elements[i] = keys[i];
    }
    return Value::fromObj((HeapObj*)out);
}
static Value native_array_isArray(Runtime* rt, Value, Value* a, uint32_t n) {
    return Value::boolean(n && a[0].isObject() && a[0].asObj()->isArray);
}
static Value native_Array(Runtime* rt, Value, Value* a, uint32_t n) {
    // new Array(len) or new Array(e0, e1, ...) — returns a fresh array object
    Object* out = Object::create(rt, rt->arrayProto, true);
    if (n == 1 && a[0].isNumber()) {
        double d = a[0].asNumber();
        uint32_t len = (d >= 0 && d < 0x40000000) ? (uint32_t)d : 0;
        if (len) {
            out->elements = (Value*)std::malloc((size_t)len * sizeof(Value));
            out->elemCap = len;
            out->elemLen = len;
            for (uint32_t i = 0; i < len; i++) out->elements[i] = Value::undef();
        }
    } else if (n) {
        out->elements = (Value*)std::malloc((size_t)n * sizeof(Value));
        out->elemCap = n;
        out->elemLen = n;
        for (uint32_t i = 0; i < n; i++) out->elements[i] = a[i];
    }
    return Value::fromObj((HeapObj*)out);
}

// ------------------------------------------------------------- Object.prototype
static Value native_op_toString(Runtime* rt, Value thisVal, Value*, uint32_t) {
    const char* s = "[object Object]";
    if (thisVal.isObject() && thisVal.asObj()->isArray) s = "[object Array]";
    if (thisVal.isFunction()) s = "[object Function]";
    return Value::fromObj((HeapObj*)rt->intern(s));
}
static Value native_op_valueOf(Runtime*, Value thisVal, Value*, uint32_t) {
    return thisVal;
}
static Value native_op_hasOwnProperty(Runtime* rt, Value thisVal, Value* a, uint32_t n) {
    if (!n || !thisVal.isObject()) return Value::false_();
    Object* o = thisVal.asObj();
    String* key = rt->toStringObj(a[0]);
    bool found = false;
    if (!o->dictMode) { o->ownSlotIndex(key, &found); }
    else found = o->dict->find(key) != nullptr;
    if (!found) {
        uint32_t idx;
        if (strToIndex(key, &idx) && idx < o->elemLen) found = true;
    }
    return Value::boolean(found);
}

// ------------------------------------------------------------- Array.prototype
static Value native_arr_push(Runtime* rt, Value thisVal, Value* a, uint32_t n) {
    if (!thisVal.isObject()) rt->throwTypeError("Array.prototype.push on non-object");
    Object* o = thisVal.asObj();
    for (uint32_t i = 0; i < n; i++) {
        if (o->elemLen >= o->elemCap) {
            uint32_t nc = o->elemCap ? o->elemCap * 2 : 4;
            o->elements = (Value*)std::realloc(o->elements, (size_t)nc * sizeof(Value));
            o->elemCap = nc;
        }
        o->elements[o->elemLen++] = a[i];
    }
    return Value::fromSmi((int32_t)o->elemLen);
}
static Value native_arr_pop(Runtime* rt, Value thisVal, Value*, uint32_t) {
    if (!thisVal.isObject()) rt->throwTypeError("Array.prototype.pop on non-object");
    Object* o = thisVal.asObj();
    if (!o->elemLen) return Value::undef();
    return o->elements[--o->elemLen];
}
static Value native_arr_join(Runtime* rt, Value thisVal, Value* a, uint32_t n) {
    if (!thisVal.isObject()) rt->throwTypeError("Array.prototype.join on non-object");
    Object* o = thisVal.asObj();
    String* sep = n ? rt->toStringObj(a[0]) : rt->intern(",");
    String* acc = rt->intern("");
    for (uint32_t i = 0; i < o->elemLen; i++) {
        if (i) acc = stringConcat(rt, acc, sep);
        Value v = o->elements[i];
        if (!v.isUndef() && !v.isNull()) acc = stringConcat(rt, acc, rt->toStringObj(v));
    }
    return Value::fromObj((HeapObj*)acc);
}
static Value native_arr_indexOf(Runtime* rt, Value thisVal, Value* a, uint32_t n) {
    if (!thisVal.isObject()) return Value::fromSmi(-1);
    Object* o = thisVal.asObj();
    for (uint32_t i = 0; i < o->elemLen; i++) {
        if (strictEquals(o->elements[i], n ? a[0] : Value::undef()))
            return Value::fromSmi((int32_t)i);
    }
    return Value::fromSmi(-1);
}
static Value native_arr_includes(Runtime* rt, Value thisVal, Value* a, uint32_t n) {
    if (!thisVal.isObject()) return Value::false_();
    Object* o = thisVal.asObj();
    for (uint32_t i = 0; i < o->elemLen; i++) {
        if (sameValueZero(o->elements[i], n ? a[0] : Value::undef()))
            return Value::true_();
    }
    return Value::false_();
}
static Value native_arr_slice(Runtime* rt, Value thisVal, Value* a, uint32_t n) {
    Object* out = Object::create(rt, rt->arrayProto, true);
    if (!thisVal.isObject()) return Value::fromObj((HeapObj*)out);
    Object* o = thisVal.asObj();
    int32_t len = (int32_t)o->elemLen;
    int32_t start = 0, end = len;
    if (n) {
        double s = a[0].asNumber();
        start = s < 0 ? (int32_t)(len + s) : (int32_t)s;
    }
    if (n > 1) {
        double e = a[1].asNumber();
        end = e < 0 ? (int32_t)(len + e) : (int32_t)e;
    }
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start > end) end = start;
    uint32_t cnt = (uint32_t)(end - start);
    if (cnt) {
        out->elements = (Value*)std::malloc(cnt * sizeof(Value));
        out->elemCap = cnt;
        out->elemLen = cnt;
        for (uint32_t i = 0; i < cnt; i++) out->elements[i] = o->elements[start + i];
    }
    return Value::fromObj((HeapObj*)out);
}
static Value native_arr_forEach(Runtime* rt, Value thisVal, Value* a, uint32_t n) {
    if (!thisVal.isObject() || !n || !a[0].isFunction()) rt->throwTypeError("forEach requires a function");
    Object* o = thisVal.asObj();
    JSFunction* f = (JSFunction*)a[0].asObj();
    Value cargs[3];
    for (uint32_t i = 0; i < o->elemLen; i++) {
        cargs[0] = o->elements[i];
        cargs[1] = Value::fromSmi((int32_t)i);
        cargs[2] = thisVal;
        rt->interp->callFunction(f, Value::undef(), cargs, 3);
    }
    return Value::undef();
}
static Value native_arr_map(Runtime* rt, Value thisVal, Value* a, uint32_t n) {
    Object* out = Object::create(rt, rt->arrayProto, true);
    if (!thisVal.isObject() || !n || !a[0].isFunction()) rt->throwTypeError("map requires a function");
    Object* o = thisVal.asObj();
    JSFunction* f = (JSFunction*)a[0].asObj();
    Value cargs[3];
    if (o->elemLen) {
        out->elements = (Value*)std::malloc(o->elemLen * sizeof(Value));
        out->elemCap = o->elemLen;
        out->elemLen = o->elemLen;
        for (uint32_t i = 0; i < o->elemLen; i++) {
            cargs[0] = o->elements[i];
            cargs[1] = Value::fromSmi((int32_t)i);
            cargs[2] = thisVal;
            out->elements[i] = rt->interp->callFunction(f, Value::undef(), cargs, 3);
        }
    }
    return Value::fromObj((HeapObj*)out);
}

// ------------------------------------------------------------- String.prototype
static Value native_str_charAt(Runtime* rt, Value thisVal, Value* a, uint32_t n) {
    String* s = rt->toStringObj(thisVal);
    int32_t i = (n && a[0].isNumber()) ? (int32_t)a[0].asNumber() : 0;
    if (i < 0 || (uint32_t)i >= s->len) return Value::fromObj((HeapObj*)rt->intern(""));
    char16_t c = s->data()[i];
    return Value::fromObj((HeapObj*)stringFromUTF16(rt, &c, 1));
}
static Value native_str_charCodeAt(Runtime* rt, Value thisVal, Value* a, uint32_t n) {
    String* s = rt->toStringObj(thisVal);
    int32_t i = (n && a[0].isNumber()) ? (int32_t)a[0].asNumber() : 0;
    if (i < 0 || (uint32_t)i >= s->len) return Value::nan();
    return Value::fromSmi((int32_t)s->data()[i]);
}
static Value native_str_indexOf(Runtime* rt, Value thisVal, Value* a, uint32_t n) {
    String* s = rt->toStringObj(thisVal);
    String* needle = n ? rt->toStringObj(a[0]) : rt->intern("undefined");
    if (needle->len > s->len) return Value::fromSmi(-1);
    for (uint32_t i = 0; i + needle->len <= s->len; i++) {
        if (std::memcmp(s->data() + i, needle->data(), needle->len * 2) == 0)
            return Value::fromSmi((int32_t)i);
    }
    return Value::fromSmi(-1);
}
static Value native_str_substring(Runtime* rt, Value thisVal, Value* a, uint32_t n) {
    String* s = rt->toStringObj(thisVal);
    int32_t len = (int32_t)s->len;
    int32_t start = 0, end = len;
    if (n) { double v = a[0].asNumber(); start = std::isnan(v) ? 0 : (int32_t)v; }
    if (n > 1) { double v = a[1].asNumber(); end = std::isnan(v) ? len : (int32_t)v; }
    if (start < 0) start = 0; if (start > len) start = len;
    if (end < 0) end = 0; if (end > len) end = len;
    if (start > end) { int32_t t = start; start = end; end = t; }
    return Value::fromObj((HeapObj*)stringFromUTF16(rt, s->data() + start, (uint32_t)(end - start)));
}
static Value native_str_slice(Runtime* rt, Value thisVal, Value* a, uint32_t n) {
    String* s = rt->toStringObj(thisVal);
    int32_t len = (int32_t)s->len;
    int32_t start = 0, end = len;
    if (n) { double v = a[0].asNumber(); start = v < 0 ? (int32_t)(len + v) : (int32_t)v; }
    if (n > 1) { double v = a[1].asNumber(); end = v < 0 ? (int32_t)(len + v) : (int32_t)v; }
    if (start < 0) start = 0; if (start > len) start = len;
    if (end < 0) end = 0; if (end > len) end = len;
    if (start > end) return Value::fromObj((HeapObj*)rt->intern(""));
    return Value::fromObj((HeapObj*)stringFromUTF16(rt, s->data() + start, (uint32_t)(end - start)));
}
static Value native_str_split(Runtime* rt, Value thisVal, Value* a, uint32_t n) {
    Object* out = Object::create(rt, rt->arrayProto, true);
    String* s = rt->toStringObj(thisVal);
    String* sep = n ? rt->toStringObj(a[0]) : rt->intern("undefined");
    std::vector<String*> parts;
    if (sep->len == 0) {
        for (uint32_t i = 0; i < s->len; i++)
            parts.push_back(stringFromUTF16(rt, s->data() + i, 1));
    } else {
        uint32_t start = 0;
        for (uint32_t i = 0; i + sep->len <= s->len;) {
            if (std::memcmp(s->data() + i, sep->data(), sep->len * 2) == 0) {
                parts.push_back(stringFromUTF16(rt, s->data() + start, i - start));
                i += sep->len;
                start = i;
            } else i++;
        }
        parts.push_back(stringFromUTF16(rt, s->data() + start, s->len - start));
    }
    if (!parts.empty()) {
        out->elements = (Value*)std::malloc(parts.size() * sizeof(Value));
        out->elemCap = (uint32_t)parts.size();
        out->elemLen = (uint32_t)parts.size();
        for (size_t i = 0; i < parts.size(); i++) out->elements[i] = Value::fromObj((HeapObj*)parts[i]);
    }
    return Value::fromObj((HeapObj*)out);
}
static Value native_str_toString(Runtime* rt, Value thisVal, Value*, uint32_t) {
    return Value::fromObj((HeapObj*)rt->toStringObj(thisVal));
}

// ------------------------------------------------------------- Number.prototype
static Value native_num_toString(Runtime* rt, Value thisVal, Value*, uint32_t) {
    double d = thisVal.isNumber() ? thisVal.asNumber() : rt->toDoubleForArith(thisVal);
    return Value::fromObj((HeapObj*)numberToString(rt, d));
}

// ------------------------------------------------------------- install
void installBuiltins(Runtime* rt) {
    // global functions
    rt->defineBuiltin(rt->globalObj, "print", native_print);
    rt->defineBuiltin(rt->globalObj, "parseInt", native_parseInt);
    rt->defineBuiltin(rt->globalObj, "parseFloat", native_parseFloat);
    rt->defineBuiltin(rt->globalObj, "isNaN", native_isNaN);
    rt->defineBuiltin(rt->globalObj, "Number", native_Number);
    rt->defineBuiltin(rt->globalObj, "String", native_String);
    rt->defineBuiltin(rt->globalObj, "Boolean", native_Boolean);
    {
        JSFunction* errCtor = rt->defineBuiltin(rt->globalObj, "Error", native_Error);
        errCtor->fn.isCtor = true; // `new Error(...)` supported
    }

    // console
    Object* console = Object::create(rt, rt->objectProto, false);
    rt->defineBuiltin(console, "log", native_console_log);
    rt->defineBuiltin(console, "error", native_console_error);
    rt->definePropertyStr(rt->globalObj, rt->intern("console"), Value::fromObj((HeapObj*)console));

    // Math
    Object* math = Object::create(rt, rt->objectProto, false);
    rt->defineBuiltin(math, "sqrt", native_math_sqrt);
    rt->defineBuiltin(math, "abs", native_math_abs);
    rt->defineBuiltin(math, "floor", native_math_floor);
    rt->defineBuiltin(math, "ceil", native_math_ceil);
    rt->defineBuiltin(math, "round", native_math_round);
    rt->defineBuiltin(math, "min", native_math_min);
    rt->defineBuiltin(math, "max", native_math_max);
    rt->defineBuiltin(math, "pow", native_math_pow);
    rt->defineBuiltin(math, "log", native_math_log);
    rt->defineBuiltin(math, "random", native_math_random);
    rt->definePropertyStr(math, rt->intern("PI"), Value::fromDouble(3.141592653589793));
    rt->definePropertyStr(math, rt->intern("E"), Value::fromDouble(2.718281828459045));
    rt->definePropertyStr(rt->globalObj, rt->intern("Math"), Value::fromObj((HeapObj*)math));

    // Object
    Object* objectCtor = Object::create(rt, rt->functionProto, false);
    rt->defineBuiltin(objectCtor, "keys", native_object_keys);
    rt->definePropertyStr(rt->globalObj, rt->intern("Object"), Value::fromObj((HeapObj*)objectCtor));

    // Array
    Object* arrayCtor = Object::create(rt, rt->functionProto, false);
    rt->defineBuiltin(arrayCtor, "isArray", native_array_isArray);
    {
        JSFunction* arrCtor = rt->defineBuiltin(rt->globalObj, "Array", native_Array);
        arrCtor->fn.isCtor = true;
    }
    rt->definePropertyStr(rt->globalObj, rt->intern("isArray"), Value::fromObj((HeapObj*)arrayCtor)); // keep legacy ref off global

    // Object.prototype
    rt->defineBuiltin(rt->objectProto, "toString", native_op_toString);
    rt->defineBuiltin(rt->objectProto, "valueOf", native_op_valueOf);
    rt->defineBuiltin(rt->objectProto, "hasOwnProperty", native_op_hasOwnProperty);

    // Array.prototype
    rt->defineBuiltin(rt->arrayProto, "push", native_arr_push);
    rt->defineBuiltin(rt->arrayProto, "pop", native_arr_pop);
    rt->defineBuiltin(rt->arrayProto, "join", native_arr_join);
    rt->defineBuiltin(rt->arrayProto, "indexOf", native_arr_indexOf);
    rt->defineBuiltin(rt->arrayProto, "includes", native_arr_includes);
    rt->defineBuiltin(rt->arrayProto, "slice", native_arr_slice);
    rt->defineBuiltin(rt->arrayProto, "forEach", native_arr_forEach);
    rt->defineBuiltin(rt->arrayProto, "map", native_arr_map);

    // String.prototype
    rt->defineBuiltin(rt->stringProto, "charAt", native_str_charAt);
    rt->defineBuiltin(rt->stringProto, "charCodeAt", native_str_charCodeAt);
    rt->defineBuiltin(rt->stringProto, "indexOf", native_str_indexOf);
    rt->defineBuiltin(rt->stringProto, "substring", native_str_substring);
    rt->defineBuiltin(rt->stringProto, "slice", native_str_slice);
    rt->defineBuiltin(rt->stringProto, "split", native_str_split);
    rt->defineBuiltin(rt->stringProto, "toString", native_str_toString);

    // Number.prototype
    rt->defineBuiltin(rt->numberProto, "toString", native_num_toString);
    rt->defineBuiltin(rt->numberProto, "valueOf", native_op_valueOf);
}

} // namespace ts
