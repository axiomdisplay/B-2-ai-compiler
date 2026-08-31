#pragma once
// B-2 IR - deterministic textual printer (Rule 124).
//
// WHY THIS FILE EXISTS:
// Golden tests and bug reports need a diff-stable dump: the same graph (same
// construction sequence) must print byte-identically across runs, machines,
// and serialization round-trips. The format is normative in docs/ir_spec.md
// appendix A - changing it is a versioned format change (Rule 31) because
// golden fixtures pin it. Strings appear ONLY here (debug tooling), never in
// the IR itself (Rule 16): identifiers print as their interned ids.

#include <cstdint>
#include <string>

#include "b2/ir/Graph.h"

namespace b2::ir {

// Prints the graph: header, nodes in id order (inputs, payload suffixes,
// attached metadata), side tables (virtual objects, SpecMeta, dependencies),
// and the replacement log. Deterministic by construction.
[[nodiscard]] std::string print(const Graph& g);

// Result type of a node's value, computed from the node + graph (Phi joins
// its value inputs; Parameter/Call/Load* read payloads; vectors derive from
// lane payload). Shared by the printer and the verifier.
[[nodiscard]] IRType resultTypeOf(const Graph& g, NodeId n);

} // namespace b2::ir
