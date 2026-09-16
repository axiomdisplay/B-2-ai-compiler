// TurboScript — BigInt arithmetic (sign + 32-bit limbs, little-endian).
// Semantics per Rule 72 / ECMA-262 BigInt: truncating division, remainder
// with dividend sign, canonical zero.
#include "ts_value.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ts {

// Strip trailing zero limbs and clear sign on zero (canonical form).
static void canonicalize(BigInt* x) {
  while (!x->limbs.empty() && x->limbs.back() == 0) x->limbs.pop_back();
  if (x->limbs.empty()) x->negative = false;
}

// a += b*mul<<shift, used by schoolbook division (Knuth D simplify).
static void addMulShift(uint32_t* a, size_t aLen, const uint32_t* b, size_t bLen,
                        uint32_t mul, size_t shift) {
  uint64_t carry = 0;
  for (size_t i = 0; i < bLen; i++) {
    uint64_t cur = static_cast<uint64_t>(a[i + shift]) +
                   static_cast<uint64_t>(b[i]) * mul + carry;
    a[i + shift] = static_cast<uint32_t>(cur & 0xFFFFFFFFu);
    carry = cur >> 32;
  }
  for (size_t i = bLen; carry != 0 && i + shift < aLen; i++) {
    uint64_t cur = static_cast<uint64_t>(a[i + shift]) + carry;
    a[i + shift] = static_cast<uint32_t>(cur & 0xFFFFFFFFu);
    carry = cur >> 32;
  }
}

static int subInPlace(uint32_t* a, const uint32_t* b, size_t len) {
  int64_t borrow = 0;
  for (size_t i = 0; i < len; i++) {
    int64_t cur = static_cast<int64_t>(a[i]) - b[i] - borrow;
    borrow = cur < 0 ? 1 : 0;
    a[i] = static_cast<uint32_t>(cur & 0xFFFFFFFFu);
  }
  return static_cast<int>(borrow);
}

int BigInt::compareMag(const BigInt& a, const BigInt& b) {
  if (a.limbs.size() != b.limbs.size()) {
    return a.limbs.size() < b.limbs.size() ? -1 : 1;
  }
  for (size_t i = a.limbs.size(); i-- > 0;) {
    if (a.limbs[i] != b.limbs[i]) return a.limbs[i] < b.limbs[i] ? -1 : 1;
  }
  return 0;
}

int BigInt::compare(const BigInt& a, const BigInt& b) {
  if (a.sign() != b.sign()) return a.sign() < b.sign() ? -1 : 1;
  int mag = compareMag(a, b);
  return a.negative ? -mag : mag;
}

BigInt BigInt::negate(const BigInt& a) {
  BigInt r = a;
  r.negative = !r.negative;
  canonicalize(&r);
  return r;
}

BigInt BigInt::add(const BigInt& a, const BigInt& b) {
  if (a.negative == b.negative) {
    BigInt r;
    r.negative = a.negative;
    const auto& big = a.limbs.size() >= b.limbs.size() ? a.limbs : b.limbs;
    const auto& small = a.limbs.size() >= b.limbs.size() ? b.limbs : a.limbs;
    r.limbs.reserve(big.size() + 1);
    uint64_t carry = 0;
    for (size_t i = 0; i < big.size(); i++) {
      uint64_t cur = static_cast<uint64_t>(big[i]) + carry +
                     (i < small.size() ? small[i] : 0);
      r.limbs.push_back(static_cast<uint32_t>(cur & 0xFFFFFFFFu));
      carry = cur >> 32;
    }
    if (carry != 0) r.limbs.push_back(static_cast<uint32_t>(carry));
    canonicalize(&r);
    return r;
  }
  // Opposite signs: a + b = a - (-b).
  BigInt nb = b;
  nb.negative = !nb.negative;
  return sub(a, nb);
}

BigInt BigInt::sub(const BigInt& a, const BigInt& b) {
  if (a.negative != b.negative) {
    BigInt nb = b;
    nb.negative = !nb.negative;
    return add(a, nb);
  }
  int cmp = compareMag(a, b);
  if (cmp == 0) return BigInt{};
  BigInt r;
  r.negative = (cmp < 0) ? !a.negative : a.negative;
  const auto& big = cmp >= 0 ? a.limbs : b.limbs;
  const auto& small = cmp >= 0 ? b.limbs : a.limbs;
  r.limbs.resize(big.size(), 0);
  std::copy(big.begin(), big.end(), r.limbs.begin());
  subInPlace(r.limbs.data(), small.data(), small.size());
  canonicalize(&r);
  return r;
}

