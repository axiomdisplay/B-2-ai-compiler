#pragma once
// B-2 IR - the sea-of-nodes Graph: storage, edges, use-def chains, epochs,
// and the deopt/PEA/speculation metadata side structures.
//
// WHY THIS FILE EXISTS:
// This is the T2/T3 core representation (Amendment B.1). Design is fixed by
// law: arena allocation with bulk free (Rule 7), NodeId-only edges (Rule 15),
// no strings (Rule 16), SmallVector use lists (Rule 19), epoch-tagged
// replacement (Rule 14), deterministic ids (Rule 124). The metadata side
// structures carry what Rules 5, 42, 122, and Part XVIII require the IR to
// REPRESENT (not optimize): FrameState deopt snapshots, PEA virtual objects
// and materialization plans, speculative metadata, and invalidation
// dependencies for the watchdog.
//
// Storage model:
//   - nodes_    : dense NodeId -> Node (ids are creation order, stable for
//                 the graph's lifetime; replaced nodes stay as tombstones).
//   - edgePool_ : one flat pool; node inputs are the slice
//                 [edgeOffset, edgeOffset + numInputs). appendInput may
//                 relocate a slice to the pool tail (old slice abandoned;
//                 the pool is arena memory).
//   - uses_     : per-node SmallVector<Use, 3> def-use lists (Rule 19).
//   - side tables (FrameState descs, virtual objects, SpecMeta, Dependency)
//                 are referenced by id from node payloads / records.

#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <vector>

#include "b2/ir/Node.h"
#include "b2/ir/SmallVector.h"
#include "b2/ir/Types.h"

namespace b2::ir {

// A def-use entry: node `user` reads the defining node at input `slot`.
struct Use {
  NodeId user = kInvalidNodeId;
  std::uint16_t slot = 0;

  [[nodiscard]] constexpr bool operator==(const Use& o) const noexcept {
    return user == o.user && slot == o.slot;
  }
};

using FrameStateId = std::uint32_t;
inline constexpr FrameStateId kInvalidFrameState = 0xFFFFFFFFu;

// FrameState descriptor (Rule 5). The FrameState NODE carries the local
// values as input edges (so replacement automatically updates snapshots);
// this record carries everything that is not an edge.
struct FrameStateDesc {
  MethodId method = 0;      // opaque frontend method id
  std::uint32_t pc = 0;     // RBC pc to resume at (deopt re-entry)
  std::uint32_t caller = kInvalidFrameState; // inlined caller's FrameStateId
  std::uint32_t vobjOffset = 0; // slice [offset, offset+count) of fsVobjs_
  std::uint32_t vobjCount = 0;  // virtual objects to materialize at deopt
};

// PEA virtual object identity: the NodeId of the VirtualObjectState node
// (fields are input edges of that node, so replacement propagates). The
// field layout is documented with NodeKind::VirtualObjectState in Node.h.
using VirtualObjectId = NodeId;
inline constexpr VirtualObjectId kInvalidVirtualObject = kInvalidNodeId;

using DependencyId = std::uint32_t;
inline constexpr DependencyId kInvalidDependency = 0xFFFFFFFFu;

// Speculative metadata (Rule 122: the complete field set is mandatory).
struct SpecMeta {
  enum class Kind : std::uint8_t {
    ClassHierarchyStable = 0, // no subclass loading will invalidate
    MethodFinal,              // callee cannot be overridden
    TypeMonomorphic,          // single observed receiver type
    TypeBimorphic,            // two observed receiver types
    NullNeverSeen,            // receiver/array never null at this site
    BoundsAlwaysValid,        // profiled indices always in range
    ArgumentConstant,         // argument is effectively constant
    LoopTripCountProfile,     // profiled trip count bound
    StaticProofCarried,       // T3-style proof attached (Rule 131)
  };
  enum class Source : std::uint8_t { PGO = 0, Static, Assumption };
  enum class Rollback : std::uint8_t {
    None = 0,
    DeferredEffects, // effects deferred until speculation commits
    Compensated,     // explicit compensation nodes restore state on deopt
  };

