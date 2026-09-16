// TurboScript Tier 0 — heap.cpp
#include "heap.h"
#include "runtime.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <new>

namespace ts {

// ---------------------------------------------------------------- Heap
void Heap::init(size_t gcThresholdBytes) {
    objs_ = nullptr;
    bytesLive_ = 0;
    gcThreshold_ = gcThresholdBytes;
}

HeapObj* Heap::allocRaw(size_t size, ObjKind kind) {
    size_t sz = (size + 15) & ~size_t(15);
    // calloc: heap objects are created without running constructors, so every
    // field must start zeroed (NSDMIs in Object/Dict/JSFunction rely on this).
    HeapObj* o = (HeapObj*)std::calloc(1, sz);
    if (!o) { gcRequested_ = true; std::abort(); } // OOM: abort for MVP (registered gap: OOM handling)
    o->size = (uint32_t)sz;
    o->kind = (uint8_t)kind;
    o->marked = false;
    o->gcNext = objs_;
    objs_ = o;
    bytesLive_ += sz;
    if (bytesLive_ > gcThreshold_) gcRequested_ = true;
    return o;
}

void Heap::markObj(HeapObj* o) {
    if (!o || o->marked) return;
    o->marked = true;
    // Children are marked by kind-specific visitors in the runtime
    // (object.cpp / interp.cpp) — see Runtime::markValue.
}

void Heap::markValue(Value v) {
    // Only real heap pointers (low bits 000 within tag space) get marked.
    if (v.isHeap()) markObj(v.asObj());
}

void Heap::resetMarks() {
    for (HeapObj* o = objs_; o; o = o->gcNext) o->marked = false;
}

size_t Heap::collect() {
    size_t freed = 0;
    HeapObj** link = &objs_;
    while (*link) {
        HeapObj* o = *link;
        if (!o->marked) {
            *link = o->gcNext;
            freed += o->size;
            std::free(o);
        } else {
            o->marked = false;
            link = &o->gcNext;
        }
    }
    bytesLive_ -= freed;
    if (bytesLive_ > gcThreshold_ / 2) gcThreshold_ = bytesLive_ * 2; // adapt
    return freed;
}

// ---------------------------------------------------------------- Strings
uint32_t stringHash(const String* s) {
    if (s->hash) return s->hash;
    uint32_t h = 2166136261u;
    const char16_t* p = s->data();
    for (uint32_t i = 0; i < s->len; i++) { h ^= (uint32_t)p[i]; h *= 16777619u; }
    h = h ? h : 1;
    // hash cache: String is heap-allocated but we can patch through const_cast
    const_cast<String*>(s)->hash = h;
    return h;
}

bool stringEquals(const String* a, const String* b) {
    if (a == b) return true;
    if (a->len != b->len || stringHash(a) != stringHash(b)) return false;
    return std::memcmp(a->data(), b->data(), a->len * 2) == 0;
}

int stringCompare(const String* a, const String* b) {
    uint32_t n = a->len < b->len ? a->len : b->len;
    const char16_t* pa = a->data(); const char16_t* pb = b->data();
    for (uint32_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
    }
    return a->len == b->len ? 0 : (a->len < b->len ? -1 : 1);
}

String* stringFromUTF16(Runtime* rt, const char16_t* s, uint32_t len);
String* stringFromAscii(Runtime* rt, const char* s) {
    uint32_t n = 0; while (s[n]) n++;
    String* out = (String*)rt->heap.allocRaw(sizeof(String) + (size_t)n * 2 + 2, ObjKind::String);
    out->len = n; out->hash = 0; out->isAscii = true;
    char16_t* d = out->data();
    for (uint32_t i = 0; i < n; i++) d[i] = (char16_t)(unsigned char)s[i];
    d[n] = 0;
    return out;
}

String* stringFromUTF16(Runtime* rt, const char16_t* s, uint32_t len) {
    String* out = (String*)rt->heap.allocRaw(sizeof(String) + (size_t)len * 2 + 2, ObjKind::String);
    out->len = len; out->hash = 0; out->isAscii = true;
    char16_t* d = out->data();
    bool ascii = true;
    for (uint32_t i = 0; i < len; i++) { d[i] = s[i]; if (s[i] >= 128) ascii = false; }
    out->isAscii = ascii;
    d[len] = 0;
    return out;
}

String* stringConcat(Runtime* rt, String* a, String* b) {
    uint32_t n = a->len + b->len;
    String* out = (String*)rt->heap.allocRaw(sizeof(String) + (size_t)n * 2 + 2, ObjKind::String);
    out->len = n; out->hash = 0;
    out->isAscii = a->isAscii && b->isAscii;
    std::memcpy(out->data(), a->data(), (size_t)a->len * 2);
    std::memcpy(out->data() + a->len, b->data(), (size_t)b->len * 2);
    out->data()[n] = 0;
    return out;
}

// ---------------------------------------------------------------- number -> string
// Shortest digits via %.{p}e round-trip, then ES §6.1.7.1 formatting.
String* numberToString(Runtime* rt, double x) {
    char buf[64];
    if (std::isnan(x)) return stringFromAscii(rt, "NaN");
    if (x == 0) return stringFromAscii(rt, "0");
    bool neg = x < 0 || (x == 0 && std::signbit(x));
    double ax = neg ? -x : x;
    if (std::isinf(ax)) return stringFromAscii(rt, neg ? "-Infinity" : "Infinity");

    // shortest round-tripping significant-digit string
    int k = 0; double n10 = 0; // k digits, decimal exponent n (value = 0.d1..dk * 10^n)
    char digits[24];
    for (int p = 1; p <= 17; p++) {
        std::snprintf(buf, sizeof buf, "%.*e", p - 1, ax);
        if (std::strtod(buf, nullptr) == ax) { k = p; break; }
    }
    if (k == 0) k = 17;
    // parse "d.dddde±XX"
    {
        char* e = std::strchr(buf, 'e');
        int expv = std::atoi(e + 1);
        char* w = digits;
        for (char* r = buf; r < e; r++) if (*r >= '0' && *r <= '9') *w++ = *r;
        // strip trailing zeros of digits (from %.Ne padding)
        while (w > digits + 1 && w[-1] == '0') w--;
        *w = 0;
        k = (int)(w - digits);
        n10 = (double)(expv + 1); // n: value = 0.digits × 10^n
    }
    char out[40]; int oi = 0;
    if (neg) out[oi++] = '-';
    int n = (int)n10;
    if (k <= n && n <= 21) {
        for (int i = 0; i < k; i++) out[oi++] = digits[i];
        for (int i = 0; i < n - k; i++) out[oi++] = '0';
    } else if (0 < n && n <= 21) {
        for (int i = 0; i < n; i++) out[oi++] = digits[i];
        out[oi++] = '.';
        for (int i = n; i < k; i++) out[oi++] = digits[i];
    } else if (-6 < n && n <= 0) {
        out[oi++] = '0'; out[oi++] = '.';
        for (int i = 0; i < -n; i++) out[oi++] = '0';
        for (int i = 0; i < k; i++) out[oi++] = digits[i];
    } else {
        out[oi++] = digits[0];
        if (k > 1) { out[oi++] = '.'; for (int i = 1; i < k; i++) out[oi++] = digits[i]; }
        out[oi++] = 'e';
        int e = n - 1;
        out[oi++] = e < 0 ? '-' : '+';
        int ae = e < 0 ? -e : e;
        char eb[12]; int el = 0;
        do { eb[el++] = (char)('0' + ae % 10); ae /= 10; } while (ae);
        while (el) out[oi++] = eb[--el];
    }
    out[oi] = 0;
    return stringFromAscii(rt, out);
}

// ---------------------------------------------------------------- string -> number
static bool isWs(char16_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' ||
           c == 0x00A0 || c == 0xFEFF || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
           c == 0x2029 || c == 0x1680 || c == 0x202F || c == 0x205F || c == 0x3000;
}

double stringToNumber(const String* s) {
    const char16_t* p = s->data();
    uint32_t b = 0, e = s->len;
    while (b < e && isWs(p[b])) b++;
    while (e > b && isWs(p[e - 1])) e--;
    if (b == e) return 0;
    uint32_t n = e - b;
    // ASCII-only fast rejection: numbers are ASCII
    char tmp[64];
    if (n >= 63) return std::nan("");
    bool ascii = true;
    for (uint32_t i = 0; i < n; i++) if (p[b + i] >= 128) { ascii = false; break; }
    if (!ascii) return std::nan("");
    for (uint32_t i = 0; i < n; i++) tmp[i] = (char)p[b + i];
    tmp[n] = 0;
    // radix prefixes (ES string->number grammar allows 0x/0o/0b)
    if (n > 2 && tmp[0] == '0') {
        char c = tmp[1] | 32;
        if (c == 'x' || c == 'o' || c == 'b') {
            char* end = nullptr;
            long long v = std::strtoll(tmp + 2, &end, c == 'x' ? 16 : c == 'o' ? 8 : 2);
            if (end != tmp + n) return std::nan("");
            return (double)v;
        }
    }
    char* end = nullptr;
    double v = std::strtod(tmp, &end);
    if (end != tmp + n) return std::nan("");
    return v;
}

} // namespace ts
