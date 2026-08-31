#pragma once
// B-2 IR - versioned serialization and replay (Rules 31, 38, 124).
//
// WHY THIS FILE EXISTS:
// Rule 124 requires deterministic, replayable compilation; Rule 38 replay
// artifacts need a versioned on-disk form; the charter promises node ids are
// preserved by serialization. The format is little-endian, fixed-layout, and
// self-describing enough to reject stale versions and truncated payloads
// WITHOUT trusting any field (total parser: bad input -> error result,
// never UB). Loading replays node creation in id order (two-pass over edges
// so forward references work), then the side tables, then the replacement
// log; the result is verified before use.

#include <cstdint>
#include <string>
#include <vector>

#include "b2/ir/Graph.h"

namespace b2::ir {

inline constexpr std::uint32_t kIrMagic = 0x52493242u; // 'B2IR' LE
inline constexpr std::uint32_t kIrFormatVersion = 1;

struct SerializeError {
  std::string message;
};

// Serialize the graph. Byte-deterministic for the same graph state (Rule
// 124): nodes in id order, side tables in id order, replacement log in log
// order. Dead tombstone nodes ARE serialized (id preservation).
[[nodiscard]] std::vector<std::uint8_t> serialize(const Graph& g);

struct DeserializeResult {
  bool ok = false;
  SerializeError error; // message when !ok
};

// Parse + rebuild + verify INTO `out` (which must be a freshly-constructed
// Graph: replay patches node 0 = Start in place and appends every other
// node in id order). Rejects: wrong magic, unsupported version (Rule 31),
// truncated buffers, out-of-range ids/kinds, malformed side tables, and
// graphs that parse but fail verification (the load gate, Rule 40).
// Returns ok=false without touching anything the caller still needs; on
// failure `out` may be partially built and must be discarded.
[[nodiscard]] DeserializeResult deserializeInto(const std::uint8_t* data,
                                                 std::size_t size,
                                                 Graph& out) noexcept;

[[nodiscard]] inline DeserializeResult
deserializeInto(const std::vector<std::uint8_t>& bytes, Graph& out) noexcept {
  return deserializeInto(bytes.data(), bytes.size(), out);
}

} // namespace b2::ir
