#pragma once
// B-2 codegen - T2 IR-to-x86-64 lowering: the optimizing tier's execution path.
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include "b2/codegen/Instantiate.h"
#include "b2/codegen/Tier1.h"

namespace b2::ir { class Graph; }
namespace b2::rbc { struct Method; struct Program; }

namespace b2::codegen {

struct T2LoweringConfig { bool run_pipeline = false; };

[[nodiscard]] Tier1RunResult lowerAndExecute(
    ir::Graph& g, const rbc::Program& program, const rbc::Method& method,
    std::uint32_t methodId, Tier1& engine, std::span<const interp::Value> args,
    const T2LoweringConfig& config = {});

[[nodiscard]] std::unique_ptr<CompiledCode> lowerOnly(
    const ir::Graph& g, const rbc::Program& program, const rbc::Method& method,
    std::uint32_t methodId, interp::Runtime& rt, std::string* refusalReason = nullptr);

} // namespace b2::codegen