  Kind kind = Kind::TypeMonomorphic;
  Source source = Source::PGO;
  std::uint32_t confidence = 0; // 0..10000 basis points (Rule 44 fields live
                                // in the profile store; this is the snapshot)
  NodeId guard = kInvalidNodeId; // guard node enforcing it (invalid =>
                                 // source must be Static)
  std::uint32_t deoptTarget = 0; // DeoptId (plan/reason)
  std::uint32_t cost = 0;        // cost-model units (Rule 45)
  std::uint32_t dependency = kInvalidDependency; // watchdog entry (Rule 42)
  Rollback rollback = Rollback::None;
};

using SpecMetaId = std::uint32_t;
inline constexpr SpecMetaId kInvalidSpecMeta = 0xFFFFFFFFu;

// Invalidation dependency (Rule 42: no assumption without invalidation).
struct Dependency {
  enum class Kind : std::uint8_t {
    ClassHierarchy = 0, // target = TypeId
    MethodBody,         // target = MethodId (redefinition)
    FieldFinality,      // target = FieldId
    ProfileCounter,     // target = profile site id
    StaticProof,        // target = proof artifact id (Rule 131)
  };
  Kind kind = Kind::ClassHierarchy;
  std::uint32_t target = 0;
};

// Replacement log entry (Rule 14): `old` was replaced by `with` at `epoch`.
struct Replacement {
  NodeId oldNode = kInvalidNodeId;
  NodeId newNode = kInvalidNodeId;
  std::uint32_t epoch = 0;
};

class Graph {
public:
  Graph();
  ~Graph();

  Graph(const Graph&) = delete;
  Graph& operator=(const Graph&) = delete;

  // --- node creation -------------------------------------------------------
  // `make` is the trusted fast path (builder discipline): it does NOT verify
  // input validity or role/type conformance - that is the verifier's job
  // (Rule 40), exactly like RbcBuilder vs the RBC verifier. Out-of-range
  // input ids are stored as-is; the verifier rejects them.
  NodeId make(NodeKind kind, std::span<const NodeId> inputs = {},
              std::uint32_t payload = 0, std::uint32_t payload2 = 0);
  NodeId make(NodeKind kind, std::initializer_list<NodeId> inputs,
              std::uint32_t payload = 0, std::uint32_t payload2 = 0);

  // Typed convenience makers (the common builder vocabulary).
  NodeId constantI(std::int32_t v);
  NodeId constantL(std::int64_t v);
  NodeId constantF(float v);   // bits preserved exactly
  NodeId constantD(double v);  // bits preserved exactly, incl. NaN payloads
  NodeId constantNull();
  NodeId constantSym(SymbolId sym);
  NodeId parameter(std::uint16_t index, IRType type);

  NodeId makeFrameState(MethodId method, std::uint32_t pc,
                        std::span<const NodeId> locals,
                        FrameStateId caller = kInvalidFrameState,
                        std::span<const VirtualObjectId> vobjs = {});
  NodeId makeFrameState(MethodId method, std::uint32_t pc,
                        std::initializer_list<NodeId> locals,
                        FrameStateId caller = kInvalidFrameState,
                        std::initializer_list<VirtualObjectId> vobjs = {});

  // PEA virtual objects (Part XVIII). Returns the VirtualObjectState node id
  // (the VirtualObjectId). Instance fields / array elements are the input
  // edges; the array's length is the FIRST input. Field values are NodeIds
  // (the current virtual state).
  NodeId makeVirtualObject(TypeId classId,
                           std::span<const NodeId> fieldValues);
  NodeId makeVirtualObject(TypeId classId,
                           std::initializer_list<NodeId> fieldValues);
  NodeId makeArrayVirtualObject(IRType elemType, NodeId length,
                                std::span<const NodeId> elements);
  NodeId makeArrayVirtualObject(IRType elemType, NodeId length,
                                std::initializer_list<NodeId> elements);

  // Speculation metadata (Rule 122) and watchdog dependencies (Rule 42).
  SpecMetaId addSpecMeta(const SpecMeta& sm);
  DependencyId addDependency(const Dependency& dep);

  // Attach SpecMeta to an existing node (sets the Speculative flag too).
  void attachSpecMeta(NodeId n, SpecMetaId id);

  // --- inputs / uses ----------------------------------------------------------
  [[nodiscard]] std::uint16_t numInputs(NodeId n) const;
  [[nodiscard]] NodeId input(NodeId n, std::uint16_t slot) const;
  void setInput(NodeId n, std::uint16_t slot, NodeId v); // updates use lists
  void appendInput(NodeId n, NodeId v); // grows Region/Phi/Call/FrameState

  [[nodiscard]] const SmallVector<Use, 3>& usesOf(NodeId n) const;