BigInt BigInt::mul(const BigInt& a, const BigInt& b) {
  if (a.isZero() || b.isZero()) return BigInt{};
  BigInt r;
  r.negative = a.negative != b.negative;
  r.limbs.assign(a.limbs.size() + b.limbs.size(), 0);
  for (size_t i = 0; i < a.limbs.size(); i++) {
    uint64_t carry = 0;
    for (size_t j = 0; j < b.limbs.size(); j++) {
      uint64_t cur = static_cast<uint64_t>(r.limbs[i + j]) +
                     static_cast<uint64_t>(a.limbs[i]) * b.limbs[j] + carry;
      r.limbs[i + j] = static_cast<uint32_t>(cur & 0xFFFFFFFFu);
      carry = cur >> 32;
    }
    size_t k = i + b.limbs.size();
    while (carry != 0) {
      uint64_t cur = static_cast<uint64_t>(r.limbs[k]) + carry;
      r.limbs[k] = static_cast<uint32_t>(cur & 0xFFFFFFFFu);
      carry = cur >> 32;
      k++;
    }
  }
  canonicalize(&r);
  return r;
}

bool BigInt::divMod(const BigInt& a, const BigInt& b, BigInt* quotient,
                    BigInt* remainder) {
  if (b.isZero()) return false;
  int cmp = compareMag(a, b);
  if (cmp < 0) {
    if (quotient) *quotient = BigInt{};
    if (remainder) *remainder = a;
    return true;
  }
  if (b.limbs.size() == 1) {
    // Single-limb divisor: short division.
    uint64_t divisor = b.limbs[0];
    BigInt q;
    q.negative = a.negative != b.negative;
    q.limbs.assign(a.limbs.size(), 0);
    uint64_t rem = 0;
    for (size_t i = a.limbs.size(); i-- > 0;) {
      uint64_t cur = (rem << 32) | a.limbs[i];
      q.limbs[i] = static_cast<uint32_t>(cur / divisor);
      rem = cur % divisor;
    }
    canonicalize(&q);
    BigInt r;
    r.negative = a.negative;  // remainder carries dividend sign
    if (rem != 0) r.limbs.push_back(static_cast<uint32_t>(rem));
    canonicalize(&r);
    if (quotient) *quotient = std::move(q);
    if (remainder) *remainder = std::move(r);
    return true;
  }
  // Schoolbook long division (Knuth Algorithm D) on shifted magnitudes.
  int shift = 0;
  while ((b.limbs.back() << shift) < 0x80000000u) shift++;
  BigInt bn;
  bn.limbs.assign(b.limbs.size(), 0);
  for (size_t i = 0; i < b.limbs.size(); i++) {
    bn.limbs[i] = b.limbs[i] << shift;
    if (i + 1 < b.limbs.size() && shift > 0) {
      bn.limbs[i] |= b.limbs[i + 1] >> (32 - shift);
    }
  }
  std::vector<uint32_t> an(a.limbs.size() + 1, 0);
  for (size_t i = 0; i < a.limbs.size(); i++) {
    an[i] = a.limbs[i] << shift;
    if (i + 1 < a.limbs.size() && shift > 0) {
      an[i] |= a.limbs[i + 1] >> (32 - shift);
    }
  }
  size_t n = bn.limbs.size();
  size_t m = a.limbs.size() - n;
  BigInt q;
  q.negative = a.negative != b.negative;
  q.limbs.assign(m + 1, 0);
  for (size_t j = m + 1; j-- > 0;) {
    uint64_t num = (static_cast<uint64_t>(an[j + n]) << 32) | an[j + n - 1];
    uint64_t qhat = num / bn.limbs[n - 1];
    uint64_t rhat = num % bn.limbs[n - 1];
    while (qhat > 0xFFFFFFFFu ||
           (n >= 2 && qhat * bn.limbs[n - 2] > ((rhat << 32) | an[j + n - 2]))) {
      qhat--;
      rhat += bn.limbs[n - 1];
      if (rhat > 0xFFFFFFFFu) break;
    }
    // Multiply and subtract.
    addMulShift(&an[j], n + 1, bn.limbs.data(), n,
                static_cast<uint32_t>(qhat), 0);
    if (an[j + n] != 0) {  // subtract underflow: add back once.
      qhat--;
      uint64_t carry = 0;
      for (size_t i = 0; i < n; i++) {
        uint64_t cur = static_cast<uint64_t>(an[j + i]) + bn.limbs[i] + carry;
        an[j + i] = static_cast<uint32_t>(cur & 0xFFFFFFFFu);
        carry = cur >> 32;
      }
      an[j + n] = static_cast<uint32_t>(carry & 0xFFFFFFFFu);
    }
    q.limbs[j] = static_cast<uint32_t>(qhat);
  }
  canonicalize(&q);
  BigInt r;
  r.negative = a.negative;
  r.limbs.assign(n, 0);
  for (size_t i = 0; i < n; i++) {
    r.limbs[i] = shift > 0
                     ? (an[i] >> shift) |
                           (i + 1 < n ? an[i + 1] << (32 - shift) : 0)
                     : an[i];
  }
  canonicalize(&r);
  if (quotient) *quotient = std::move(q);
  if (remainder) *remainder = std::move(r);
  return true;
}

