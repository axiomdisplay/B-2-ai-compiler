// B-2 codegen - the Tier-1 execution engine: runtime helpers, code cache,
// compiled entry, deopt-to-T0, and exception dispatch into compiled frames.
//
// WHY THIS FILE EXISTS:
// A CompiledCode block is inert bytes. Something must enter it (the engine),
// service its helper calls (this file's extern "C" helpers: the v0 object
// model lives behind interp::Runtime and Rule-15 object ids, so compiled
// field/array/monitor/call code routes through C++), and honor its deopt
// exits by rebuilding a T0 Frame and calling Interpreter::resume (Amendment
// B.3; docs/deopt_backend.md Part A SS3). The helper ABI is the seam T2
// will reuse (docs/codegen_contract.md SS8).
//
// THE DEOPT PATH (SS9): every deopt exits compiled code with the activation
// filled in (deopt_pc, deopt_id, pending_exc, reenter). The engine then:
//   pending_exc != 0  -> exception dispatch: caught by a plan exception
//                        edge at the rbc pc (table order, catch-type match)
//                        -> reset regs, regs[0] = exc, re-enter compiled
//                        code at the handler's entry stub; not caught ->
//                        deopt to T0 with the pending exception (T0's
//                        exception algorithm unwinds further).
//   pending_exc == 0  -> guard failure (clean T0 re-execution) or the
//                        inline idiv/ldiv zero trap (ArithmeticException
//                        "/ by zero" built here, the only inline trap).
//
// SINGLE-THREAD v0: helpers reach the engine through a thread-local current
// pointer (the multi-threaded form is a documented future contract).

#include "compiler/codegen/src/CodeGenInternal.h"
#include "compiler/codegen/src/Helpers.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "b2/baseline/Compiler.h"
#include "b2/codegen/Archive.h"
#include "b2/codegen/Instantiate.h"
#include "b2/codegen/Tier1.h"
#include "b2/interp/Interp.h"
#include "b2/interp/Runtime.h"
#include "b2/rbc/Rbc.h"

