// B-2 IR tests - the 144-entry effect reorder table vs its construction
// rules, plus the documented spot checks (Rule 121 / special_passes.md 2.1).

#include "TestHarness.h"

#include <b2/ir/Effect.h>

using namespace b2;
using EK = ir::EffectKind;
using R = ir::EffectOrderResult;

B2_TEST(effect_table_matches_construction_rules_for_all_144_entries) {
  for (int a = 0; a < 12; ++a) {
    for (int b = 0; b < 12; ++b) {
      const EK ea = static_cast<EK>(a);
      const EK eb = static_cast<EK>(b);
      CHECK_MSG(ir::canReorder(ea, eb) == ir::reorderByRule(ea, eb),
                ir::effectName(ea) + std::string(" before ") +
                    ir::effectName(eb) + " disagrees with the rules");
    }
  }
}

B2_TEST(effect_pure_commutes_with_every_class) {
  for (int k = 0; k < 12; ++k) {
    const EK e = static_cast<EK>(k);
    CHECK(ir::canReorder(EK::Pure, e) == R::Allowed);
    CHECK(ir::canReorder(e, EK::Pure) == R::Allowed);
  }
}

B2_TEST(effect_thread_local_effects_commute_with_shared_and_sync_classes) {
  // R5: classification-level disjointness of thread-local storage.
  const EK tls[] = {EK::ReadLocal, EK::WriteLocal};
  const EK shared[] = {EK::ReadShared, EK::WriteShared, EK::VolatileRead,
                       EK::VolatileWrite, EK::MonitorEnter, EK::MonitorExit,
                       EK::Allocation};
  for (EK a : tls) {
    for (EK b : shared) {
      CHECK(ir::canReorder(a, b) == R::Allowed);
      CHECK(ir::canReorder(b, a) == R::Allowed);
    }
  }
}

B2_TEST(effect_thread_local_write_pairs_require_compensation) {
  CHECK(ir::canReorder(EK::ReadLocal, EK::ReadLocal) == R::Allowed);
  CHECK(ir::canReorder(EK::ReadLocal, EK::WriteLocal) ==
        R::RequiresCompensation);
  CHECK(ir::canReorder(EK::WriteLocal, EK::ReadLocal) ==
        R::RequiresCompensation);
  CHECK(ir::canReorder(EK::WriteLocal, EK::WriteLocal) ==
        R::RequiresCompensation);
}

B2_TEST(effect_volatile_and_monitor_classes_forbidden_among_themselves) {
  // R6: synchronization order follows program order (JLS 17.4.4).
  const EK sync[] = {EK::VolatileRead, EK::VolatileWrite, EK::MonitorEnter,
                     EK::MonitorExit};
  for (EK a : sync) {
    for (EK b : sync) {
      CHECK(ir::canReorder(a, b) == R::Forbidden);
    }
  }
}

B2_TEST(effect_shared_write_crossing_strong_sync_is_forbidden) {
  // R7: happens-before visibility for other threads (JLS 17.4.5).
  CHECK(ir::canReorder(EK::WriteShared, EK::VolatileWrite) == R::Forbidden);
  CHECK(ir::canReorder(EK::VolatileWrite, EK::WriteShared) == R::Forbidden);
  CHECK(ir::canReorder(EK::WriteShared, EK::MonitorEnter) == R::Forbidden);
  CHECK(ir::canReorder(EK::MonitorExit, EK::WriteShared) == R::Forbidden);
  // ... but a plain write vs an acquire read only needs compensation.
  CHECK(ir::canReorder(EK::WriteShared, EK::VolatileRead) ==
        R::RequiresCompensation);
  CHECK(ir::canReorder(EK::VolatileRead, EK::WriteShared) ==
        R::RequiresCompensation);
}