BigInt BigInt::fromInt64(int64_t v) {
  BigInt r;
  if (v == 0) return r;
  r.negative = v < 0;
  uint64_t mag = v < 0
                     ? static_cast<uint64_t>(-(v + 1)) + 1  // avoid UB on MIN
                     : static_cast<uint64_t>(v);
  while (mag != 0) {
    r.limbs.push_back(static_cast<uint32_t>(mag & 0xFFFFFFFFu));
    mag >>= 32;
  }
  return r;
}

bool BigInt::fromDouble(double v, BigInt* out) {
  if (std::isnan(v) || std::isinf(v) || v != std::trunc(v)) return false;
  *out = BigInt{};
  if (v == 0) return true;
  out->negative = v < 0;
  double mag = std::fabs(v);
  // Decompose mag = mantissa(1.x) * 2^exp exactly.
  int exp = 0;
  double frac = std::frexp(mag, &exp);  // frac in [0.5, 1)
  // Build limbs from the 53-bit mantissa, then shift by (exp - 53).
  uint64_t mantissa =
      static_cast<uint64_t>(std::ldexp(frac, 53));  // exact: 53-bit integer
  BigInt acc = fromInt64(static_cast<int64_t>(mantissa));
  // mag = mantissa * 2^(exp-53). shift may be negative for |mag| < 2^52:
  // dividing by 2^(-shift) is exact because mag is integral (checked above).
  int shift = exp - 53;
  while (shift >= 32) {
    acc.limbs.insert(acc.limbs.begin(), 0);
    shift -= 32;
  }
  while (shift <= -32) {
    // Drop 32 low bits (they must be zero: mag is an exact integer).
    if (!acc.limbs.empty()) acc.limbs.erase(acc.limbs.begin());
    shift += 32;
  }
  if (shift > 0) {
    // Multiply magnitude by 2^shift.
    uint32_t carry = 0;
    for (size_t i = 0; i < acc.limbs.size(); i++) {
      uint64_t cur = (static_cast<uint64_t>(acc.limbs[i]) << shift) | carry;
      acc.limbs[i] = static_cast<uint32_t>(cur & 0xFFFFFFFFu);
      carry = cur >> 32;
    }
    if (carry != 0) acc.limbs.push_back(carry);
  } else if (shift < 0) {
    // Exact right-shift by -shift (< 32 bits).
    int s = -shift;
    uint64_t carry = 0;
    for (size_t i = acc.limbs.size(); i-- > 0;) {
      uint64_t cur = (carry << 32) | acc.limbs[i];
      carry = cur & ((1ull << s) - 1);
      acc.limbs[i] = static_cast<uint32_t>(cur >> s);
    }
  }
  *out = std::move(acc);
  canonicalize(out);
  out->negative = v < 0;
  return true;
}