  // --- replacement (Rule 14) ----------------------------------------------------
  // Rewires every user of `old` to `with`, tags old Dead|Replaced with the
  // current epoch, logs the replacement, and returns the new epoch.
  // Replacing with a dead node is refused (returns 0, no change).
  std::uint32_t replaceNode(NodeId oldNode, NodeId withNode);

  // Kill a node without a replacement (e.g. dead-code elimination): users
  // are NOT rewired; the pass must have removed all uses first (the
  // verifier rejects dangling inputs, so a kill with live users is a bug).
  void killNode(NodeId n);

  [[nodiscard]] std::uint32_t epoch() const noexcept { return epoch_; }
  void nextEpoch() noexcept { ++epoch_; }

  // --- accessors -------------------------------------------------------------------
  [[nodiscard]] std::uint32_t nodeCount() const noexcept {
    return static_cast<std::uint32_t>(nodes_.size());
  }
  [[nodiscard]] NodeId startNode() const noexcept { return start_; }
  [[nodiscard]] const Node& node(NodeId n) const noexcept; // OOB -> badNode()
  // Replay-only mutable access (Serialize.cpp patch path); all normal
  // mutation goes through make/setInput/appendInput/replaceNode.
  [[nodiscard]] Node& nodeForReplay(NodeId n) noexcept;

  [[nodiscard]] const FrameStateDesc& frameState(FrameStateId id) const noexcept;
  [[nodiscard]] const SpecMeta& specMeta(SpecMetaId id) const noexcept;
  [[nodiscard]] const Dependency& dependency(DependencyId id) const noexcept;

  [[nodiscard]] std::uint32_t frameStateCount() const noexcept {
    return static_cast<std::uint32_t>(fsDesc_.size());
  }
  [[nodiscard]] std::uint32_t specMetaCount() const noexcept {
    return static_cast<std::uint32_t>(specMetas_.size());
  }
  [[nodiscard]] std::uint32_t dependencyCount() const noexcept {
    return static_cast<std::uint32_t>(deps_.size());
  }

  // FrameState vobj list (slice accessor). Entries are VirtualObjectState
  // node ids.
  [[nodiscard]] std::span<const VirtualObjectId>
  frameStateVobjs(FrameStateId id) const noexcept;

  [[nodiscard]] const std::vector<Replacement>& replacements() const noexcept {
    return replacements_;
  }

  // Edge pool size (diagnostics/telemetry only).
  [[nodiscard]] std::size_t edgePoolSize() const noexcept {
    return edgePool_.size();
  }

  // Total live (non-dead) nodes.
  [[nodiscard]] std::uint32_t liveNodeCount() const noexcept;

private:
  NodeId makeRaw(NodeKind kind, std::span<const NodeId> inputs,
                 std::uint32_t payload, std::uint32_t payload2,
                 std::int64_t constValue = 0);
  void removeFromUses(NodeId def, NodeId user, std::uint16_t slot);
  FrameStateId addFrameStateDesc(MethodId method, std::uint32_t pc,
                                 FrameStateId caller,
                                 std::span<const VirtualObjectId> vobjs);

public:
  // --- replay support (deserialization ONLY; Serialize.cpp) ------------------
  // Appends one FrameState descriptor (rebuilds fsVobjs_ in order).
  FrameStateId replayFrameStateDesc(MethodId method, std::uint32_t pc,
                                    FrameStateId caller,
                                    std::span<const VirtualObjectId> vobjs);
  // Re-tags a serialized replacement WITHOUT rewiring (the serialized node
  // state is already post-rewire); appends the log entry.
  void replayReplacement(NodeId oldNode, NodeId newNode, std::uint32_t epoch);

private:

  // Arena first (members below allocate from it). Monotonic: bulk-freed on
  // destruction (Rule 7). The graph's memory lifetime IS the compile job's.
  std::pmr::monotonic_buffer_resource arena_;

  std::pmr::vector<Node> nodes_{&arena_};
  std::pmr::vector<NodeId> edgePool_{&arena_};
  std::pmr::vector<SmallVector<Use, 3>> uses_{&arena_};

  std::pmr::vector<FrameStateDesc> fsDesc_{&arena_};
  std::pmr::vector<VirtualObjectId> fsVobjs_{&arena_};
  std::pmr::vector<SpecMeta> specMetas_{&arena_};
  std::pmr::vector<Dependency> deps_{&arena_};

  std::vector<Replacement> replacements_; // diagnostics path: plain vector
  std::uint32_t epoch_ = 0;
  NodeId start_ = kInvalidNodeId;
};

} // namespace b2::ir