B2_TEST(effect_plain_shared_read_pairs_commute_write_pairs_compensate) {
  CHECK(ir::canReorder(EK::ReadShared, EK::ReadShared) == R::Allowed);
  CHECK(ir::canReorder(EK::ReadShared, EK::WriteShared) ==
        R::RequiresCompensation);
  CHECK(ir::canReorder(EK::WriteShared, EK::ReadShared) ==
        R::RequiresCompensation);
  CHECK(ir::canReorder(EK::WriteShared, EK::WriteShared) ==
        R::RequiresCompensation);
}

B2_TEST(effect_call_opaque_blocks_all_reordering_except_thread_locals) {
  // R2: a callee can observe everything except thread-local state.
  for (int k = 0; k < 12; ++k) {
    const EK e = static_cast<EK>(k);
    if (e == EK::Pure || e == EK::ReadLocal || e == EK::WriteLocal ||
        e == EK::CallOpaque) {
      continue;
    }
    CHECK(ir::canReorder(e, EK::CallOpaque) == R::Forbidden);
    CHECK(ir::canReorder(EK::CallOpaque, e) == R::Forbidden);
  }
  CHECK(ir::canReorder(EK::ReadLocal, EK::CallOpaque) == R::Allowed);
  CHECK(ir::canReorder(EK::WriteLocal, EK::CallOpaque) == R::Allowed);
  CHECK(ir::canReorder(EK::CallOpaque, EK::ReadLocal) == R::Allowed);
  CHECK(ir::canReorder(EK::CallOpaque, EK::WriteLocal) == R::Allowed);
  CHECK(ir::canReorder(EK::CallOpaque, EK::CallOpaque) == R::Forbidden);
}

B2_TEST(effect_exception_throw_observes_writes_but_not_reads_or_allocs) {
  // R3: publication point.
  CHECK(ir::canReorder(EK::ReadShared, EK::ExceptionThrow) == R::Allowed);
  CHECK(ir::canReorder(EK::ExceptionThrow, EK::ReadShared) == R::Allowed);
  CHECK(ir::canReorder(EK::Allocation, EK::ExceptionThrow) == R::Allowed);
  CHECK(ir::canReorder(EK::WriteShared, EK::ExceptionThrow) == R::Forbidden);
  CHECK(ir::canReorder(EK::ExceptionThrow, EK::WriteLocal) == R::Forbidden);
  CHECK(ir::canReorder(EK::ExceptionThrow, EK::MonitorEnter) == R::Forbidden);
  CHECK(ir::canReorder(EK::ExceptionThrow, EK::ExceptionThrow) == R::Forbidden);
}

B2_TEST(effect_allocation_commutes_with_all_non_opaque_non_throw_classes) {
  // R9: allocation identity/order is not Java-observable.
  for (int k = 0; k < 12; ++k) {
    const EK e = static_cast<EK>(k);
    if (e == EK::CallOpaque || e == EK::ExceptionThrow || e == EK::Pure) {
      continue; // covered by their own rules
    }
    CHECK(ir::canReorder(EK::Allocation, e) == R::Allowed);
    CHECK(ir::canReorder(e, EK::Allocation) == R::Allowed);
  }
  CHECK(ir::canReorder(EK::Allocation, EK::Allocation) == R::Allowed);
}

B2_TEST(effect_only_thread_local_classes_are_speculatively_reorderable) {
  // special_passes.md 2.3 Rule 4.
  for (int k = 0; k < 12; ++k) {
    const EK e = static_cast<EK>(k);
    const bool expect = e == EK::Pure || e == EK::ReadLocal ||
                        e == EK::WriteLocal;
    CHECK(ir::isThreadLocalEffect(e) == expect);
  }
}

B2_TEST(effect_classification_helpers_agree_with_the_table) {
  CHECK(ir::isReadEffect(EK::VolatileRead));
  CHECK(!ir::isWriteEffect(EK::VolatileRead));
  CHECK(ir::isWriteEffect(EK::WriteShared));
  CHECK(ir::isStrongSyncEffect(EK::VolatileWrite));
  CHECK(!ir::isStrongSyncEffect(EK::VolatileRead)); // acquire is not strong
  CHECK(ir::isSyncEffect(EK::VolatileRead));
}