double BigInt::toDouble(bool* ok) const {
  *ok = true;
  if (limbs.empty()) return 0.0;
  if (limbs.size() > 64) {  // > 2048 bits cannot be represented; saturate.
    double d = std::numeric_limits<double>::infinity();
    return negative ? -d : d;
  }
  double acc = 0.0;
  for (size_t i = limbs.size(); i-- > 0;) {
    acc = acc * 4294967296.0 + static_cast<double>(limbs[i]);
  }
  return negative ? -acc : acc;
}

bool BigInt::fromString(std::u16string_view s, BigInt* out) {
  *out = BigInt{};
  size_t i = 0;
  bool neg = false;
  if (i < s.size() && (s[i] == u'-' || s[i] == u'+')) {
    neg = s[i] == u'-';
    i++;
  }
  if (i >= s.size()) return false;
  BigInt ten = fromInt64(10);
  BigInt acc;
  for (; i < s.size(); i++) {
    if (s[i] == u'_') continue;  // numeric separator, allowed between digits
    if (s[i] < u'0' || s[i] > u'9') return false;
    uint32_t digit = static_cast<uint32_t>(s[i] - u'0');
    acc = mul(acc, ten);
    acc = add(acc, fromInt64(digit));
    if (acc.limbs.size() > kMaxBigIntBits / 32) return false;
  }
  acc.negative = neg;
  canonicalize(&acc);
  *out = std::move(acc);
  return true;
}

std::u16string BigInt::toString() const {
  if (limbs.empty()) return u"0";
  // Repeated division by 10^9, digits collected little-endian.
  BigInt cur = *this;
  cur.negative = false;
  const uint64_t chunk = 1000000000ull;
  std::vector<uint32_t> chunks;
  while (!cur.limbs.empty()) {
    uint64_t rem = 0;
    for (size_t i = cur.limbs.size(); i-- > 0;) {
      uint64_t curVal = (rem << 32) | cur.limbs[i];
      cur.limbs[i] = static_cast<uint32_t>(curVal / chunk);
      rem = curVal % chunk;
    }
    while (!cur.limbs.empty() && cur.limbs.back() == 0) cur.limbs.pop_back();
    chunks.push_back(static_cast<uint32_t>(rem));
  }
  std::u16string out;
  if (negative) out.push_back(u'-');
  // Most significant chunk printed without padding, rest zero-padded to 9.
  auto appendChunk = [&](uint32_t c, bool pad) {
    char buf[16];
    int len = std::snprintf(buf, sizeof buf, pad ? "%09u" : "%u", c);
    for (int i = 0; i < len; i++) out.push_back(static_cast<char16_t>(buf[i]));
  };
  for (size_t i = chunks.size(); i-- > 0;) {
    appendChunk(chunks[i], i + 1 != chunks.size());
  }
  return out;
}

// ---------------------------------------------------------------------------
// StringObj::flat — in-place materialization of cons nodes (v0.6).
// Iterative DFS with an explicit stack: `s += piece` loops build left-
// leaning trees of depth == iteration count, so a recursive flatten would
// overflow the machine stack long before kMaxCallDepth matters. After this
// runs, the node is flat (kind == kFlat, links nulled) and every other node
// that referenced it observes the materialized text — flattening is a
// representation change with zero semantic surface (Rule 96).
// ---------------------------------------------------------------------------
const std::u16string& StringObj::flat() const {
  if (kind == kFlat) return data;
  StringObj* self = const_cast<StringObj*>(this);
  std::u16string out;
  out.reserve(length);
  std::vector<const StringObj*> stack;
  stack.reserve(16);
  stack.push_back(self);
  while (!stack.empty()) {
    const StringObj* n = stack.back();
    stack.pop_back();
    if (n->kind == kFlat) {
      out.append(n->data);
    } else {
      // Push right first so the left subtree materializes first (text order).
      stack.push_back(n->right);
      stack.push_back(n->left);
    }
  }
  self->left = nullptr;
  self->right = nullptr;
  self->kind = kFlat;
  self->data = std::move(out);
  return self->data;
}

