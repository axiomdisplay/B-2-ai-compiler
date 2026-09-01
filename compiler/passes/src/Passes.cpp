// B-2 Passes - the pass registry, the dispatcher, and the early-cleanup +
// GVN pipeline driver (suite items 11-20, 35).
//
// WHY THIS FILE EXISTS:
// Rule 123 requires every pass to be a registry entry with a declared
// contract; Rules 132/144 require independent kill switches; Rule 10
// requires bounded fixpoints for pass groups; Rule 40 requires the
// verifier between passes; Rule 124 requires deterministic execution.
// This file is that law, executable: a fixed registry table, a dispatcher
// that runs one pass in isolation, and the pipeline that runs the group
// to a bounded fixpoint with fail-closed verification.

#include "b2/ir/Node.h"
#include "b2/ir/Verifier.h"
#include "b2/passes/Passes.h"

#include <string>
#include <string_view>
#include <vector>

#include "PassInternal.h"

namespace b2::passes {

namespace {

// The delivered registry, in appendix-key order (normative numbering).
// Contracts summarized to one line here; docs/pass_contracts.md is the
// full text (the review artifact for every rewrite rule).
constexpr PassInfo kRegistry[] = {
    {PassKey::DeadCodeElimination, "dce",
     "requires: verifier-clean graph; produces: no live pure value with "
     "zero live users (conservative exclusion set documented); shrinks",
     false, kDefaultPassRewriteBudget},
    {PassKey::TrivialBlockFusion, "fusion",
     "requires: verifier-clean graph; produces: no Region/LoopBegin with a "
     "dead or single predecessor, no trivial phi; repairs control edges",
     true, kDefaultPassRewriteBudget},
    {PassKey::Canonicalization, "canon",
     "requires: verifier-clean graph; produces: constants right on "
     "commutative integer ops, Not(test) folded to the complement test; "
     "FP commutativity untouched (exact-bits policy)",
     true, kDefaultPassRewriteBudget},
    {PassKey::ConstantFolding, "constfold",
     "requires: verifier-clean graph; produces: constant expressions "
     "replaced by exact-bit constants (Java semantics: wrap, shifts, "
     "IEEE, JLS 5.1.3 conversions); FP folds only when NaN-free",
     true, kDefaultPassRewriteBudget},
    {PassKey::StrengthReduction, "strength",
     "requires: verifier-clean graph; produces: Mul-by-power-of-two "
     "(>=2) as Shl, identical wrap semantics; negatives/FP untouched",
     true, kDefaultPassRewriteBudget},
    {PassKey::IdentityRemoval, "identity",
     "requires: verifier-clean graph; produces: x+0/x*1/x&-1/x<<0/x-x/"
     "self-compares/double-negation/round-trip conversions removed; FP "
     "additive identities NOT removed (exact-bits policy)",
     true, kDefaultPassRewriteBudget},
    {PassKey::NullCheckFolding, "nullfold",
     "requires: verifier-clean graph; produces: NullCheck guards on "
     "provably non-null values pass through (control rewired); guards on "
     "maybe-null values kept",
     false, kDefaultPassRewriteBudget},
    {PassKey::BranchNormalization, "branchnorm",
     "requires: verifier-clean graph; produces: constant-condition Ifs "
     "replaced by the taken edge, constant guards pass through or become "
     "Deopt (same DeoptId, same FrameState); unreachable aftermath "
     "repaired; switches NOT folded (v1: case labels are frontend-side)",
     true, kDefaultPassRewriteBudget},
    {PassKey::ControlFlowSimplification, "cfs",
     "requires: verifier-clean graph; produces: no live control consumer "
     "unreachable from Start; regions/phis repaired after kills",
     true, kDefaultPassRewriteBudget},
    {PassKey::GVN, "gvn",
     "requires: verifier-clean graph; produces: no two live nodes with "
     "identical (kind, payload, payload2, constValue, inputs) in the "
     "eligible set; replacement by the lowest id; FrameStates auto-update "
     "(Rule 14)",
     false, kDefaultPassRewriteBudget},
    {PassKey::EscapeAnalysis, "escape",
     "requires: verifier-clean post-inline graph (guards folded); "
     "produces: every allocation classified on the height-6 escape "
     "lattice with an explainable decision; analysis-only mode "
     "rewrites nothing",
     false, kDefaultPassRewriteBudget},
    {PassKey::PartialEscapeAnalysis, "pea",
     "requires: verifier-clean graph; produces: every partially-escaping "
     "allocation materialized at its first escape use (Materialize wired "
     "into ctrl AND mem; pre-escape loads still forward)",
     true, kDefaultPassRewriteBudget},
    {PassKey::ScalarReplacement, "scalar",
     "requires: verifier-clean graph; produces: no-escape allocations "
     "replaced by field values (loads forwarded, virtual stores "
     "spliced out, IsNull folded, allocation killed)",
     true, kDefaultPassRewriteBudget},
    {PassKey::MaterializationPlanning, "matplan",
     "requires: verifier-clean graph; produces: materialized objects "
     "carry complete vobj snapshots (typed defaults for unread fields, "
     "inner-first nested references)",
     true, kDefaultPassRewriteBudget},
};

constexpr std::size_t kRegistrySize = sizeof(kRegistry) / sizeof(kRegistry[0]);

struct BadRow {
  // Impossible key value: every real registry row self-identifies, so an
  // unknown/not-delivered key can never collide with a delivered one.
  static constexpr PassInfo value{static_cast<PassKey>(0xFFFFu), "bad",
                                  "not delivered", false, 0};
};

constexpr std::uint32_t kDiagCap = 32;

void addDiag(PassResult& r, PassKey pass, std::string message) {
  if (r.diags.size() >= kDiagCap) {
    return;
  }
  PassDiag d;
  d.pass = pass;
  d.message = std::move(message);
  r.diags.push_back(std::move(d));
}

// One pass body by key (dispatcher). Budgets come from the registry row;
// every pass body shares the run's Junk sink cache (deterministic node
// creation order, Rule 124).
void runOne(PassKey key, ir::Graph& g, PassTelemetry& t, detail::Budget& b,
            detail::Junk& jk) {
  switch (key) {
  case PassKey::DeadCodeElimination:
    detail::runDCE(g, t, b, jk);
    return;
  case PassKey::TrivialBlockFusion:
    detail::runSimplify(g, detail::kTrivialPhi, t, b, jk);
    detail::rebuildRegions(g, t, b, jk);
    return;
  case PassKey::Canonicalization:
    detail::runSimplify(g, detail::kCanonical, t, b, jk);
    return;
  case PassKey::ConstantFolding:
    detail::runSimplify(g, detail::kFold, t, b, jk);
    return;
  case PassKey::StrengthReduction:
    detail::runSimplify(g, detail::kStrength, t, b, jk);
    return;
  case PassKey::IdentityRemoval:
    detail::runSimplify(g, detail::kIdentity, t, b, jk);
    return;
  case PassKey::NullCheckFolding:
    detail::runNullCheckFolding(g, t, b, jk);
    return;
  case PassKey::BranchNormalization:
    detail::runBranchNormalization(g, t, b, jk);
    return;
  case PassKey::ControlFlowSimplification:
    detail::unreachableSweep(g, t, b, jk);
    return;
  case PassKey::GVN:
    detail::runGVN(g, t, b, jk);
    return;
  case PassKey::EscapeAnalysis:
    // Analysis-only mode (key 65 in isolation): the classification and
    // decision surface without any rewrite.
    detail::runPEA(g, detail::kPeaAnalyze, t, b, jk, nullptr);
    return;
  case PassKey::PartialEscapeAnalysis:
    detail::runPEA(g, detail::kPeaAnalyze | detail::kPeaPartial, t, b,
                   jk, nullptr);
    return;
  case PassKey::ScalarReplacement:
    detail::runPEA(g, detail::kPeaAnalyze | detail::kPeaScalar, t, b,
                   jk, nullptr);
    return;
  case PassKey::MaterializationPlanning:
    detail::runPEA(g, detail::kPeaAnalyze | detail::kPeaPartial |
                           detail::kPeaPlanning,
                   t, b, jk, nullptr);
    return;
  default:
    return; // unreachable via the dispatcher's deliverability check
  }
}

// Verifies and folds diagnostics into the result. Returns true when clean.
bool verifyInto(PassResult& r, ir::Graph& g, PassKey pass) {
  const ir::VerifyResult v = ir::verify(g);
  if (v.ok) {
    return true;
  }
  r.ok = false;
  for (const ir::VerifyDiag& d : v.diags) {
    if (r.diags.size() >= kDiagCap) {
      break;
    }
    addDiag(r, pass, "IR verify failed at n" + std::to_string(d.node) +
                         ": " + d.message);
  }
  return false;
}

} // namespace

const PassInfo& passInfo(PassKey key) {
  for (std::size_t i = 0; i < kRegistrySize; ++i) {
    if (kRegistry[i].key == key) {
      return kRegistry[i];
    }
  }
  return BadRow::value;
}

std::span<const PassInfo> passRegistry() {
  return std::span<const PassInfo>(kRegistry, kRegistrySize);
}

std::size_t passRegistryIndex(PassKey key) {
  for (std::size_t i = 0; i < kRegistrySize; ++i) {
    if (kRegistry[i].key == key) {
      return i;
    }
  }
  return kInvalidPassIndex;
}

bool PassConfig::isPassEnabled(PassKey key) const {
  const std::size_t idx = passRegistryIndex(key);
  if (idx == kInvalidPassIndex) {
    return false;
  }
  return (enabledBits & (1u << idx)) != 0;
}

void PassConfig::setPassEnabled(PassKey key, bool on) {
  const std::size_t idx = passRegistryIndex(key);
  if (idx == kInvalidPassIndex || idx >= 16) {
    return; // registry is <= 16 rows; unknown keys are no-ops
  }
  const std::uint16_t bit = static_cast<std::uint16_t>(1u << idx);
  if (on) {
    enabledBits |= bit;
  } else {
    enabledBits &= static_cast<std::uint16_t>(~bit);
  }
}

void PassConfig::disableAll() {
  enabledBits = 0;
  maxCleanupRounds = 0;
}

PassResult runSinglePass(ir::Graph& g, PassKey key, const PassConfig& cfg) {
  PassResult r;
  const PassInfo& info = passInfo(key);
  if (info.key != key) {
    r.ok = false;
    addDiag(r, key, "pass key " + std::to_string(static_cast<unsigned>(key)) +
                       " is not in the delivered registry");
    return r;
  }
  if (!cfg.isPassEnabled(key)) {
    return r; // kill switch: successful no-op (Rules 132/144)
  }

  detail::Budget b{info.rewriteBudget};
  detail::Junk jk(g);
  runOne(key, g, r.telemetry, b, jk);
  if (b.exceeded) {
    r.telemetry.budgetOverruns += 1;
    r.telemetry.converged = false;
    addDiag(r, key, std::string(info.name) +
                       ": rewrite budget exhausted (" +
                       std::to_string(info.rewriteBudget) +
                       "); stopped at a valid point");
  }
  if (cfg.verifyAfterEachPass && !verifyInto(r, g, key)) {
    return r; // fail closed: diagnostics already recorded
  }
  return r;
}

PeaResult runPartialEscapeAnalysis(ir::Graph& g, const PassConfig& cfg) {
  PeaResult r;
  const std::uint32_t mask =
      (cfg.isPassEnabled(PassKey::EscapeAnalysis) ? detail::kPeaAnalyze
                                                 : 0u) |
      (cfg.isPassEnabled(PassKey::PartialEscapeAnalysis) ? detail::kPeaPartial
                                                        : 0u) |
      (cfg.isPassEnabled(PassKey::ScalarReplacement) ? detail::kPeaScalar
                                                    : 0u) |
      (cfg.isPassEnabled(PassKey::MaterializationPlanning)
           ? detail::kPeaPlanning
           : 0u);
  if (mask == 0) {
    return r; // key 65 kill switch: the engine is a no-op
  }
  detail::Budget b{kDefaultPassRewriteBudget};
  detail::Junk jk(g);
  detail::runPEA(g, mask, r.telemetry, b, jk, &r.decisions);
  if (b.exceeded) {
    r.telemetry.budgetOverruns += 1;
    r.telemetry.converged = false;
    addDiag(r, PassKey::EscapeAnalysis,
            "pea: rewrite budget exhausted (" +
                std::to_string(kDefaultPassRewriteBudget) +
                "); stopped at a valid point");
  }
  if (r.telemetry.peaRejected > 0 || r.telemetry.budgetOverruns > 0) {
    // Not a failure: rejections are PEA Rule option 4 (sound refusal)
    // and ride the decision log; the diag is the log-summary surface
    // for pipeline-style callers that only read PassResult.
    addDiag(r, PassKey::EscapeAnalysis,
            "pea: " + std::to_string(r.telemetry.peaScalarized) +
                " scalarized, " +
                std::to_string(r.telemetry.peaMaterialized) +
                " materialized, " + std::to_string(r.telemetry.peaRejected) +
                " rejected");
  }
  if (cfg.verifyAfterEachPass && !verifyInto(r, g, PassKey::EscapeAnalysis)) {
    return r; // fail closed
  }
  return r;
}

PassResult runEarlyCleanup(ir::Graph& g, const PassConfig& cfg) {
  PassResult r;

  // The executed sequence, in order (Rule 1: one list, both tiers). Keys
  // not enabled contribute nothing; the simplify classes merge into one
  // sweep gated by any of their keys being enabled.
  const bool simplifyOn =
      cfg.isPassEnabled(PassKey::ConstantFolding) ||
      cfg.isPassEnabled(PassKey::IdentityRemoval) ||
      cfg.isPassEnabled(PassKey::StrengthReduction) ||
      cfg.isPassEnabled(PassKey::Canonicalization) ||
      cfg.isPassEnabled(PassKey::TrivialBlockFusion);
  const std::uint32_t simplifyMask =
      (cfg.isPassEnabled(PassKey::ConstantFolding) ? detail::kFold : 0u) |
      (cfg.isPassEnabled(PassKey::IdentityRemoval) ? detail::kIdentity
                                                   : 0u) |
      (cfg.isPassEnabled(PassKey::StrengthReduction) ? detail::kStrength
                                                     : 0u) |
      (cfg.isPassEnabled(PassKey::Canonicalization) ? detail::kCanonical
                                                    : 0u) |
      (cfg.isPassEnabled(PassKey::TrivialBlockFusion) ? detail::kTrivialPhi
                                                      : 0u);

  const std::uint32_t rounds =
      cfg.maxCleanupRounds > kMaxEarlyCleanupRounds ? kMaxEarlyCleanupRounds
                                                    : cfg.maxCleanupRounds;

  // Per-pass budgets come from the registry rows; the simplify sweep uses
  // the largest budget among its constituent keys (conservative: the sweep
  // is the sum of its classes).
  std::uint32_t simplifyBudget = 0;
  if (simplifyOn) {
    for (const PassInfo& pi : passRegistry()) {
      if (pi.key == PassKey::ConstantFolding ||
          pi.key == PassKey::IdentityRemoval ||
          pi.key == PassKey::StrengthReduction ||
          pi.key == PassKey::Canonicalization ||
          pi.key == PassKey::TrivialBlockFusion) {
        if (pi.rewriteBudget > simplifyBudget) {
          simplifyBudget = pi.rewriteBudget;
        }
      }
    }
  }

  detail::Junk jk(g);
  bool changed = true;
  for (std::uint32_t round = 0; round < rounds && changed; ++round) {
    changed = false;
    r.telemetry.rounds = round + 1;
    const std::uint32_t before =
        r.telemetry.rewrites + r.telemetry.removals;

    auto step = [&](PassKey key) {
      const PassInfo& info = passInfo(key);
      detail::Budget b{info.rewriteBudget};
      runOne(key, g, r.telemetry, b, jk);
      if (b.exceeded) {
        r.telemetry.budgetOverruns += 1;
        r.telemetry.converged = false;
        addDiag(r, key, std::string(info.name) +
                           ": rewrite budget exhausted (" +
                           std::to_string(info.rewriteBudget) +
                           "); stopped at a valid point");
      }
      if (cfg.verifyAfterEachPass && !verifyInto(r, g, key)) {
        return false; // fail closed
      }
      return true;
    };

    if (simplifyOn) {
      detail::Budget b{simplifyBudget};
      detail::runSimplify(g, simplifyMask, r.telemetry, b, jk);
      if (b.exceeded) {
        r.telemetry.budgetOverruns += 1;
        r.telemetry.converged = false;
        addDiag(r, PassKey::ConstantFolding,
                "simplify sweep: rewrite budget exhausted (" +
                    std::to_string(simplifyBudget) +
                    "); stopped at a valid point");
      }
      if (cfg.verifyAfterEachPass &&
          !verifyInto(r, g, PassKey::ConstantFolding)) {
        return r;
      }
    }
    if (!step(PassKey::NullCheckFolding)) {
      return r;
    }
    if (!step(PassKey::BranchNormalization)) {
      return r;
    }
    if (!step(PassKey::ControlFlowSimplification)) {
      return r;
    }
    if (!step(PassKey::DeadCodeElimination)) {
      return r;
    }
    if (!step(PassKey::GVN)) {
      return r;
    }
    // CM-PEA (keys 65/66/67/69 - one engine call, stage bits per key):
    // runs after GVN (redundant loads are already merged) and after the
    // guards on the allocations' uses were folded (nullfold/DCE), which
    // is the precondition the engine documents. A disabled stage bit
    // rejects that disposition (PEA Rule option 4), never half-applies.
    {
      const std::uint32_t peaMask =
          (cfg.isPassEnabled(PassKey::EscapeAnalysis) ? detail::kPeaAnalyze
                                                     : 0u) |
          (cfg.isPassEnabled(PassKey::PartialEscapeAnalysis)
               ? detail::kPeaPartial
               : 0u) |
          (cfg.isPassEnabled(PassKey::ScalarReplacement)
               ? detail::kPeaScalar
               : 0u) |
          (cfg.isPassEnabled(PassKey::MaterializationPlanning)
               ? detail::kPeaPlanning
               : 0u);
      if (peaMask != 0) {
        detail::Budget b{kDefaultPassRewriteBudget};
        detail::runPEA(g, peaMask, r.telemetry, b, jk, nullptr);
        if (b.exceeded) {
          r.telemetry.budgetOverruns += 1;
          r.telemetry.converged = false;
          addDiag(r, PassKey::EscapeAnalysis,
                  "pea: rewrite budget exhausted (" +
                      std::to_string(kDefaultPassRewriteBudget) +
                      "); stopped at a valid point");
        }
        if (cfg.verifyAfterEachPass &&
            !verifyInto(r, g, PassKey::EscapeAnalysis)) {
          return r;
        }
      }
    }
    if (!step(PassKey::DeadCodeElimination)) {
      return r;
    }

    const std::uint32_t after = r.telemetry.rewrites + r.telemetry.removals;
    changed = after != before;
    if (!changed) {
      r.telemetry.converged = true;
    } else if (round + 1 == rounds) {
      // Stopped at the round budget while the graph was still changing:
      // the graph is valid (verified), but the fixpoint is not confirmed.
      r.telemetry.converged = false;
      addDiag(r, PassKey::DeadCodeElimination,
              "cleanup fixpoint stopped at the round budget (" +
                  std::to_string(rounds) + " rounds) while still changing");
    }
  }
  return r;
}

} // namespace b2::passes
