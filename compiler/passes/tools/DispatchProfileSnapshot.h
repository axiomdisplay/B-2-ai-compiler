#pragma once
// B-2 Passes - the ICDG Phase 2 ingestion bridge: T0's dispatch profile
// -> the passes-side DispatchProfile snapshot (docs/icdg.md SS7/SS24;
// the T0 side is docs/interp_contract.md SS8.1, v1.2.0).
//
// WHY THIS FILE EXISTS (and lives in tools/):
// The converter needs BOTH sides: b2::interp types (the read) and
// b2::passes types (the write). Placing it here keeps the passes
// LIBRARY interp-free (the charter boundary: T0 is a tier below T2,
// never a link dependency of the optimizer) while giving every
// interp-linking integrator - b2graph (--pgo) and the inline tests -
// ONE conversion, so the snapshot semantics cannot drift between
// surfaces. The mapping is 1:1 and total:
//
//   site key          (caller MethodId, call pc)      unchanged
//   kind              DispatchSiteKind                ProfileSiteKind
//   megamorphic       sticky flag                     sticky flag
//   count             saturating site total           saturating site total
//   entry.recvClass   runtime ClassId                 internal NAME
//   entry.target      MethodId.v                      unchanged (unified
//                                                     program-table space)
//
// The NAME is the whole point: the engine resolves the guard's ir
// TypeId through the CalleeSource's resolver (the graph's id space),
// and the class name is the only value valid in BOTH the runtime
// ClassId space and the resolver space. Names are copied IMMEDIATELY
// (ClassInfo::name views can dangle across a later classes_ growth -
// the Runtime.h caveat; nothing registers classes after the run, but
// the copy is cheap and unconditional).
//
// Determinism (Rule 124): rows walk in (method, pc) order, entries in
// the T0 first-seen order; the snapshot is a pure function of the
// interpreter's final state.

#include <cstdint>
#include <vector>

#include "b2/interp/Interp.h"
#include "b2/passes/Inline.h"

namespace b2::passes {

// The sentinels must agree by value: the snapshot reuses T0's target
// sentinel semantics verbatim (kProfileNoTarget is passes-local only so
// Inline.h needs no interp include; this pins the equality forever).
static_assert(interp::kDispatchNoTarget == kProfileNoTarget,
              "target sentinel drift between T0 and the snapshot");

inline void snapshotDispatchProfile(const interp::Interpreter& interpIn,
                                    DispatchProfile& out) {
  const std::vector<std::vector<interp::DispatchSiteProfile>>& rows =
      interpIn.dispatchProfiles();
  out.sites.assign(rows.size(), {});
  for (std::size_t m = 0; m < rows.size(); ++m) {
    std::vector<ProfileSite>& dstRow = out.sites[m];
    dstRow.resize(rows[m].size());
    for (std::size_t pc = 0; pc < rows[m].size(); ++pc) {
      const interp::DispatchSiteProfile& p = rows[m][pc];
      ProfileSite& q = dstRow[pc];
      switch (p.kind) {
      case interp::DispatchSiteKind::Virtual:
        q.kind = ProfileSiteKind::Virtual;
        break;
      case interp::DispatchSiteKind::Interface:
        q.kind = ProfileSiteKind::Interface;
        break;
      case interp::DispatchSiteKind::Special:
        q.kind = ProfileSiteKind::Special;
        break;
      case interp::DispatchSiteKind::Static:
        q.kind = ProfileSiteKind::Static;
        break;
      }
      q.megamorphic = p.megamorphic;
      q.count = p.count;
      for (std::size_t i = 0; i < interp::kMaxDispatchProfileEntries; ++i) {
        const interp::DispatchEntry& e = p.entries[i];
        q.entries[i].count = e.count;
        q.entries[i].target = e.target;
        if (e.recvClass == interp::kDispatchNoRecvClass) {
          q.entries[i].recvClass.clear(); // the static-resolution sentinel
        } else {
          // Copy NOW: ClassInfo::name is a view into a growable vector.
          q.entries[i].recvClass.assign(
              interpIn.runtime().classInfo(interp::ClassId{e.recvClass}).name);
        }
      }
    }
  }
}

} // namespace b2::passes