namespace b2::codegen {

namespace {

// JVM exception classes + messages (Runtime::makeException pins).
constexpr std::string_view kClsArithmetic = "java/lang/ArithmeticException";
constexpr std::string_view kClsNullPointer = "java/lang/NullPointerException";
constexpr std::string_view kClsClassCast = "java/lang/ClassCastException";
constexpr std::string_view kClsArrayStore = "java/lang/ArrayStoreException";
constexpr std::string_view kClsNegativeArraySize =
    "java/lang/NegativeArraySizeException";
constexpr std::string_view kClsIllegalMonitor =
    "java/lang/IllegalMonitorStateException";
constexpr std::string_view kClsStackOverflow = "java/lang/StackOverflowError";
constexpr std::string_view kClsNoSuchMethod = "java/lang/NoSuchMethodError";
constexpr std::string_view kClsInternalError = "java/lang/InternalError";

[[nodiscard]] std::string decimal(std::int64_t v) {
  return std::to_string(v);
}

} // namespace

// --- the engine internals -----------------------------------------------------------

struct Tier1::Impl {
  std::unordered_map<std::uint32_t, std::unique_ptr<CompiledCode>> cache;
  Archive archive;
  bool archiveOk = false;
  std::uint32_t depth = 0; // helper-recursion depth (StackOverflow budget)
  Tier1Stats* stats = nullptr;      // the owning engine's counters
  interp::Runtime* rt = nullptr;    // shared runtime (heap/statics/profiles)
  interp::Interpreter* interp = nullptr; // the T0 fallback + deopt target
  const rbc::Program* program = nullptr;
  const baseline::StencilSet* set = nullptr; // the manifest plans build on
  baseline::CompileOptions planOptions{};
};

// Forward declarations (helpers -> engine -> helpers).
[[nodiscard]] Tier1RunResult engineExecuteMethod(
    Tier1::Impl* impl, std::uint32_t methodIndex,
    std::span<const interp::Value> args, bool* compiled);

// The thread-local current engine (single-threaded v0; helpers' only route
// back to execution services).
thread_local Tier1::Impl* g_current = nullptr;



// --- small helper utilities (activation access) ----------------------------------

[[nodiscard]] inline T1Activation* actOf(void* act) noexcept {
  return static_cast<T1Activation*>(act);
}

[[nodiscard]] inline interp::Value* slotAt(T1Activation* act,
                                           std::uint32_t off) noexcept {
  return reinterpret_cast<interp::Value*>(reinterpret_cast<std::uint8_t*>(act) +
                                          off);
}

[[nodiscard]] inline interp::Runtime& rtOf(T1Activation*) {
  return *g_current->rt;
}

[[nodiscard]] inline CompiledCode* codeOf(T1Activation* act) noexcept {
  return static_cast<CompiledCode*>(act->code);
}

// Writes a Value into an activation slot (payload then tag).
inline void writeSlot(T1Activation* act, std::uint32_t off,
                      const interp::Value& v) noexcept {
  *slotAt(act, off) = v;
}

// Trap: build the exception, stash it, return the kind (never 0).
[[nodiscard]] std::uint32_t trap(T1Activation* act, TrapKind kind,
                                 std::string_view cls,
                                 std::string_view msg) {
  interp::ObjRef exc = rtOf(act).makeException(cls, msg);
  act->pending_exc = exc.id;
  act->trap_kind = static_cast<std::uint64_t>(kind);
  return static_cast<std::uint32_t>(kind);
}

[[nodiscard]] std::uint32_t trapWith(T1Activation* act, TrapKind kind,
                                     interp::ObjRef exc) {
  act->pending_exc = exc.id;
  act->trap_kind = static_cast<std::uint64_t>(kind);
  return static_cast<std::uint32_t>(kind);
}

// --- class initialization (JVMS 5.5; mirrors initClassIfNeeded) ---------------------

[[nodiscard]] bool initClassIfNeeded(T1Activation* act,
                                        interp::ClassId cls) {
  interp::Runtime& rt = rtOf(act);
  if (!rt.needsInit(cls)) {
    return true;
  }
  // findClinit contract subtlety: needsInit()==true IMPLIES an RBC <clinit>
  // exists (Runtime.cpp pins it), so the returned id is usable even when
  // v == 0 (method 0 can BE <clinit>); the bounds check is defensive only.
  const interp::MethodId clinit = rt.findClinit(cls);
  rt.markInitialized(cls); // BEFORE running (recursion terminates)
  if (clinit.v >= g_current->program->methods.size()) {
    return true; // defensive totality: no reachable <clinit>
  }
  const Tier1RunResult r = engineExecuteMethod(g_current, clinit.v, {},
                                               nullptr);
  if (r.status == Tier1Status::Threw) {
    (void)trapWith(act, TrapKind::Thrown, r.exception);
    return false;
  }
  return true;
}

// --- the runtime helpers (SS8) -------------------------------------------------------

std::uint32_t b2cg_get_field(T1Activation* act, std::uint32_t objSlotOff,
                             std::uint32_t fieldOffAndType,
                             std::uint32_t dstSlotOff) {
  ++g_current->stats->helper_calls;
  const interp::Value& obj = *slotAt(act, objSlotOff);
  if (obj.isNull()) {
    return trap(act, TrapKind::Npe, kClsNullPointer, "");
  }
  interp::Runtime& rt = rtOf(act);
  const std::uint32_t fieldOff = fieldOffAndType & 0x0FFF'FFFFu;
  const rbc::RType fieldType =
      static_cast<rbc::RType>((fieldOffAndType >> 28) & 0xFu);
  const std::uint32_t slot = rt.slotOfFieldOffset(fieldOff);
  interp::Value v = rt.heap().loadField(obj.ref(), slot);
  if (v.type == rbc::RType::Bottom) {
    // A5: Bottom = "never written" -> the JLS 4.12.5 default through the
    // resolved descriptor. No type bits (quickened corner): the same honest
    // refusal T0 pins ("write the field before reading it quickened").
    if (!rt.heap().isInstance(obj.ref())) {
      return trap(act, TrapKind::Npe, kClsInternalError,
                  "field access on non-instance");
    }
    switch (fieldType) {
      case rbc::RType::Int:
        v = interp::Value::intVal(0);
        break;
      case rbc::RType::Long:
        v = interp::Value::longVal(0);
        break;
      case rbc::RType::Float:
        v = interp::Value::floatVal(0.0F);
        break;
      case rbc::RType::Double:
        v = interp::Value::doubleVal(0.0);
        break;
      case rbc::RType::Null:
      case rbc::RType::Ref:
        v = interp::Value::nullVal();
        break;
      default:
        return trap(act, TrapKind::Npe, kClsInternalError,
                    "quickened getfield of unwritten field (v0)");
    }
  }
  writeSlot(act, dstSlotOff, v);
  return 0;
}

std::uint32_t b2cg_put_field(T1Activation* act, std::uint32_t objSlotOff,
                             std::uint32_t fieldOff, std::uint32_t valSlotOff) {
  ++g_current->stats->helper_calls;
  const interp::Value& obj = *slotAt(act, objSlotOff);
  if (obj.isNull()) {
    return trap(act, TrapKind::Npe, kClsNullPointer, "");
  }
  interp::Runtime& rt = rtOf(act);
  const std::uint32_t slot = rt.slotOfFieldOffset(fieldOff);
  if (!rt.heap().storeField(obj.ref(), slot, *slotAt(act, valSlotOff))) {
    return trap(act, TrapKind::Npe, kClsInternalError,
                "field access on non-instance");
  }
  return 0;
}

std::uint32_t b2cg_get_static(T1Activation* act,
                              std::uint32_t fieldIdOrBuiltin,
                              std::uint32_t dstSlotOff) {
  ++g_current->stats->helper_calls;
  interp::Runtime& rt = rtOf(act);
  if (fieldIdOrBuiltin >= kStaticBuiltinBase) {
    // Instantiation resolved a builtin singleton (System.out/err): the
    // ObjRef id travels in the low bits; ids are Runtime-stable (Rule 15).
    const interp::ObjRef obj{fieldIdOrBuiltin & ~kStaticBuiltinBase};
    writeSlot(act, dstSlotOff, interp::Value::refVal(obj));
    return 0;
  }
  const interp::FieldId fid{fieldIdOrBuiltin};
  // JVMS 5.5 trigger: in the v0 one-class world the only class with statics
  // is the program class (builtinStatics are handled above), and needsInit
  // gates on it internally (the T0 BE-2 pin); call and trust it.
  const interp::ClassId progClass =
      rt.classId(g_current->program->className);
  if (!initClassIfNeeded(act, progClass)) {
    return static_cast<std::uint32_t>(TrapKind::Thrown);
  }
  interp::Value v = rt.loadStatic(fid);
  if (v.type == rbc::RType::Bottom) {
    // A5: unset statics default to the field type's zero (JLS 4.12.5). The
    // field's RType is recoverable from the declaring descriptor; v0
    // verified streams only ever observe this through the dump path, and
    // the interpreter's own Bottom read-back pins the zero-Int form.
    v = interp::Value::intVal(0);
  }
  writeSlot(act, dstSlotOff, v);
  return 0;
}

std::uint32_t b2cg_put_static(T1Activation* act, std::uint32_t fieldId,
                              std::uint32_t valSlotOff) {
  ++g_current->stats->helper_calls;
  interp::Runtime& rt = rtOf(act);
  const interp::ClassId progClass =
      rt.classId(g_current->program->className);
  if (!initClassIfNeeded(act, progClass)) {
    return static_cast<std::uint32_t>(TrapKind::Thrown);
  }
  rt.storeStatic(interp::FieldId{fieldId}, *slotAt(act, valSlotOff));
  return 0;
}

namespace {

// Shared bounds/NPE prologue for array element access.
[[nodiscard]] bool arrayPrologue(T1Activation* act, std::uint32_t arrSlotOff,
                                 std::uint32_t idxSlotOff, std::int32_t& idx,
                                 std::uint32_t& len) {
  const interp::Value& arr = *slotAt(act, arrSlotOff);
  if (arr.isNull()) {
    (void)trap(act, TrapKind::Npe, kClsNullPointer, "");
    return false;
  }
  idx = slotAt(act, idxSlotOff)->as.i;
  len = rtOf(act).heap().arrayLength(arr.ref());
  if (idx < 0 || static_cast<std::uint32_t>(idx) >= len) {
    (void)trap(act, TrapKind::ArrayBounds,
               "java/lang/ArrayIndexOutOfBoundsException",
               "Index " + decimal(idx) + " out of bounds for length " +
                   decimal(len));
    return false;
  }
  return true;
}

} // namespace

std::uint32_t b2cg_array_load(T1Activation* act, std::uint32_t arrSlotOff,
                              std::uint32_t idxSlotOff, std::uint32_t dstSlotOff,
                              std::uint32_t elemKind) {
  (void)elemKind; // loads are raw (the narrowing is store-side; T0 pin)
  ++g_current->stats->helper_calls;
  std::int32_t idx = 0;
  std::uint32_t len = 0;
  if (!arrayPrologue(act, arrSlotOff, idxSlotOff, idx, len)) {
    return static_cast<std::uint32_t>(
        static_cast<T1Activation*>(act)->trap_kind);
  }
  const interp::Value& arr = *slotAt(act, arrSlotOff);
  const interp::Value v =
      rtOf(act).heap().loadElem(arr.ref(), static_cast<std::uint32_t>(idx));
  if (v.type == rbc::RType::Bottom) {
    return trap(act, TrapKind::ArrayBounds, kClsInternalError,
                "array element access on non-array");
  }
  writeSlot(act, dstSlotOff, v);
  return 0;
}

std::uint32_t b2cg_array_store(T1Activation* act, std::uint32_t arrSlotOff,
                               std::uint32_t idxSlotOff, std::uint32_t valSlotOff,
                               std::uint32_t elemKind) {
  ++g_current->stats->helper_calls;
  std::int32_t idx = 0;
  std::uint32_t len = 0;
  if (!arrayPrologue(act, arrSlotOff, idxSlotOff, idx, len)) {
    return static_cast<std::uint32_t>(
        static_cast<T1Activation*>(act)->trap_kind);
  }
  interp::Value v = *slotAt(act, valSlotOff); // dst READ (spec pin P3)
  switch (elemKind) { // Atype codes; narrowing is store-side (rbc SS3.13)
    case 8: // byte
      v = interp::Value::intVal(
          static_cast<std::int32_t>(static_cast<std::int8_t>(v.as.i)));
      break;
    case 5: // char
      v = interp::Value::intVal(
          static_cast<std::int32_t>(static_cast<std::uint16_t>(v.as.i)));
      break;
    case 9: // short
      v = interp::Value::intVal(
          static_cast<std::int32_t>(static_cast<std::int16_t>(v.as.i)));
      break;
    default:
      break;
  }
  const interp::Value& arr = *slotAt(act, arrSlotOff);
  if (!rtOf(act).heap().storeElem(arr.ref(), static_cast<std::uint32_t>(idx),
                                  v)) {
    return trap(act, TrapKind::ArrayBounds, kClsInternalError,
                "array element access on non-array");
  }
  return 0;
}

std::uint32_t b2cg_array_length(T1Activation* act, std::uint32_t arrSlotOff,
                                std::uint32_t dstSlotOff) {
  ++g_current->stats->helper_calls;
  const interp::Value& arr = *slotAt(act, arrSlotOff);
  if (arr.isNull()) {
    return trap(act, TrapKind::Npe, kClsNullPointer, "");
  }
  interp::Runtime& rt = rtOf(act);
  if (!rt.heap().isArray(arr.ref())) {
    return trap(act, TrapKind::Npe, kClsInternalError,
                "arraylength on non-array");
  }
  writeSlot(act, dstSlotOff,
            interp::Value::intVal(
                static_cast<std::int32_t>(rt.heap().arrayLength(arr.ref()))));
  return 0;
}

std::uint32_t b2cg_new_object(T1Activation* act, std::uint32_t classId,
                              std::uint32_t dstSlotOff) {
  ++g_current->stats->helper_calls;
  interp::Runtime& rt = rtOf(act);
  const interp::ClassId cls{classId};
  if (!initClassIfNeeded(act, cls)) {
    return static_cast<std::uint32_t>(TrapKind::Thrown);
  }
  const interp::ObjRef obj = rt.newInstance(cls);
  writeSlot(act, dstSlotOff, interp::Value::refVal(obj));
  return 0;
}

std::uint32_t b2cg_new_array(T1Activation* act, std::uint32_t lenSlotOff,
                             std::uint32_t atypeOrClassId, std::uint32_t dstSlotOff,
                             std::uint32_t flags) {
  ++g_current->stats->helper_calls;
  interp::Runtime& rt = rtOf(act);
  const std::int32_t len = slotAt(act, lenSlotOff)->as.i;
  if (len < 0) {
    return trap(act, TrapKind::NegativeArraySize, kClsNegativeArraySize,
                decimal(len));
  }
  interp::ObjRef arr{};
  if ((flags & 1u) != 0) { // ref array: the id is a ClassId
    const interp::ClassId elem{atypeOrClassId};
    if (!initClassIfNeeded(act, elem)) {
      return static_cast<std::uint32_t>(TrapKind::Thrown);
    }
    arr = rt.newRefArray(elem, static_cast<std::uint32_t>(len));
  } else { // primitive array: the id is an Atype code
    arr = rt.newArray(static_cast<rbc::Atype>(atypeOrClassId),
                      static_cast<std::uint32_t>(len));
  }
  writeSlot(act, dstSlotOff, interp::Value::refVal(arr));
  return 0;
}

std::uint32_t b2cg_check_cast(T1Activation* act, std::uint32_t classId,
                              std::uint32_t srcSlotOff, std::uint32_t dstSlotOff) {
  ++g_current->stats->helper_calls;
  const interp::Value& v = *slotAt(act, srcSlotOff);
  if (v.isNull()) { // null passes every cast unchanged (JLS 5.5)
    writeSlot(act, dstSlotOff, v);
    return 0;
  }
  interp::Runtime& rt = rtOf(act);
  const interp::ClassId target{classId};
  const interp::ClassId actual = rt.heap().classOf(v.ref());
  if (rt.isAssignableFrom(target, actual)) {
    writeSlot(act, dstSlotOff, v);
    return 0;
  }
  return trap(act, TrapKind::ClassCast, kClsClassCast,
              "class " + rt.dottedClassName(actual) + " cannot be cast to class " +
                  rt.dottedClassName(target));
}

std::uint32_t b2cg_instance_of(T1Activation* act, std::uint32_t classId,
                               std::uint32_t srcSlotOff, std::uint32_t dstSlotOff) {
  ++g_current->stats->helper_calls;
  const interp::Value& v = *slotAt(act, srcSlotOff);
  if (v.isNull()) { // null is never an instance
    writeSlot(act, dstSlotOff, interp::Value::intVal(0));
    return 0;
  }
  interp::Runtime& rt = rtOf(act);
  const interp::ClassId target{classId};
  const interp::ClassId actual = rt.heap().classOf(v.ref());
  writeSlot(act, dstSlotOff,
            interp::Value::intVal(rt.isAssignableFrom(target, actual) ? 1 : 0));
  return 0;
}

std::uint32_t b2cg_monitor_enter(T1Activation* act, std::uint32_t objSlotOff) {
  ++g_current->stats->helper_calls;
  const interp::Value& v = *slotAt(act, objSlotOff);
  if (v.isNull()) {
    return trap(act, TrapKind::Npe, kClsNullPointer, "");
  }
  interp::Runtime& rt = rtOf(act);
  const interp::Heap::MonFail mf = rt.heap().monitorenter(v.ref());
  if (mf == interp::Heap::MonFail::Null) {
    return trap(act, TrapKind::Npe, kClsNullPointer, "");
  }
  if (mf == interp::Heap::MonFail::NotOwner) {
    return trap(act, TrapKind::Monitor, kClsIllegalMonitor, "");
  }
  // Frame.h pins the record MOST-RECENT-FIRST: index 0 is the freshest.
  if (act->mon_count >= kT1MaxMonitors) {
    return trap(act, TrapKind::Monitor, kClsInternalError,
                "T1 monitor record overflow");
  }
  for (std::uint32_t i = kT1MaxMonitors - 1; i > 0; --i) {
    act->mon_ids[i] = act->mon_ids[i - 1];
  }
  act->mon_ids[0] = v.as.obj;
  ++act->mon_count;
  return 0;
}

std::uint32_t b2cg_monitor_exit(T1Activation* act, std::uint32_t objSlotOff) {
  ++g_current->stats->helper_calls;
  const interp::Value& v = *slotAt(act, objSlotOff);
  if (v.isNull()) {
    return trap(act, TrapKind::Npe, kClsNullPointer, "");
  }
  interp::Runtime& rt = rtOf(act);
  const interp::Heap::MonFail mf = rt.heap().monitorexit(v.ref());
  if (mf == interp::Heap::MonFail::Null ||
      mf == interp::Heap::MonFail::NotOwner) {
    return trap(act, TrapKind::Monitor, kClsIllegalMonitor, "");
  }
  // LIFO record: remove the front-most match.
  for (std::uint32_t i = 0; i < act->mon_count; ++i) {
    if (act->mon_ids[i] == v.as.obj) {
      for (std::uint32_t j = i; j + 1 < act->mon_count; ++j) {
        act->mon_ids[j] = act->mon_ids[j + 1];
      }
      --act->mon_count;
      break;
    }
  }
  return 0;
}

std::uint32_t b2cg_athrow(T1Activation* act, std::uint32_t excSlotOff) {
  ++g_current->stats->helper_calls;
  const interp::Value& v = *slotAt(act, excSlotOff);
  if (v.isNull()) { // JLS 14.18: athrow null raises NPE
    return trap(act, TrapKind::Npe, kClsNullPointer, "");
  }
  return trapWith(act, TrapKind::Thrown, v.ref());
}

std::uint32_t b2cg_call(T1Activation* act, std::uint32_t argBaseOff,
                        std::uint32_t argCount, std::uint32_t packedTarget,
                        std::uint32_t dstSlotOff) {
  ++g_current->stats->helper_calls;
  Tier1::Impl* impl = g_current;
  interp::Runtime& rt = *impl->rt;
  const std::uint32_t flavor = packedTarget >> kCallFlavorShift;
  const std::uint32_t id = packedTarget & 0x0FFF'FFFFu;
  CompiledCode* cc = codeOf(act);
  const rbc::Method& caller = impl->program->methods[cc->method_index];
  const std::span<const interp::Value> args(slotAt(act, argBaseOff),
                                            argCount);

  // Depth budget: native-stack exhaustion becomes a Java-visible error.
  if (impl->depth >= kT1MaxCallDepthDefault) {
    return trap(act, TrapKind::StackOverflow, kClsStackOverflow, "");
  }

  std::uint32_t targetIdx = 0xFFFFFFFFu;
  bool isBuiltin = false;
  interp::Runtime::Builtin builtin = interp::Runtime::Builtin::None;

  if (flavor == kCallFlavorVirtual) {
    // Receiver first (Interp.h call protocol): null -> NPE at the call site.
    if (args.empty() || args.front().isNull()) {
      return trap(act, TrapKind::Npe, kClsNullPointer, "");
    }
    const rbc::Const& ref = caller.cp[id];
    builtin = rt.lookupBuiltinVirtual(rt.heap().classOf(args.front().ref()),
                                      ref.str2, ref.str3);
    if (builtin != interp::Runtime::Builtin::None) {
      isBuiltin = true;
    } else {
      const std::optional<interp::MethodId> mid = rt.resolveMethod(ref);
      if (!mid) {
        return trap(act, TrapKind::NoSuchMethod, kClsNoSuchMethod,
                    std::string(ref.str) + "." + ref.str2 + ref.str3);
      }
      targetIdx = mid->v;
    }
  } else {
    if (flavor == kCallFlavorStatic) {
      // JVMS 5.5 first-use trigger (the class of a program method).
      const interp::ClassId progClass =
      rt.classId(impl->program->className);
      if (!initClassIfNeeded(act, progClass)) {
        return static_cast<std::uint32_t>(TrapKind::Thrown);
      }
    }
    targetIdx = id;
  }

  if (isBuiltin) {
    if (!rt.execBuiltin(builtin, args)) {
      return trap(act, TrapKind::NoSuchMethod, kClsInternalError,
                  "builtin arity mismatch");
    }
    return 0; // println/print return void; no dst write
  }

  const rbc::Method& callee = impl->program->methods[targetIdx];
  if (callee.isAbstract() || (callee.flags & rbc::method_flags::Native) != 0) {
    return trap(act, TrapKind::NoSuchMethod, kClsNoSuchMethod,
                impl->program->className + "." + callee.name +
                    callee.descriptor);
  }

  // Execute the callee (compiled or T0; the engine decides, memoized).
  bool compiled = false;
  const Tier1RunResult r =
      engineExecuteMethod(impl, targetIdx, args, &compiled);
  if (r.status == Tier1Status::Threw) {
    return trapWith(act, TrapKind::Thrown, r.exception);
  }
  if (r.status != Tier1Status::Returned) {
    return trap(act, TrapKind::NoSuchMethod, kClsInternalError,
                "callee execution failed");
  }
  // Result: write iff the callee's return type is non-void.
  if (rbc::parseReturn(callee.descriptor) != rbc::RType::Bottom) {
    writeSlot(act, dstSlotOff, r.result);
  }
  return 0;
}

std::uint32_t b2cg_ldc_const(T1Activation* act, std::uint32_t cpIndex,
                             std::uint32_t dstSlotOff) {
  ++g_current->stats->helper_calls;
  Tier1::Impl* impl = g_current;
  interp::Runtime& rt = *impl->rt;
  const rbc::Const& c =
      impl->program->methods[codeOf(act)->method_index].cp[cpIndex];
  switch (c.kind) {
    case rbc::Const::Kind::String:
      // JVMS 5.1 interning: equal constants are one object.
      writeSlot(act, dstSlotOff, interp::Value::refVal(rt.internString(c.str)));
      return 0;
    case rbc::Const::Kind::Class: {
      const interp::ClassId cls = rt.classId(c.str);
      if (!initClassIfNeeded(act, cls)) {
        return static_cast<std::uint32_t>(TrapKind::Thrown);
      }
      writeSlot(act, dstSlotOff,
                interp::Value::refVal(rt.classObject(cls)));
      return 0;
    }
    case rbc::Const::Kind::MethodType:
    case rbc::Const::Kind::MethodHandle:
      // No method-handle runtime in v0: honest refusal, never silent.
      return trap(act, TrapKind::Thrown, kClsInternalError,
                  "ldc of methodtype/methodhandle not supported in v0");
    default:
      return trap(act, TrapKind::Thrown, kClsInternalError,
                  "ldc of an unsupported constant kind");
  }
}

float b2cg_fmod_f(T1Activation* act, float a, float b) {
  (void)act;
  return std::fmod(a, b);
}

double b2cg_fmod_d(T1Activation* act, double a, double b) {
  (void)act;
  return std::fmod(a, b);
}

void* helperAddress(std::uint8_t helperId) noexcept {
  switch (static_cast<HelperId>(helperId)) {
    case HelperId::GetField:
      return reinterpret_cast<void*>(&b2cg_get_field);
    case HelperId::PutField:
      return reinterpret_cast<void*>(&b2cg_put_field);
    case HelperId::GetStatic:
      return reinterpret_cast<void*>(&b2cg_get_static);
    case HelperId::PutStatic:
      return reinterpret_cast<void*>(&b2cg_put_static);
    case HelperId::ArrayLoad:
      return reinterpret_cast<void*>(&b2cg_array_load);
    case HelperId::ArrayStore:
      return reinterpret_cast<void*>(&b2cg_array_store);
    case HelperId::ArrayLength:
      return reinterpret_cast<void*>(&b2cg_array_length);
    case HelperId::NewObject:
      return reinterpret_cast<void*>(&b2cg_new_object);
    case HelperId::NewArray:
      return reinterpret_cast<void*>(&b2cg_new_array);
    case HelperId::CheckCast:
      return reinterpret_cast<void*>(&b2cg_check_cast);
    case HelperId::InstanceOf:
      return reinterpret_cast<void*>(&b2cg_instance_of);
    case HelperId::MonitorEnter:
      return reinterpret_cast<void*>(&b2cg_monitor_enter);
    case HelperId::MonitorExit:
      return reinterpret_cast<void*>(&b2cg_monitor_exit);
    case HelperId::Athrow:
      return reinterpret_cast<void*>(&b2cg_athrow);
    case HelperId::Call:
      return reinterpret_cast<void*>(&b2cg_call);
    case HelperId::LdcConst:
      return reinterpret_cast<void*>(&b2cg_ldc_const);
    case HelperId::FmodF:
      return reinterpret_cast<void*>(&b2cg_fmod_f);
    case HelperId::FmodD:
      return reinterpret_cast<void*>(&b2cg_fmod_d);
    case HelperId::None:
    case HelperId::Count:
      break;
  }
  return nullptr;
}

// --- the engine's compiled execution ---------------------------------------------------

using EntryFn = std::uint32_t (*)(void*);

// Builds the T0 fallback frame for a method invocation.
[[nodiscard]] interp::Frame t0Frame(const rbc::Method& m, std::uint32_t pc,
                                    std::span<const interp::Value> args) {
  interp::Frame f;
  f.method = &m;
  f.pc = pc;
  f.locals.assign(args.begin(), args.end());
  f.locals.resize(m.numLocals, interp::Value::bottom());
  f.regs.assign(m.numRegs, interp::Value::bottom());
  return f;
}

[[nodiscard]] Tier1RunResult mapRun(const interp::RunResult& r) {
  Tier1RunResult out;
  switch (r.status) {
    case interp::RunStatus::Returned:
      out.status = Tier1Status::Returned;
      out.result = r.result;
      break;
    case interp::RunStatus::Threw:
      out.status = Tier1Status::Threw;
      out.exception = r.exception;
      break;
    case interp::RunStatus::VerifyFailed:
      out.status = Tier1Status::VerifyFailed;
      out.verify_diags = r.verifyDiags;
      break;
  }
  return out;
}

[[nodiscard]] Tier1RunResult executeCompiled(Tier1::Impl* impl,
                                             CompiledCode* cc,
                                             std::span<const interp::Value> args);

// One method invocation on the engine (compiled when possible, T0 otherwise).
[[nodiscard]] Tier1RunResult engineExecuteMethod(
    Tier1::Impl* impl, std::uint32_t methodIndex,
    std::span<const interp::Value> args, bool* compiled) {
  const rbc::Program& program = *impl->program;
  const rbc::Method& m = program.methods[methodIndex];
  impl->rt->bumpInvocations(interp::MethodId{methodIndex});

  // 1. Compiled path (cache hit or fresh instantiation).
  auto it = impl->cache.find(methodIndex);
  if (it == impl->cache.end()) {
    ++impl->stats->compile_attempts;
    const baseline::PlanResult pr =
        baseline::compilePlan(program, methodIndex, *impl->set,
                              impl->planOptions);
    if (!pr.ok) {
      ++impl->stats->plan_refusals;
    } else {
      InstantiationResult ir =
          instantiate(program, methodIndex, pr.plan, *impl->set,
                      impl->archive, *impl->rt);
      if (ir.ok()) {
        impl->stats->code_bytes += ir.code->code.size();
        ++impl->stats->compile_ok;
        it = impl->cache.emplace(methodIndex, std::move(ir.code)).first;
      } else {
        ++impl->stats->instantiation_refusals;
      }
    }
  }
  if (it != impl->cache.end()) {
    if (compiled != nullptr) {
      *compiled = true;
    }
    return executeCompiled(impl, it->second.get(), args);
  }

  // 2. T0 fallback (Rule 96: the plan is a cache, not a correctness claim).
  if (compiled != nullptr) {
    *compiled = false;
  }
  ++impl->stats->t0_fallback_executions;
  const interp::RunResult r = impl->interp->resume(t0Frame(m, 0, args));
  return mapRun(r);
}

[[nodiscard]] Tier1RunResult executeCompiled(Tier1::Impl* impl,
                                             CompiledCode* cc,
                                             std::span<const interp::Value> args) {
  const rbc::Method& m = impl->program->methods[cc->method_index];

  // Activation: zeroed control block + args in locals + Bottom regs.
  const std::size_t bytes = activationBytes(cc->num_locals, cc->num_regs);
  std::vector<std::uint8_t> buffer(bytes, 0);
  T1Activation* act = reinterpret_cast<T1Activation*>(buffer.data());
  act->magic = kT1ActivationMagic;
  act->code = cc;
  act->status = kStatusNormal;
  act->mon_count = 0;
  interp::Value* slots = act->slots;
  for (std::uint32_t i = 0; i < cc->num_locals; ++i) {
    slots[i] = i < args.size() ? args[i] : interp::Value::bottom();
  }
  for (std::uint32_t i = 0; i < cc->num_regs; ++i) {
    slots[cc->num_locals + i] = interp::Value::bottom();
  }

  ++impl->stats->t1_entries;
  ++impl->depth;
  struct DepthGuard {
    Tier1::Impl* impl;
    ~DepthGuard() { --impl->depth; }
  } guard{impl};

  // The engine pointer is per-invocation (save/restore for nesting safety
  // with multiple engine objects in one thread - tests do this).
  Tier1::Impl* prev = g_current;
  g_current = impl;

  std::uint32_t rax = 0;
  std::uint32_t entryOffset = cc->entries.empty() ? 0
                           : cc->entries.front().native_offset;
  for (;;) {
    EntryFn entry = reinterpret_cast<EntryFn>(cc->exec_base + entryOffset);
    rax = entry(act);
    if (rax == kExitNormal) {
      break;
    }
    // Deopt. Exception dispatch first (uniform pending-exc path).
    if (act->pending_exc != 0) {
      // Rule 119: the reason is observable. Exception deopts split into
      // call-site (CallException points) and trap-site (Thrown) kinds.
      const RealDeoptPoint* why =
          cc->deoptPoint(static_cast<std::uint32_t>(act->deopt_id));
      if (why != nullptr &&
          why->reason == baseline::DeoptReason::CallException) {
        ++impl->stats->deopt_call_exception;
      } else {
        ++impl->stats->deopt_trap;
      }
      const interp::ObjRef exc{static_cast<std::uint32_t>(act->pending_exc)};
      const std::uint32_t rbcPc = cc->rbcPcAt(
          static_cast<std::uint32_t>(act->deopt_pc));
      const baseline::StencilPlan& plan = cc->plan;
      interp::Runtime& rt = *impl->rt;
      bool caught = false;
      for (const baseline::ExceptionEdge& e : plan.exception_edges) {
        if (rbcPc < e.start || rbcPc >= e.end) {
          continue;
        }
        bool match = false;
        if (e.catch_type < 0) {
          match = true; // catch-all
        } else {
          const rbc::Const& ccConst =
              m.cp[static_cast<std::uint32_t>(e.catch_type)];
          const interp::ClassId catchCls = rt.classId(ccConst.str);
          const interp::ClassId excCls = rt.heap().classOf(exc);
          match = rt.isAssignableFrom(catchCls, excCls);
        }
        if (!match) {
          continue;
        }
        // CAUGHT: reset regs (ALL die, SS5.3), deliver to regs[0], re-enter
        // at the handler's entry stub.
        for (std::uint32_t i = 0; i < cc->num_regs; ++i) {
          slots[cc->num_locals + i] = interp::Value::bottom();
        }
        if (cc->num_regs > 0) {
          slots[cc->num_locals] = interp::Value::refVal(exc);
        }
        act->pending_exc = 0;
        act->trap_kind = 0;
        act->reenter = 0;
        bool found = false;
        for (const CodeEntry& ce : cc->entries) {
          if (ce.rbc_pc == e.handler_pc && !ce.is_method_entry) {
            entryOffset = ce.native_offset;
            found = true;
            break;
          }
        }
        if (!found) {
          // The instantiation guarantees handler entries exist; defensive.
          g_current = prev;
          Tier1RunResult bad;
          bad.status = Tier1Status::Threw;
          bad.exception = impl->rt->makeException(kClsInternalError,
                                                  "handler entry missing");
          return bad;
        }
        caught = true;
        break;
      }
      if (caught) {
        continue; // re-enter compiled code at the handler
      }
      // NOT caught: deopt to T0 with the pending exception; T0's exception
      // algorithm unwinds this frame (releasing monitors) and beyond.
      interp::Frame f = t0Frame(m, rbcPc, {});
      f.locals.assign(slots, slots + cc->num_locals);
      f.regs.assign(slots + cc->num_locals,
                    slots + cc->num_locals + cc->num_regs);
      for (std::uint32_t i = 0; i < act->mon_count; ++i) {
        f.monitors.push_back(interp::ObjRef{act->mon_ids[i]});
      }
      f.pendingException = exc;
      g_current = prev;
      const interp::RunResult r = impl->interp->resume(std::move(f));
      return mapRun(r);
    }

    // Guard failure (no exception): clean T0 re-execution at the pc.
    // Inline traps (idiv/ldiv zero) are the only no-pending Trap points.
    {
      const RealDeoptPoint* why =
          cc->deoptPoint(static_cast<std::uint32_t>(act->deopt_id));
      if (why != nullptr && why->reason == baseline::DeoptReason::Guard) {
        ++impl->stats->deopt_guard;
      } else {
        ++impl->stats->deopt_trap;
      }
    }
    const std::uint32_t rbcPc = cc->rbcPcAt(
        static_cast<std::uint32_t>(act->deopt_pc));
    const RealDeoptPoint* point =
        cc->deoptPoint(static_cast<std::uint32_t>(act->deopt_id));
    interp::Frame f;
    f.method = &m;
    f.pc = rbcPc;
    f.locals.assign(slots, slots + cc->num_locals);
    f.regs.assign(slots + cc->num_locals,
                  slots + cc->num_locals + cc->num_regs);
    for (std::uint32_t i = 0; i < act->mon_count; ++i) {
      f.monitors.push_back(interp::ObjRef{act->mon_ids[i]});
    }
    if (point != nullptr && point->reason == baseline::DeoptReason::Trap &&
        rbcPc < m.code.size()) {
      const rbc::Op op = m.code[rbcPc].opcode();
      if (op == rbc::Op::Idiv || op == rbc::Op::Ldiv ||
          op == rbc::Op::Irem || op == rbc::Op::Lrem) {
        f.pendingException = impl->rt->makeException(kClsArithmetic, "/ by zero");
      }
    }
    g_current = prev;
    const interp::RunResult r = impl->interp->resume(std::move(f));
    return mapRun(r);
  }

  g_current = prev;
  Tier1RunResult out;
  out.status = Tier1Status::Returned;
  out.result = act->ret_value;
  return out;
}

} // namespace b2::codegen

