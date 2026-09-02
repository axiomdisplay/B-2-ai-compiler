// B-2 Passes - the simplify engine: constant folding, identity removal,
// strength reduction, canonicalization, and trivial-phi collapse
// (registry keys 12-16; one sweep, mask-selected rewrite classes).
//
// WHY THIS FILE EXISTS:
// The early-cleanup rewrites are the optimizer's workhorse vocabulary.
// Every rule here is a LOCAL, exact-semantics transformation: the
// replacement value is bit-identical to the original for EVERY runtime
// input, or the rewrite does not fire. The exact-bits discipline for
// floating point (fold only when neither input nor result is NaN; never
// commute FP operands; never fold FP additive identities) is what makes
// "exact" true on every IEEE-754 host without appeal to JLS NaN-bit
// freedom. Rule 72 (Java numeric semantics preserved exactly) is the law
// this file implements; docs/pass_contracts.md section 5 is the rule
// table that reviewers diff.

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>

#include "PassInternal.h"

namespace b2::passes::detail {

// constOf/isConst DEFINITIONS (declared in PassInternal.h; the flavor
// constructors and the evaluators live there / below).
ConstVal constOf(const ir::Graph& g, ir::NodeId n) {
  ConstVal c;
  const ir::Node& node = g.node(n);
  if (node.isDead()) {
    return c;
  }
  switch (node.kind) {
  case ir::NodeKind::ConstantI:
    c.flavor = CV::I;
    c.i = static_cast<std::int32_t>(node.constValue);
    return c;
  case ir::NodeKind::ConstantL:
    c.flavor = CV::L;
    c.l = node.constValue;
    return c;
  case ir::NodeKind::ConstantF:
    c.flavor = CV::F;
    c.fb = static_cast<std::uint32_t>(node.constValue);
    return c;
  case ir::NodeKind::ConstantD:
    c.flavor = CV::D;
    c.db = static_cast<std::uint64_t>(node.constValue);
    return c;
  case ir::NodeKind::ConstantNull:
    c.flavor = CV::Null;
    return c;
  default:
    return c;
  }
}

bool isConst(const ConstVal& c) {
  return c.flavor != CV::None;
}

namespace {

// Named sweep budget (Rule 23): re-sweeps cover cascades where a rewrite
// at a high id enables one at a lower id (replacement rewiring can move
// defs above uses in id order). Monotone: each sweep strictly shrinks the
// live-node set or stops.
constexpr std::uint32_t kMaxSimplifySweeps = 8;

[[nodiscard]] float bitsToF(std::uint32_t b) {
  float f = 0.0f;
  std::memcpy(&f, &b, sizeof(f));
  return f;
}

[[nodiscard]] std::uint32_t fToBits(float f) {
  std::uint32_t b = 0;
  std::memcpy(&b, &f, sizeof(b));
  return b;
}

[[nodiscard]] double bitsToD(std::uint64_t b) {
  double d = 0.0;
  std::memcpy(&d, &b, sizeof(d));
  return d;
}

[[nodiscard]] std::uint64_t dToBits(double d) {
  std::uint64_t b = 0;
  std::memcpy(&b, &d, sizeof(b));
  return b;
}

// JLS 5.1.3 narrowing float/double -> int/long: NaN -> 0, +-Inf ->
// MAX/MIN, else round toward zero. Host static_cast is UB out of range,
// so the rule is spelled out.
[[nodiscard]] std::int64_t fpToInt64(double v, bool isLong) {
  if (std::isnan(v)) {
    return 0;
  }
  if (v >= 9.2233720368547758e18) {
    return isLong ? INT64_MAX
                  : static_cast<std::int64_t>(INT32_MAX);
  }
  if (v <= -9.2233720368547758e18) {
    return isLong ? INT64_MIN
                  : static_cast<std::int64_t>(INT32_MIN);
  }
  if (!isLong) {
    if (v > 2147483647.0) {
      return INT32_MAX;
    }
    if (v < -2147483648.0) {
      return INT32_MIN;
    }
  }
  return static_cast<std::int64_t>(v); // in-range truncation
}

// Integer division/remainder with JVM wrapping semantics (INT_MIN / -1
// wraps to INT_MIN; C++ makes that UB, so it is special-cased).
[[nodiscard]] std::int32_t javaDivI(std::int32_t a, std::int32_t b) {
  if (a == INT32_MIN && b == -1) {
    return INT32_MIN;
  }
  return a / b;
}

[[nodiscard]] std::int32_t javaRemI(std::int32_t a, std::int32_t b) {
  if (a == INT32_MIN && b == -1) {
    return 0;
  }
  return a % b;
}

[[nodiscard]] std::int64_t javaDivL(std::int64_t a, std::int64_t b) {
  if (a == INT64_MIN && b == -1) {
    return INT64_MIN;
  }
  return a / b;
}

[[nodiscard]] std::int64_t javaRemL(std::int64_t a, std::int64_t b) {
  if (a == INT64_MIN && b == -1) {
    return 0;
  }
  return a % b;
}

// --- fold helpers: (kind, constants) -> replacement node -------------------

// Integer binary folds with exact wrap semantics.
[[nodiscard]] std::optional<std::int64_t> foldIntBin(ir::NodeKind k,
                                                     std::int32_t a,
                                                     std::int32_t b) {
  using K = ir::NodeKind;
  const std::uint32_t ua = static_cast<std::uint32_t>(a);
  const std::uint32_t ub = static_cast<std::uint32_t>(b);
  switch (k) {
  case K::AddI:
    return static_cast<std::int32_t>(ua + ub);
  case K::SubI:
    return static_cast<std::int32_t>(ua - ub);
  case K::MulI:
    return static_cast<std::int32_t>(ua * ub);
  case K::AndI:
    return a & b;
  case K::OrI:
    return a | b;
  case K::XorI:
    return a ^ b;
  case K::ShlI:
    return static_cast<std::int32_t>(ua << (b & 31u));
  case K::ShrI:
    return a >> (b & 31u);
  case K::UShrI:
    return static_cast<std::int32_t>(ua >> (b & 31u));
  case K::DivI:
    if (b == 0) {
      return std::nullopt; // trap: leave to the guard
    }
    return javaDivI(a, b);
  case K::RemI:
    if (b == 0) {
      return std::nullopt;
    }
    return javaRemI(a, b);
  case K::CmpI:
    return static_cast<std::int64_t>((a > b) - (a < b));
  case K::EqI:
    return a == b ? 1 : 0;
  case K::NeI:
    return a != b ? 1 : 0;
  case K::LtI:
    return a < b ? 1 : 0;
  case K::LeI:
    return a <= b ? 1 : 0;
  case K::GtI:
    return a > b ? 1 : 0;
  case K::GeI:
    return a >= b ? 1 : 0;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<std::int64_t> foldLongBin(ir::NodeKind k,
                                                      std::int64_t a,
                                                      std::int64_t b) {
  using K = ir::NodeKind;
  const std::uint64_t ua = static_cast<std::uint64_t>(a);
  const std::uint64_t ub = static_cast<std::uint64_t>(b);
  switch (k) {
  case K::AddL:
    return static_cast<std::int64_t>(ua + ub);
  case K::SubL:
    return static_cast<std::int64_t>(ua - ub);
  case K::MulL:
    return static_cast<std::int64_t>(ua * ub);
  case K::AndL:
    return a & b;
  case K::OrL:
    return a | b;
  case K::XorL:
    return a ^ b;
  case K::ShlL:
    return static_cast<std::int64_t>(ua << (b & 63u));
  case K::ShrL:
    return a >> (b & 63u);
  case K::UShrL:
    return static_cast<std::int64_t>(ua >> (b & 63u));
  case K::DivL:
    if (b == 0) {
      return std::nullopt;
    }
    return javaDivL(a, b);
  case K::RemL:
    if (b == 0) {
      return std::nullopt;
    }
    return javaRemL(a, b);
  case K::CmpL:
    return static_cast<std::int64_t>((a > b) - (a < b));
  default:
    return std::nullopt;
  }
}

// Float folds: exact-bits policy - fold only when neither input is NaN and
// the computed result is not NaN. Div by zero producing +-Inf is defined
// and exact, so it folds.
[[nodiscard]] std::optional<std::uint32_t> foldFloatBin(ir::NodeKind k,
                                                        float a, float b) {
  using K = ir::NodeKind;
  if (std::isnan(a) || std::isnan(b)) {
    return std::nullopt;
  }
  float r = 0.0f;
  switch (k) {
  case K::AddF:
    r = a + b;
    break;
  case K::SubF:
    r = a - b;
    break;
  case K::MulF:
    r = a * b;
    break;
  case K::DivF:
    r = a / b;
    break;
  case K::RemF:
    if (b == 0.0f || std::isinf(a)) {
      return std::nullopt; // NaN by IEEE
    }
    r = std::fmod(a, b);
    break;
  default:
    return std::nullopt;
  }
  if (std::isnan(r)) {
    return std::nullopt;
  }
  return fToBits(r);
}

[[nodiscard]] std::optional<std::uint64_t> foldDoubleBin(ir::NodeKind k,
                                                         double a, double b) {
  using K = ir::NodeKind;
  if (std::isnan(a) || std::isnan(b)) {
    return std::nullopt;
  }
  double r = 0.0;
  switch (k) {
  case K::AddD:
    r = a + b;
    break;
  case K::SubD:
    r = a - b;
    break;
  case K::MulD:
    r = a * b;
    break;
  case K::DivD:
    r = a / b;
    break;
  case K::RemD:
    if (b == 0.0 || std::isinf(a)) {
      return std::nullopt;
    }
    r = std::fmod(a, b);
    break;
  default:
    return std::nullopt;
  }
  if (std::isnan(r)) {
    return std::nullopt;
  }
  return dToBits(r);
}

// One-input folds delegate the SEMANTICS to detail::evalUnaryOp (below)
// and only create the replacement node: one Java-semantics source of truth
// shared with SCCP's lattice evaluator.
[[nodiscard]] std::optional<ir::NodeId> foldUnary(ir::Graph& g,
                                                  ir::NodeKind k,
                                                  const ConstVal& a) {
  if (!isConst(a)) {
    return std::nullopt;
  }
  const auto r = evalUnaryOp(k, a);
  if (!r) {
    return std::nullopt;
  }
  return makeConstNode(g, *r);
}

// Value-level folds that need graph context (never-null facts, node
// identity, object shape) - IsNull, RefEq, InstanceOf.
[[nodiscard]] std::optional<ir::NodeId> foldValueFacts(ir::Graph& g,
                                                       ir::NodeKind k,
                                                       ir::NodeId n) {
  using K = ir::NodeKind;
  switch (k) {
  case K::IsNull: {
    const ir::NodeId x = g.input(n, 0);
    const ConstVal c = constOf(g, x);
    if (c.flavor == CV::Null) {
      return g.constantI(1);
    }
    if (isNeverNullNode(g, x)) {
      return g.constantI(0);
    }
    return std::nullopt;
  }
  case K::RefEq: {
    const ir::NodeId a = g.input(n, 0);
    const ir::NodeId b = g.input(n, 1);
    const ConstVal ca = constOf(g, a);
    const ConstVal cb = constOf(g, b);
    if (a == b) {
      return g.constantI(1); // same SSA value: identical reference
    }
    if (ca.flavor == CV::Null && cb.flavor == CV::Null) {
      return g.constantI(1);
    }
    if ((ca.flavor == CV::Null && isNeverNullNode(g, b)) ||
        (cb.flavor == CV::Null && isNeverNullNode(g, a))) {
      return g.constantI(0);
    }
    return std::nullopt;
  }
  case K::InstanceOf: {
    const ConstVal c = constOf(g, g.input(n, 0));
    if (c.flavor == CV::Null) {
      return g.constantI(0); // null instanceof T == false, always
    }
    return std::nullopt;
  }
  default:
    return std::nullopt;
  }
}

// --- identity removal -------------------------------------------------------

[[nodiscard]] std::optional<ir::NodeId> tryIdentity(ir::Graph& g,
                                                    ir::NodeKind k,
                                                    ir::NodeId n,
                                                    bool isLong) {
  using K = ir::NodeKind;
  const ir::NodeId a = g.input(n, 0);
  const ir::NodeId b = g.input(n, 1);
  const ir::Node& na = g.node(a);
  const ir::Node& nb = g.node(b);
  const auto isI = [&](const ir::Node& nd) {
    return nd.kind == (isLong ? K::ConstantL : K::ConstantI);
  };
  const auto valOf = [&](const ir::Node& nd) {
    return isLong ? nd.constValue : static_cast<std::int64_t>(
                                        static_cast<std::int32_t>(
                                            nd.constValue));
  };
  const std::int64_t zero = 0;
  const std::int64_t one = 1;

  // Self-operand identities (commutative and comparison kinds).
  if (a == b) {
    switch (k) {
    case K::SubI:
    case K::SubL:
      return isLong ? g.constantL(0) : g.constantI(0);
    case K::AndI:
    case K::AndL:
    case K::OrI:
    case K::OrL:
      return a; // x & x == x, x | x == x (bit-exact)
    case K::XorI:
    case K::XorL:
      return isLong ? g.constantL(0) : g.constantI(0);
    case K::CmpI:
    case K::CmpL:
      return g.constantI(0); // comparisons return int, not long
    case K::EqI:
    case K::LeI:
    case K::GeI:
      return g.constantI(1); // int/long self-compare: total order
    case K::NeI:
    case K::LtI:
    case K::GtI:
      return g.constantI(0);
    default:
      break;
    }
  }

  // Constant-operand identities (either side for commutative kinds).
  switch (k) {
  case K::AddI:
  case K::AddL:
    if (isI(na) && valOf(na) == zero) {
      return b;
    }
    if (isI(nb) && valOf(nb) == zero) {
      return a;
    }
    return std::nullopt;
  case K::SubI:
  case K::SubL:
    if (isI(nb) && valOf(nb) == zero) {
      return a;
    }
    return std::nullopt;
  case K::MulI:
  case K::MulL:
    if (isI(na) && valOf(na) == one) {
      return b;
    }
    if (isI(nb) && valOf(nb) == one) {
      return a;
    }
    if ((isI(na) && valOf(na) == zero) || (isI(nb) && valOf(nb) == zero)) {
      return isLong ? g.constantL(0) : g.constantI(0);
    }
    return std::nullopt;
  case K::AndI:
  case K::AndL:
    if (isI(na) && valOf(na) == -one) {
      return b;
    }
    if (isI(nb) && valOf(nb) == -one) {
      return a;
    }
    if ((isI(na) && valOf(na) == zero) || (isI(nb) && valOf(nb) == zero)) {
      return isLong ? g.constantL(0) : g.constantI(0);
    }
    return std::nullopt;
  case K::OrI:
  case K::OrL:
    if (isI(na) && valOf(na) == zero) {
      return b;
    }
    if (isI(nb) && valOf(nb) == zero) {
      return a;
    }
    if ((isI(na) && valOf(na) == -one) || (isI(nb) && valOf(nb) == -one)) {
      return isLong ? g.constantL(-1) : g.constantI(-1);
    }
    return std::nullopt;
  case K::XorI:
  case K::XorL:
    if (isI(na) && valOf(na) == zero) {
      return b;
    }
    if (isI(nb) && valOf(nb) == zero) {
      return a;
    }
    return std::nullopt;
  case K::ShlI:
  case K::ShrI:
  case K::UShrI:
  case K::ShlL:
  case K::ShrL:
  case K::UShrL:
    // Shift count zero (the count operand is Int even for long shifts).
    if (nb.kind == K::ConstantI && static_cast<std::int32_t>(nb.constValue) == 0) {
      return a;
    }
    return std::nullopt;
  case K::DivI:
  case K::DivL:
    if (isI(nb) && valOf(nb) == one) {
      return a;
    }
    return std::nullopt;
  case K::RemI:
  case K::RemL:
    if (isI(nb) && valOf(nb) == one) {
      return isLong ? g.constantL(0) : g.constantI(0);
    }
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

// Negation-of-negation (exact for both integer wrap and FP bit flip).
[[nodiscard]] std::optional<ir::NodeId> tryDoubleNeg(ir::Graph& g,
                                                     ir::NodeKind k,
                                                     ir::NodeId n) {
  using K = ir::NodeKind;
  switch (k) {
  case K::NegI:
  case K::NegL:
  case K::NegF:
  case K::NegD:
    break;
  default:
    return std::nullopt;
  }
  const ir::NodeId inner = g.input(n, 0);
  const ir::Node& ni = g.node(inner);
  if (ni.isDead() || ni.kind != k) {
    return std::nullopt;
  }
  // -( -x ) == x exactly: integer wrap is an involution, FP negation is a
  // sign-bit flip (payload-preserving).
  return g.input(inner, 0);
}

// Round-trip conversion pairs that are exact identities.
[[nodiscard]] std::optional<ir::NodeId> tryConversionIdentity(ir::Graph& g,
                                                              ir::NodeKind k,
                                                              ir::NodeId n) {
  using K = ir::NodeKind;
  const ir::NodeId inner = g.input(n, 0);
  const ir::Node& ni = g.node(inner);
  if (ni.isDead()) {
    return std::nullopt;
  }
  switch (k) {
  case K::L2I:
    // L2I(I2L(x)) == x: sign-extend then truncate low 32 bits.
    if (ni.kind == K::I2L) {
      return g.input(inner, 0);
    }
    return std::nullopt;
  case K::I2B:
  case K::I2C:
  case K::I2S:
    // Narrowing is idempotent: (T)(T)x == (T)x.
    if (ni.kind == k) {
      return inner;
    }
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

// Not(Not(t)) == t when t provably produces 0/1 (boolean tests); a general
// int x has Not(Not(x)) == (x != 0), NOT x - so only test kinds qualify.
[[nodiscard]] std::optional<ir::NodeId> tryNotNot(ir::Graph& g,
                                                  ir::NodeKind k,
                                                  ir::NodeId n) {
  if (k != ir::NodeKind::Not) {
    return std::nullopt;
  }
  const ir::NodeId inner = g.input(n, 0);
  const ir::Node& ni = g.node(inner);
  if (ni.isDead() || ni.kind != ir::NodeKind::Not) {
    return std::nullopt;
  }
  const ir::NodeId inner2 = g.input(inner, 0);
  const ir::Node& n2 = g.node(inner2);
  switch (n2.kind) {
  case ir::NodeKind::EqI:
  case ir::NodeKind::NeI:
  case ir::NodeKind::LtI:
  case ir::NodeKind::LeI:
  case ir::NodeKind::GtI:
  case ir::NodeKind::GeI:
  case ir::NodeKind::IsNull:
  case ir::NodeKind::RefEq:
    return inner2;
  default:
    return std::nullopt;
  }
}

// --- strength reduction -----------------------------------------------------

[[nodiscard]] std::optional<ir::NodeId> tryStrength(ir::Graph& g,
                                                    ir::NodeKind k,
                                                    ir::NodeId n) {
  using K = ir::NodeKind;
  const bool isMulI = k == K::MulI;
  const bool isMulL = k == K::MulL;
  if (!isMulI && !isMulL) {
    return std::nullopt;
  }
  const ir::Node& na = g.node(g.input(n, 0));
  const ir::Node& nb = g.node(g.input(n, 1));
  // The constant may sit on either side (commutative).
  for (const ir::Node* nd : {&na, &nb}) {
    if (isMulI && nd->kind != K::ConstantI) {
      continue;
    }
    if (isMulL && nd->kind != K::ConstantL) {
      continue;
    }
    const std::uint64_t raw =
        isMulI ? static_cast<std::uint32_t>(nd->constValue)
               : static_cast<std::uint64_t>(nd->constValue);
    // c > 1, power of two: x * 2^k == x << k under wrap semantics.
    if (raw > 1 && (raw & (raw - 1)) == 0) {
      const unsigned shift = static_cast<unsigned>(std::countr_zero(raw));
      const ir::NodeId x =
          (nd == &na) ? g.input(n, 1) : g.input(n, 0);
      const ir::NodeId shiftK = g.constantI(static_cast<std::int32_t>(shift));
      const ir::NodeKind shl = isMulI ? K::ShlI : K::ShlL;
      return g.make(shl, {x, shiftK});
    }
  }
  return std::nullopt;
}

// --- canonicalization -------------------------------------------------------

[[nodiscard]] bool isCommutativeInt(ir::NodeKind k) {
  switch (k) {
  case ir::NodeKind::AddI:
  case ir::NodeKind::MulI:
  case ir::NodeKind::AndI:
  case ir::NodeKind::OrI:
  case ir::NodeKind::XorI:
  case ir::NodeKind::AddL:
  case ir::NodeKind::MulL:
  case ir::NodeKind::AndL:
  case ir::NodeKind::OrL:
  case ir::NodeKind::XorL:
    return true;
  default:
    return false;
  }
}

// Not(test) -> complement test. Returns the complement kind or nullopt.
[[nodiscard]] std::optional<ir::NodeKind> complementTest(ir::NodeKind k) {
  using K = ir::NodeKind;
  switch (k) {
  case K::EqI:
    return K::NeI;
  case K::NeI:
    return K::EqI;
  case K::LtI:
    return K::GeI;
  case K::GeI:
    return K::LtI;
  case K::LeI:
    return K::GtI;
  case K::GtI:
    return K::LeI;
  default:
    return std::nullopt;
  }
}

// --- trivial phi ------------------------------------------------------------

// A phi whose value inputs (ignoring self inputs - the loop-invariant
// marker) are all one node is that node. A phi with exactly one value
// input is that input (single-pred region pending, or after pred drops).
[[nodiscard]] std::optional<ir::NodeId> tryTrivialPhi(const ir::Graph& g,
                                                      ir::NodeId n) {
  const ir::Node& node = g.node(n);
  if (node.kind != ir::NodeKind::Phi || node.numInputs < 1) {
    return std::nullopt;
  }
  ir::NodeId distinct = ir::kInvalidNodeId;
  for (std::uint16_t s = 1; s < node.numInputs; ++s) {
    const ir::NodeId v = g.input(n, s);
    if (v == n) {
      continue; // self input: loop-invariant marker, not a value
    }
    if (distinct == ir::kInvalidNodeId) {
      distinct = v;
    } else if (distinct != v) {
      return std::nullopt;
    }
  }
  if (distinct == ir::kInvalidNodeId) {
    return std::nullopt; // all self: malformed, leave to the verifier
  }
  return distinct;
}

} // namespace

// --- the shared value-level fold evaluators (keys 14 + 38) -------------------
//
// Defined here, beside the arithmetic helpers they call (the anonymous-
// namespace members above are visible at detail scope). SCCP.cpp calls
// these through PassInternal.h - the semantics table reviewers diff
// lives once, not twice.

std::optional<ConstVal> evalBinOp(ir::NodeKind k, const ConstVal& a,
                                  const ConstVal& b) {
  using K = ir::NodeKind;
  if (a.flavor == CV::I && b.flavor == CV::I) {
    const auto r = foldIntBin(k, a.i, b.i);
    if (!r) {
      return std::nullopt;
    }
    ConstVal c;
    c.flavor = CV::I;
    c.i = static_cast<std::int32_t>(*r);
    return c;
  }
  if (a.flavor == CV::L && b.flavor == CV::L) {
    const auto r = foldLongBin(k, a.l, b.l);
    if (!r) {
      return std::nullopt;
    }
    // CmpL produces an INT result (-1/0/1) even though its operands are
    // Long; every other long op produces Long.
    ConstVal c;
    if (k == K::CmpL) {
      c.flavor = CV::I;
      c.i = static_cast<std::int32_t>(*r);
    } else {
      c.flavor = CV::L;
      c.l = *r;
    }
    return c;
  }
  if (a.flavor == CV::F && b.flavor == CV::F) {
    if (const auto r = foldFloatBin(k, bitsToF(a.fb), bitsToF(b.fb))) {
      ConstVal c;
      c.flavor = CV::F;
      c.fb = *r;
      return c;
    }
    // Float COMPARISONS fold even on NaN inputs: the result is a defined
    // int (NaN compares less/greater per the kind), so the exact-bits
    // policy does not block them.
    if (k == K::CmpFl || k == K::CmpFg) {
      const double x = static_cast<double>(bitsToF(a.fb));
      const double y = static_cast<double>(bitsToF(b.fb));
      std::int32_t r = 0;
      if (k == K::CmpFl) {
        r = (std::isnan(x) || std::isnan(y))
                ? -1
                : static_cast<std::int32_t>((x > y) - (x < y));
      } else {
        r = (std::isnan(x) || std::isnan(y))
                ? 1
                : static_cast<std::int32_t>((x > y) - (x < y));
      }
      ConstVal c;
      c.flavor = CV::I;
      c.i = r;
      return c;
    }
    return std::nullopt;
  }
  if (a.flavor == CV::D && b.flavor == CV::D) {
    if (const auto r = foldDoubleBin(k, bitsToD(a.db), bitsToD(b.db))) {
      ConstVal c;
      c.flavor = CV::D;
      c.db = *r;
      return c;
    }
    if (k == K::CmpDl || k == K::CmpDg) {
      const double x = bitsToD(a.db);
      const double y = bitsToD(b.db);
      std::int32_t r = 0;
      if (k == K::CmpDl) {
        r = (std::isnan(x) || std::isnan(y))
                ? -1
                : static_cast<std::int32_t>((x > y) - (x < y));
      } else {
        r = (std::isnan(x) || std::isnan(y))
                ? 1
                : static_cast<std::int32_t>((x > y) - (x < y));
      }
      ConstVal c;
      c.flavor = CV::I;
      c.i = r;
      return c;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<ConstVal> evalUnaryOp(ir::NodeKind k, const ConstVal& a) {
  using K = ir::NodeKind;
  if (!isConst(a)) {
    return std::nullopt;
  }
  switch (k) {
  case K::NegI:
    if (a.flavor == CV::I) {
      return constOfI(
          static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(a.i)));
    }
    return std::nullopt;
  case K::Not:
    if (a.flavor == CV::I) {
      return constOfI(a.i == 0 ? 1 : 0);
    }
    return std::nullopt;
  case K::NegL:
    if (a.flavor == CV::L) {
      return constOfL(
          static_cast<std::int64_t>(0u - static_cast<std::uint64_t>(a.l)));
    }
    return std::nullopt;
  case K::NegF:
    // Exact bit flip: sign XOR 0x80000000, NaN payload preserved.
    if (a.flavor == CV::F) {
      return constOfF(a.fb ^ 0x80000000u);
    }
    return std::nullopt;
  case K::NegD:
    if (a.flavor == CV::D) {
      return constOfD(a.db ^ 0x8000000000000000ull);
    }
    return std::nullopt;
  case K::I2L:
    if (a.flavor == CV::I) {
      return constOfL(a.i);
    }
    return std::nullopt;
  case K::L2I:
    if (a.flavor == CV::L) {
      return constOfI(
          static_cast<std::int32_t>(static_cast<std::uint32_t>(a.l)));
    }
    return std::nullopt;
  case K::I2B:
    if (a.flavor == CV::I) {
      return constOfI(
          static_cast<std::int32_t>(static_cast<std::int8_t>(a.i)));
    }
    return std::nullopt;
  case K::I2C:
    if (a.flavor == CV::I) {
      return constOfI(a.i & 0xFFFF);
    }
    return std::nullopt;
  case K::I2S:
    if (a.flavor == CV::I) {
      return constOfI(
          static_cast<std::int32_t>(static_cast<std::int16_t>(a.i)));
    }
    return std::nullopt;
  case K::I2F:
    if (a.flavor == CV::I) {
      return constOfF(fToBits(static_cast<float>(a.i)));
    }
    return std::nullopt;
  case K::I2D:
    if (a.flavor == CV::I) {
      return constOfD(dToBits(static_cast<double>(a.i)));
    }
    return std::nullopt;
  case K::L2F:
    if (a.flavor == CV::L) {
      return constOfF(fToBits(static_cast<float>(a.l)));
    }
    return std::nullopt;
  case K::L2D:
    if (a.flavor == CV::L) {
      return constOfD(dToBits(static_cast<double>(a.l)));
    }
    return std::nullopt;
  case K::F2I:
    if (a.flavor == CV::F) {
      return constOfI(static_cast<std::int32_t>(
          fpToInt64(bitsToF(a.fb), false)));
    }
    return std::nullopt;
  case K::F2L:
    if (a.flavor == CV::F) {
      return constOfL(fpToInt64(bitsToF(a.fb), true));
    }
    return std::nullopt;
  case K::F2D:
    if (a.flavor == CV::F && !std::isnan(bitsToF(a.fb))) {
      return constOfD(dToBits(static_cast<double>(bitsToF(a.fb))));
    }
    return std::nullopt;
  case K::D2I:
    if (a.flavor == CV::D) {
      return constOfI(static_cast<std::int32_t>(
          fpToInt64(bitsToD(a.db), false)));
    }
    return std::nullopt;
  case K::D2L:
    if (a.flavor == CV::D) {
      return constOfL(fpToInt64(bitsToD(a.db), true));
    }
    return std::nullopt;
  case K::D2F:
    if (a.flavor == CV::D && !std::isnan(bitsToD(a.db))) {
      const float r = static_cast<float>(bitsToD(a.db));
      if (std::isnan(r)) {
        return std::nullopt; // only from NaN input (checked above): safety
      }
      return constOfF(fToBits(r));
    }
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

ir::NodeId makeConstNode(ir::Graph& g, const ConstVal& c) {
  switch (c.flavor) {
  case CV::I:
    return g.constantI(c.i);
  case CV::L:
    return g.constantL(c.l);
  case CV::F:
    return g.constantF(bitsToF(c.fb));
  case CV::D:
    return g.constantD(bitsToD(c.db));
  case CV::Null:
    return g.constantNull();
  default:
    return ir::kInvalidNodeId;
  }
}

void runSimplify(ir::Graph& g, std::uint32_t classMask, PassTelemetry& t,
                 Budget& b, const Junk& jk) {
  for (std::uint32_t sweep = 0; sweep < kMaxSimplifySweeps; ++sweep) {
    bool changed = false;
    for (ir::NodeId n = 0; n < g.nodeCount() && !b.exceeded; ++n) {
      const ir::Node& node = g.node(n);
      if (node.isDead()) {
        continue;
      }
      const ir::NodeKind k = node.kind;
      std::optional<ir::NodeId> rep;
      bool swapOperands = false;

      if ((classMask & kTrivialPhi) != 0) {
        rep = tryTrivialPhi(g, n);
      }

      if (!rep && (classMask & kFold) != 0) {
        if (node.numInputs == 2) {
          const ConstVal a = constOf(g, g.input(n, 0));
          const ConstVal bIn = constOf(g, g.input(n, 1));
          if (isConst(a) && isConst(bIn)) {
            // One evaluator, shared with SCCP's lattice: integer/long
            // arithmetic + comparisons, exact-bits FP arithmetic, and
            // the NaN-tolerant FP comparisons (CmpFl/CmpFg/CmpDl/CmpDg).
            if (const auto r = evalBinOp(k, a, bIn)) {
              rep = makeConstNode(g, *r);
            }
          }
        }
        if (!rep && node.numInputs == 1) {
          rep = foldUnary(g, k, constOf(g, g.input(n, 0)));
        }
        if (!rep) {
          rep = foldValueFacts(g, k, n);
        }
      }

      if (!rep && (classMask & kIdentity) != 0) {
        if (node.numInputs == 2) {
          const bool isLong = k == ir::NodeKind::AddL ||
                              k == ir::NodeKind::SubL ||
                              k == ir::NodeKind::MulL ||
                              k == ir::NodeKind::AndL ||
                              k == ir::NodeKind::OrL ||
                              k == ir::NodeKind::XorL ||
                              k == ir::NodeKind::DivL ||
                              k == ir::NodeKind::RemL;
          rep = tryIdentity(g, k, n, isLong);
        }
        if (!rep) {
          rep = tryDoubleNeg(g, k, n);
        }
        if (!rep) {
          rep = tryConversionIdentity(g, k, n);
        }
        if (!rep) {
          rep = tryNotNot(g, k, n);
        }
      }

      if (!rep && (classMask & kStrength) != 0) {
        rep = tryStrength(g, k, n);
      }

      if (!rep && (classMask & kCanonical) != 0) {
        if (node.numInputs == 2 && isCommutativeInt(k)) {
          // Commutative integer ops: constant on the RIGHT (enables left-
          // to-right pattern matching downstream). FP is NEVER commuted:
          // NaN payload propagation can be operand-order sensitive on
          // IEEE-754 hardware (exact-bits policy, docs/pass_contracts.md).
          const ir::Node& n0 = g.node(g.input(n, 0));
          const ir::Node& n1 = g.node(g.input(n, 1));
          const bool c0 = n0.kind == ir::NodeKind::ConstantI ||
                          n0.kind == ir::NodeKind::ConstantL;
          const bool c1 = n1.kind == ir::NodeKind::ConstantI ||
                          n1.kind == ir::NodeKind::ConstantL;
          if (c0 && !c1 && !n1.isDead()) {
            swapOperands = true;
          }
        } else if (k == ir::NodeKind::Not && node.numInputs == 1) {
          // Not(test(a, b)) -> complement-test(a, b)
          const ir::NodeId inner = g.input(n, 0);
          const ir::Node& ni = g.node(inner);
          if (!ni.isDead() && ni.numInputs == 2) {
            if (const auto comp = complementTest(ni.kind)) {
              const ir::NodeId a = g.input(inner, 0);
              const ir::NodeId bIn = g.input(inner, 1);
              rep = g.make(*comp, {a, bIn});
            }
          }
        }
      }

      if (rep && *rep != n) {
        if (*rep < g.nodeCount() && g.node(*rep).isDead()) {
          continue; // never rewire onto a tombstone
        }
        const std::uint32_t rewritesBefore = t.rewrites;
        replace(g, n, *rep, t, b, jk);
        if (t.rewrites != rewritesBefore) {
          ++t.folds;
          changed = true;
        }
      } else if (swapOperands) {
        const ir::NodeId a = g.input(n, 0);
        const ir::NodeId bIn = g.input(n, 1);
        g.setInput(n, 0, bIn);
        g.setInput(n, 1, a);
        ++t.folds;
        ++t.rewrites;
        changed = true;
      }
    }
    if (!changed) {
      break; // fixpoint: no rewrite fired in a full id-order sweep
    }
  }
}

} // namespace b2::passes::detail
