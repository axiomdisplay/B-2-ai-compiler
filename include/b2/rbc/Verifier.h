#pragma once
// B-2 RBC - structural and type verifier.
//
// WHY THIS FILE EXISTS:
// RBC is the input contract of every tier. T1 composes stencils without any
// IR safety net and T0 must never see a malformed instruction, so RBC is
// verified BEFORE any tier executes it (Law: verifier before quickener before
// execution). The verifier is deliberately total: arbitrary garbage input
// produces a bounded diagnostic list, never a crash and never an infinite
// loop.

#include <cstdint>
#include <string>
#include <vector>

#include "b2/rbc/Rbc.h"

namespace b2::rbc {

struct VerifyDiag {
  std::uint32_t pc = 0;      // instruction index the diagnostic is about
  std::string message;       // human-readable, self-contained
};

struct VerifyResult {
  bool ok = false;
  std::vector<VerifyDiag> diags; // ordered by emission, capped (default 100)

  [[nodiscard]] bool hasErrors() const noexcept { return !ok; }
};

// Verifies one method. Checks, in order:
//   1. structural: opcodes valid, registers < numRegs, slots < numLocals,
//      cp indices in range and of the required Kind, branch targets in
//      [0, code.size()), switch tables well-formed, exception table ranges
//      sane and handler entries in range.
//   2. type: abstract interpretation over basic blocks; locals initialized
//      from the descriptor (static: params, else receiver + params), working
//      registers start Bottom; every operand checked against OpInfo types;
//      merges via join(); locals must hold one stable type across protected
//      ranges (exception entry sees locals but Bottom registers).
//   3. termination: every path returns or throws (or the method diverges
//      via backward goto, which is legal).
//
// Malformed methods still return a result with diagnostics - never UB.
[[nodiscard]] VerifyResult verify(const Method& m) noexcept;

} // namespace b2::rbc
