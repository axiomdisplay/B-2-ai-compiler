// TurboScript Tier 0 — heap.h
// Heap objects + non-moving mark-sweep GC.
//
// Design law compliance:
//   Rule 86 analog (interp side): the ONLY roots are well-known tracked
//   locations — the interpreter value stack (contiguous Value words), the
//   context chain of each live frame, the global object, the permanent
//   compile-time pool (shapes/bytecode/consts), and IC-cached prototype
//   holders. No raw pointers are hidden from the marker inside the Value stack.
//   Non-moving GC => no read barriers, no forwarding checks (Rule 87 N/A).
//   Safepoints: GC triggers only at allocation slow-path (Rule 88 analog).
#pragma once
#include "value.h"
#include <cstdlib>
#include <vector>

namespace ts {

// ObjKind / HeapObj / String are defined in value.h (shared layout with Value).

uint32_t stringHash(const String* s);
bool stringEquals(const String* a, const String* b);          // by value
int  stringCompare(const String* a, const String* b);         // code-unit order
String* stringConcat(class Runtime* rt, String* a, String* b);
String* stringFromAscii(class Runtime* rt, const char* s);    // convenience
String* stringFromUTF16(class Runtime* rt, const char16_t* s, uint32_t len);
String* numberToString(class Runtime* rt, double d);          // ES §6.1.7.1 (shortest repr)
double  stringToNumber(const String* s);                      // ES §7.1.3.1

// ---- GC ----
class Heap {
public:
    void init(size_t gcThresholdBytes = 16ull << 20);
    HeapObj* allocRaw(size_t size, ObjKind kind);
    size_t bytesAllocated() const { return bytesLive_; }
    void requestGC() { gcRequested_ = true; }
    bool gcRequested() const { return gcRequested_; }
    void clearGCRequest() { gcRequested_ = false; }

    // Marker entry points (interp provides the root walker).
    void markValue(Value v);
    void markObj(HeapObj* o);
    // Sweeps everything not marked. Call after marking all roots.
    size_t collect();     // returns bytes freed
    void resetMarks();

    std::vector<HeapObj*> permanentObjs;   // pinned across collections (interned etc.)

private:
    HeapObj* objs_ = nullptr;
    size_t   bytesLive_ = 0;
    size_t   gcThreshold_;
    bool     gcRequested_ = false;
};

} // namespace ts