// --- Tier1 public API ------------------------------------------------------------------

namespace b2::codegen {

Tier1::Tier1(const rbc::Program& program, const Tier1Config& config)
    : impl_(std::make_unique<Impl>()),
      program_(program),
      cfg_(config),
      interp_(program),
      set_(baseline::builtinStencilSetV0()) {
  impl_->stats = &stats_;
  impl_->rt = &interp_.runtime();
  impl_->interp = &interp_;
  impl_->program = &program_;
  impl_->set = &set_;
  impl_->planOptions = config.plan_options;
  impl_->archiveOk =
      loadEmbeddedX86_64(set_, impl_->archive).ok;
}

Tier1::~Tier1() = default;

Tier1RunResult Tier1::run(std::string_view name, std::string_view descriptor,
                          std::span<const interp::Value> args) {
  // Hard gate: verify every method (the same discipline as T0/b2run).
  for (const rbc::Method& m : program_.methods) {
    const rbc::VerifyResult vr = rbc::verify(m);
    if (!vr.ok) {
      Tier1RunResult out;
      out.status = Tier1Status::VerifyFailed;
      out.verify_diags = vr.diags;
      return out;
    }
  }
  const rbc::Method* entry = program_.find(name, descriptor);
  if (entry == nullptr) {
    // Mirror T0's run(): a missing entry raises NoSuchMethodError.
    Tier1RunResult out;
    out.status = Tier1Status::Threw;
    out.exception = interp_.runtime().makeException(
        "java/lang/NoSuchMethodError",
        program_.className + "." + std::string(name) + std::string(descriptor));
    return out;
  }
  const std::uint32_t idx = static_cast<std::uint32_t>(entry -
                                                       program_.methods.data());
  Tier1RunResult out = runMethod(idx, args);
  out.stats = stats_;
  return out;
}

Tier1RunResult Tier1::runMethod(std::uint32_t method_index,
                                std::span<const interp::Value> args) {
  if (method_index >= program_.methods.size()) {
    Tier1RunResult out;
    out.status = Tier1Status::Threw;
    out.exception = interp_.runtime().makeException(
        "java/lang/NoSuchMethodError", "method index out of range");
    return out;
  }
  if (!impl_->archiveOk) {
    // No valid archive: pure T0 (Rule 96; never a crash).
    last_run_compiled_ = false;
    const rbc::Method& m = program_.methods[method_index];
    interp_.runtime().bumpInvocations(interp::MethodId{method_index});
    const interp::RunResult r = interp_.resume(t0Frame(m, 0, args));
    return mapRun(r);
  }
  Tier1RunResult out = engineExecuteMethod(impl_.get(), method_index, args,
                                           &last_run_compiled_);
  return out;
}

const CompiledCode* Tier1::codeFor(std::uint32_t method_index) const {
  const auto it = impl_->cache.find(method_index);
  return it == impl_->cache.end() ? nullptr : it->second.get();
}

} // namespace b2::codegen