// ---------------------------------------------------------------------------
// Pure value helpers.
// ---------------------------------------------------------------------------
namespace pure {

bool toBoolean(const Value& v) {
  switch (v.kind()) {
    case ValueKind::Undefined:
    case ValueKind::Null:
      return false;
    case ValueKind::Boolean:
      return v.asBool();
    case ValueKind::Smi:
      return v.asSmi() != 0;
    case ValueKind::HeapNumber:
      // NaN != 0.0 is true -> NaN is truthy (Boolean(NaN) === true, Part 0).
      return v.asDouble() != 0.0;
    case ValueKind::String:
      // v0.6: header length — emptiness never flattens a cons node.
      return v.asString()->length != 0;
    case ValueKind::BigInt:
      return !v.asBigInt()->isZero();
    case ValueKind::Object:
    case ValueKind::Closure:
    case ValueKind::Context:
      return true;
    case ValueKind::Accessor:
    case ValueKind::Hole:
    default:
      return false;  // internal kinds must never reach ToBoolean from JS.
  }
}

int32_t toInt32(double v) {
  if (std::isnan(v) || std::isinf(v) || v == 0.0) return 0;
  double truncated = std::trunc(v);
  // modulo 2^32, then map to signed range.
  double m = std::fmod(truncated, 4294967296.0);  // in (-2^32, 2^32)
  if (m < 0) m += 4294967296.0;
  double signedVal = m >= 2147483648.0 ? m - 4294967296.0 : m;
  return static_cast<int32_t>(signedVal);
}

uint32_t toUint32(double v) {
  if (std::isnan(v) || std::isinf(v) || v == 0.0) return 0;
  double m = std::fmod(std::trunc(v), 4294967296.0);
  if (m < 0) m += 4294967296.0;
  return static_cast<uint32_t>(m);
}

std::string kindName(const Value& v) {
  switch (v.kind()) {
    case ValueKind::Undefined: return "undefined";
    case ValueKind::Null: return "object";
    case ValueKind::Boolean: return "boolean";
    case ValueKind::Smi:
    case ValueKind::HeapNumber: return "number";
    case ValueKind::String: return "string";
    case ValueKind::BigInt: return "bigint";
    case ValueKind::Symbol: return "symbol";
    case ValueKind::Closure: return "function";
    case ValueKind::Object:
    case ValueKind::Proxy: return "object";
    default: return "object";
  }
}

std::string doubleToString(double v) {
  if (std::isnan(v)) return "NaN";
  if (v == 0.0) return "0";  // covers -0 (ToString(-0) == "0")
  if (std::isinf(v)) return v < 0 ? "-Infinity" : "Infinity";
  std::string sign = v < 0 ? "-" : "";
  double mag = std::fabs(v);

  // v0.4 fast path (benchmarks_v0.3.md Section 5 #4): integral doubles
  // below 2^53 are exact uint64 values; emit their digits directly instead
  // of up to 17 snprintf/strtod round-trips. For such values the shortest
  // round-trip digit string is the exact integer itself (any shorter digit
  // string would denote a different real), and after the k <= n zero-fill
  // both paths render identical text (verified by the differential corpus
  // and the cross-engine bench checksums, Rule 36).
  if (mag < 9007199254740992.0) {  // 2^53
    double ipart;
    if (std::modf(mag, &ipart) == 0.0) {
      uint64_t n64 = static_cast<uint64_t>(ipart);
      char dig[24];
      int len = 0;
      do {
        dig[len++] = static_cast<char>('0' + (n64 % 10));
        n64 /= 10;
      } while (n64 != 0);
      // dig holds the digits reversed.
      int k = len;      // digit count of the magnitude
      int n = k;        // value = 0.<digits> * 10^n with n == k here
      std::string out = sign;
      if (n > 21) {
        // Exponential form: d[.rest]e+(n-1) — matches the slow path.
        out.push_back(dig[len - 1]);
        if (k > 1) {
          out.push_back('.');
          for (int i = len - 2; i >= 0; i--) out.push_back(dig[i]);
        }
        out += "e+" + std::to_string(n - 1);
      } else {
        for (int i = len - 1; i >= 0; i--) out.push_back(dig[i]);
      }
      return out;
    }
  }

  // Shortest digits: smallest precision p in 1..17 whose %.{p-1}e form
  // round-trips through strtod.
  char buf[64];
  int chosen = 17;
  for (int p = 1; p <= 17; p++) {
    std::snprintf(buf, sizeof buf, "%.*e", p - 1, mag);
    if (std::strtod(buf, nullptr) == mag) {
      chosen = p;
      break;
    }
  }
  std::snprintf(buf, sizeof buf, "%.*e", chosen - 1, mag);
  // buf looks like "d.ddddde±XX". Extract digits and exponent.
  std::string digits;
  int exp10 = 0;
  const char* p = buf;
  while (*p != '\0' && *p != 'e') {
    if (*p >= '0' && *p <= '9') digits.push_back(*p);
    p++;
  }
  if (*p == 'e') exp10 = std::atoi(p + 1);
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  int k = static_cast<int>(digits.size());
  int n = exp10 + 1;  // value = 0.<digits> * 10^n

  std::string out = sign;
  if (n > 21 || n <= -6) {
    // Exponential: d[.rest]e±(n-1), no leading zero in the exponent.
    out.push_back(digits[0]);
    if (k > 1) {
      out.push_back('.');
      out.append(digits, 1, std::string::npos);
    }
    int e = n - 1;
    out.push_back('e');
    out.push_back(e < 0 ? '-' : '+');
    int absE = e < 0 ? -e : e;
    out.append(std::to_string(absE));
  } else if (k <= n) {
    out.append(digits);
    out.append(static_cast<size_t>(n - k), '0');
  } else if (n > 0) {
    out.append(digits, 0, static_cast<size_t>(n));
    out.push_back('.');
    out.append(digits, static_cast<size_t>(n), std::string::npos);
  } else {
    out.append("0.");
    out.append(static_cast<size_t>(-n), '0');
    out.append(digits);
  }
  return out;
}

double stringToNumber(std::u16string_view s) {
  // Trim ECMAScript WhiteSpace and LineTerminators.
  auto isWs = [](char16_t c) {
    switch (c) {
      case u' ': case u'\t': case u'\n': case u'\v': case u'\f': case u'\r':
      case 0x00A0: case 0x1680: case 0x2028: case 0x2029: case 0x202F:
      case 0x205F: case 0x3000: case 0xFEFF:
        return true;
      default:
        return c >= 0x2000 && c <= 0x200A;
    }
  };
  size_t begin = 0;
  size_t end = s.size();
  while (begin < end && isWs(s[begin])) begin++;
  while (end > begin && isWs(s[end - 1])) end--;
  s = s.substr(begin, end - begin);
  if (s.empty()) return 0.0;

  // Convert to narrow ASCII for scanning (grammar is ASCII-only).
  std::string ascii;
  ascii.reserve(s.size());
  for (char16_t c : s) {
    if (c > 0x7F) return std::numeric_limits<double>::quiet_NaN();
    ascii.push_back(static_cast<char>(c));
  }

  auto allDigits = [](const std::string& t, int base) {
    if (t.empty()) return false;
    for (char c : t) {
      int d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
      else return false;
      if (d >= base) return false;
    }
    return true;
  };

  // Radix forms: unsigned only, no sign, no dot, no exponent.
  if (ascii.size() > 2 && ascii[0] == '0' &&
      (ascii[1] == 'x' || ascii[1] == 'X' || ascii[1] == 'o' || ascii[1] == 'O' ||
       ascii[1] == 'b' || ascii[1] == 'B')) {
    char baseCh = static_cast<char>(std::tolower(ascii[1]));
    int base = baseCh == 'x' ? 16 : baseCh == 'o' ? 8 : 2;
    std::string digitsPart = ascii.substr(2);
    if (!allDigits(digitsPart, base)) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    uint64_t acc = 0;
    for (char c : digitsPart) {
      int d = c <= '9' ? c - '0'
                       : (std::tolower(c) - 'a' + 10);
      acc = acc * base + d;  // 64-bit: valid inputs here are <= 2^53-ish
    }
    return static_cast<double>(acc);
  }

  if (ascii == "Infinity" || ascii == "+Infinity") {
    return std::numeric_limits<double>::infinity();
  }
  if (ascii == "-Infinity") return -std::numeric_limits<double>::infinity();

  // Decimal grammar: [+-]? ( Digits [. Digits?] | . Digits ) ([eE][+-]?Digits)?
  size_t i = 0;
  if (i < ascii.size() && (ascii[i] == '+' || ascii[i] == '-')) i++;
  size_t intStart = i;
  while (i < ascii.size() && ascii[i] >= '0' && ascii[i] <= '9') i++;
  bool hasIntDigits = i > intStart;
  bool hasDot = false;
  size_t fracStart = i;
  if (i < ascii.size() && ascii[i] == '.') {
    hasDot = true;
    i++;
    fracStart = i;
    while (i < ascii.size() && ascii[i] >= '0' && ascii[i] <= '9') i++;
  }
  if (!hasIntDigits && fracStart == i && (!hasDot || fracStart == intStart + 1)) {
    // No digits at all (".", "+.", "e5", ...)
    if (!hasIntDigits && fracStart == intStart) {
      return std::numeric_limits<double>::quiet_NaN();
    }
  }
  if (!hasIntDigits && fracStart == intStart + 1 && hasDot) {
    return std::numeric_limits<double>::quiet_NaN();  // bare "."
  }
  size_t numEnd = i;
  if (i < ascii.size() && (ascii[i] == 'e' || ascii[i] == 'E')) {
    i++;
    if (i < ascii.size() && (ascii[i] == '+' || ascii[i] == '-')) i++;
    size_t expStart = i;
    while (i < ascii.size() && ascii[i] >= '0' && ascii[i] <= '9') i++;
    if (i == expStart) return std::numeric_limits<double>::quiet_NaN();
  }
  if (i != ascii.size()) return std::numeric_limits<double>::quiet_NaN();
  if (!hasIntDigits && numEnd == fracStart) {
    return std::numeric_limits<double>::quiet_NaN();  // "." or "+."
  }

  return std::strtod(ascii.c_str(), nullptr);
}

bool strictEquals(const Value& a, const Value& b) {
  if (a.kind() != b.kind()) {
    // Number equality spans Smi/HeapNumber tags.
    if (a.isNumber() && b.isNumber()) {
      double x = a.asDouble();
      double y = b.asDouble();
      return x == y;
    }
    return false;
  }
  switch (a.kind()) {
    case ValueKind::Undefined:
    case ValueKind::Null:
      return true;
    case ValueKind::Boolean:
      return a.asBool() == b.asBool();
    case ValueKind::Smi:
    case ValueKind::HeapNumber:
      return a.asDouble() == b.asDouble();
    case ValueKind::String: {
      const StringObj* x = a.asString();
      const StringObj* y = b.asString();
      // v0.6: header-length early-out first (cons nodes flatten only when
      // their lengths actually match).
      if (x->length != y->length) return false;
      return x->flat() == y->flat();
    }
    case ValueKind::BigInt: {
      const BigInt* x = a.asBigInt();
      const BigInt* y = b.asBigInt();
      return BigInt::compare(*x, *y) == 0;
    }
    case ValueKind::Symbol:
    case ValueKind::Object:
    case ValueKind::Closure:
    case ValueKind::Proxy:
      return a.asPtr() == b.asPtr();
    default:
      return false;
  }
}

bool sameValue(const Value& a, const Value& b) {
  if (a.isNumber() && b.isNumber()) {
    double x = a.asDouble();
    double y = b.asDouble();
    if (std::isnan(x) && std::isnan(y)) return true;
    if (x == 0.0 && y == 0.0) return std::signbit(x) == std::signbit(y);
    return x == y;
  }
  if (a.kind() != b.kind()) return false;
  if (a.isString() && b.isString()) return strictEquals(a, b);
  if (a.isBigInt() && b.isBigInt()) return strictEquals(a, b);
  if (a.isString() || a.isBigInt()) return false;
  return strictEquals(a, b);
}

bool sameValueZero(const Value& a, const Value& b) {
  if (a.isNumber() && b.isNumber()) {
    double x = a.asDouble();
    double y = b.asDouble();
    if (std::isnan(x) && std::isnan(y)) return true;
    return x == y;  // +0 == -0
  }
  return sameValue(a, b);
}

int compareStrings(const StringObj& a, const StringObj& b) {
  // v0.6: materialize both operands (in place; repeated comparisons of the
  // same nodes stay O(1) after the first). Code-unit-wise order unchanged.
  const std::u16string& x = a.flat();
  const std::u16string& y = b.flat();
  size_t n = std::min(x.size(), y.size());
  for (size_t i = 0; i < n; i++) {
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  }
  if (x.size() == y.size()) return 0;
  return x.size() < y.size() ? -1 : 1;
}

bool doubleIsIntegral(double v) {
  return !std::isnan(v) && !std::isinf(v) && v == std::trunc(v);
}

}  // namespace pure

}  // namespace ts
