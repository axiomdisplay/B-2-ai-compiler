// b2graph - the RBC->IR graph builder's command-line surface.
//
// WHY THIS TOOL EXISTS:
// Every module ships one debug tool (b2parse, b2run, b2bench, b2jit); the
// builder's is b2graph: parse an .rbc text program, verify the RBC (the
// tier-input law), build every method's sea-of-nodes graph, run the IR
// verifier over it, and print it. This is the human review surface for
// lowering decisions (docs/graph_builder.md) and the smoke test the T2
// pipeline will grow from.
//
// --pgo (ICDG Phase 2, docs/inlining.md section 9): run the program in
// T0 FIRST (the training run - the same entry inference as b2run;
// Returned and Threw both keep the accumulated profile), snapshot the
// per-site dispatch histogram through the shared converter
// (tools/DispatchProfileSnapshot.h - this tool is an interp-linking
// integrator, the passes library is not), and feed it to the inliner:
// monomorphic virtual/interface sites then GUARD-INLINE behind a
// TypeProfile guard; every other site keeps its v1/v2 decision path.
// The training run's program output (stdout/stderr side effects) is
// NOT part of this tool's output surface - the decision log is.
//
// --pea (CM-PEA, docs/special_passes.md section 1): run the partial
// escape analysis engine after the build (and after --inline, which is
// the cross-method half: the post-inline graph is where the merged
// data flow lives) and print the per-allocation decision log - the
// classification, the action (SCALARIZED / MATERIALIZED / REJECTED),
// the refusal reason, and the profile-style numbers. The engine is
// deterministic and fail-closed: the review surface never shows an
// unverified graph. With -O the pipeline's own PEA stage (keys
// 65/66/67/69) runs as usual - it is idempotent, so a --pea-processed
// graph flows through with zero rewrites.
//
// Exit codes: 0 = all methods built and verified; 1 = any parse/verify/
// build/IR-verify failure (diagnostics on stderr).

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "DispatchProfileSnapshot.h"
#include "b2/codegen/T2Lowering.h"
#include "b2/codegen/Tier1.h"
#include "b2/interp/Interp.h"
#include "b2/ir/Printer.h"
#include "b2/ir/Verifier.h"
#include "b2/passes/GraphBuilder.h"
#include "b2/passes/Inline.h"
#include "b2/passes/Passes.h"
#include "b2/rbc/RbcText.h"
#include "b2/rbc/Verifier.h"

namespace {

int run(const b2::rbc::Program& prog, bool quiet, bool optimize, bool inl,
        bool pea, const b2::passes::DispatchProfile* profile) {
  int failures = 0;
  for (std::size_t i = 0; i < prog.methods.size(); ++i) {
    const b2::rbc::Method& m = prog.methods[i];
    if (!quiet) {
      std::printf("=== method %zu %s%s\n", i, m.name.c_str(),
                  m.descriptor.c_str());
    }
    const b2::rbc::VerifyResult vr = b2::rbc::verify(m);
    if (vr.hasErrors()) {
      std::fprintf(stderr, "b2graph: RBC verification failed for %s:\n",
                   m.name.c_str());
      for (const b2::rbc::VerifyDiag& d : vr.diags) {
        std::fprintf(stderr, "  pc %u: %s\n", d.pc, d.message.c_str());
      }
      ++failures;
      continue;
    }
    // The unified method-id space: the source doubles as the build
    // resolver (program methods = table indices, externals above the
    // table), so the build's call payloads and the inline resolution
    // agree by construction (docs/inlining.md section 3).
    b2::passes::ProgramCalleeSource resolver(prog);
    b2::ir::Graph g;
    const b2::passes::BuildResult br = b2::passes::buildGraph(
        m, resolver, g, static_cast<b2::ir::MethodId>(i));
    if (br.hasErrors()) {
      std::fprintf(stderr, "b2graph: build failed for %s:\n", m.name.c_str());
      for (const b2::passes::BuildDiag& d : br.diags) {
        std::fprintf(stderr, "  pc %u: %s\n", d.pc, d.message.c_str());
      }
      ++failures;
      continue;
    }
    const b2::ir::VerifyResult irv = b2::ir::verify(g);
    if (irv.hasErrors()) {
      std::fprintf(stderr, "b2graph: IR verification failed for %s:\n",
                   m.name.c_str());
      for (const b2::ir::VerifyDiag& d : irv.diags) {
        std::fprintf(stderr, "  n%u: %s\n", d.node, d.message.c_str());
      }
      ++failures;
      continue;
    }
    if (inl) {
      // The ICDG inline engine (docs/inlining.md): the same resolver
      // built the graph (the unified id space), the verifier runs after
      // every site, decisions + telemetry per method. With --pgo the
      // T0 training snapshot rides in the config (guard-inline on
      // monomorphic virtual/interface sites). Fail-closed on any
      // failure - the review surface never shows an unverified graph.
      b2::passes::InlineConfig icfg;
      icfg.profile = profile;
      const b2::passes::InlineResult ir =
          b2::passes::runInlining(g, resolver, icfg);
      if (!ir.ok) {
        std::fprintf(stderr, "b2graph: inlining failed for %s:\n",
                     m.name.c_str());
        for (const b2::passes::InlineDiag& d : ir.diags) {
          std::fprintf(stderr, "  n%u: %s\n", d.node, d.message.c_str());
        }
        ++failures;
        continue;
      }
      if (!quiet) {
        std::printf("  # inline: sites=%u inlined=%u guard=%u refused=%u "
                    "nodes=+%u deopts=+%u merges=%u depth=%u converged=%d\n",
                    ir.telemetry.sitesConsidered, ir.telemetry.sitesInlined,
                    ir.telemetry.sitesGuardInlined, ir.telemetry.sitesRefused,
                    ir.telemetry.nodesAdded, ir.telemetry.deoptsEmitted,
                    ir.telemetry.exitMerges, ir.telemetry.maxDepthReached,
                    ir.telemetry.converged ? 1 : 0);
        for (const b2::passes::InlineDecision& d : ir.decisions) {
          const char* act =
              d.action == b2::passes::InlineAction::DirectInline
                  ? "DIRECT-INLINE"
                  : d.action == b2::passes::InlineAction::GuardInline
                      ? "GUARD-INLINE"
                      : "KEEP-INDIRECT";
          if (d.action == b2::passes::InlineAction::GuardInline) {
            // The profile numbers ride the guard decision (icdg.md 19:
            // the confidence is part of the explanation).
            std::printf("  # inline n%u -> m%u d%u: %s (%s; insns=%u "
                        "slots=%u nodes=%u profile=%u/%u)\n",
                        d.call, d.target, d.depth, act, d.reason,
                        d.calleeInsns, d.calleeSlots, d.calleeNodes,
                        d.recvCount, d.siteCount);
          } else {
            std::printf("  # inline n%u -> m%u d%u: %s (%s; insns=%u "
                        "slots=%u nodes=%u)\n",
                        d.call, d.target, d.depth, act, d.reason,
                        d.calleeInsns, d.calleeSlots, d.calleeNodes);
          }
        }
      }
    }
    if (pea) {
      // CM-PEA decision log (docs/special_passes.md section 1): the
      // per-allocation classification + disposition + reason, with the
      // field/forwarding counts. Fail-closed like every other surface.
      const b2::passes::PeaResult pr =
          b2::passes::runPartialEscapeAnalysis(g);
      if (!pr.ok) {
        std::fprintf(stderr, "b2graph: PEA failed for %s:\n",
                     m.name.c_str());
        for (const b2::passes::PassDiag& d : pr.diags) {
          std::fprintf(stderr, "  %s\n", d.message.c_str());
        }
        ++failures;
        continue;
      }
      if (!quiet) {
        std::printf("  # pea: scalarized=%u materialized=%u rejected=%u\n",
                    pr.telemetry.peaScalarized,
                    pr.telemetry.peaMaterialized,
                    pr.telemetry.peaRejected);
        for (const b2::passes::PeaDecision& d : pr.decisions) {
          const char* kindName =
              d.kind == b2::ir::NodeKind::New
                  ? "New"
                  : d.kind == b2::ir::NodeKind::NewArray ? "NewArray"
                                                         : "NewRefArray";
          if (d.materializeAt != b2::ir::kInvalidNodeId) {
            std::printf("  # pea n%u (%s): %s (%s; %s; fields=%u loads=%u "
                        "stores=%u at n%u)\n",
                        d.alloc, kindName, d.action, d.reason,
                        b2::passes::escapeStateName(d.state), d.fields,
                        d.loads, d.stores, d.materializeAt);
          } else {
            std::printf("  # pea n%u (%s): %s (%s; %s; fields=%u loads=%u "
                        "stores=%u)\n",
                        d.alloc, kindName, d.action, d.reason,
                        b2::passes::escapeStateName(d.state), d.fields,
                        d.loads, d.stores);
          }
        }
      }
    }
    if (optimize) {
      // The early-cleanup + GVN pipeline (Rules 40/124: verified between
      // passes, deterministic). Fail-closed on any verification or
      // budget failure - the human review surface never shows an
      // unverified graph.
      const b2::passes::PassResult pr = b2::passes::runEarlyCleanup(g);
      if (!pr.ok) {
        std::fprintf(stderr, "b2graph: pass pipeline failed for %s:\n",
                     m.name.c_str());
        for (const b2::passes::PassDiag& d : pr.diags) {
          std::fprintf(stderr, "  %s\n", d.message.c_str());
        }
        ++failures;
        continue;
      }
      const b2::ir::VerifyResult irv2 = b2::ir::verify(g);
      if (irv2.hasErrors()) {
        std::fprintf(stderr,
                     "b2graph: post-pipeline verification failed for %s\n",
                     m.name.c_str());
        ++failures;
        continue;
      }
      if (!quiet) {
        std::printf("  # pipeline: rounds=%u rewrites=%u removals=%u "
                    "folds=%u gvn=%u sccp=%u pea=%u/%u/%u converged=%d\n",
                    pr.telemetry.rounds, pr.telemetry.rewrites,
                    pr.telemetry.removals, pr.telemetry.folds,
                    pr.telemetry.gvnDedups, pr.telemetry.sccpConstants,
                    pr.telemetry.peaScalarized,
                    pr.telemetry.peaMaterialized,
                    pr.telemetry.peaRejected,
                    pr.telemetry.converged ? 1 : 0);
      }
    }
    if (!quiet) {
      std::fputs(b2::ir::print(g).c_str(), stdout);
    }
  }
  return failures == 0 ? 0 : 1;
}

// --- --exec: T2 lowering + execution -------------------------------------------
[[nodiscard]] int execProgram(const b2::rbc::Program& prog,
                              bool quiet, bool optimize, bool inl, bool pea,
                              const b2::passes::DispatchProfile* profile) {
  b2::codegen::Tier1 engine(prog, b2::codegen::Tier1Config{});
  std::uint32_t lowered = 0, refused = 0;
  for (std::size_t i = 0; i < prog.methods.size(); ++i) {
    const b2::rbc::Method& m = prog.methods[i];
    const b2::rbc::VerifyResult vr = b2::rbc::verify(m);
    if (vr.hasErrors()) { std::fprintf(stderr,"  [debug] rbc verify failed for %s\n",m.name.c_str()); ++refused; continue; }
    b2::passes::ProgramCalleeSource resolver(prog);
    b2::ir::Graph g;
    const b2::passes::BuildResult br = b2::passes::buildGraph(
        m, resolver, g, static_cast<b2::ir::MethodId>(i));
    if (br.hasErrors()) { std::fprintf(stderr,"  [debug] buildGraph failed for %s\n",m.name.c_str()); ++refused; continue; }
    if (inl) { b2::passes::InlineConfig icfg; icfg.profile = profile;
      (void)b2::passes::runInlining(g, resolver, icfg); }
    if (pea) { (void)b2::passes::runPartialEscapeAnalysis(g); }
    if (optimize) { (void)b2::passes::runEarlyCleanup(g); }
    const b2::ir::VerifyResult irv = b2::ir::verify(g);
    if (irv.hasErrors()) { std::fprintf(stderr,"  [debug] IR verify failed for %s: %s\n",m.name.c_str(), irv.diags.empty()?"":irv.diags[0].message.c_str()); ++refused; continue; }
    std::string refusalReason;
    auto cc = b2::codegen::lowerOnly(g, m, static_cast<std::uint32_t>(i),
                                     engine.interp().runtime(), &refusalReason);
    if (!cc) { std::fprintf(stderr,"  [debug] lowerOnly refused for %s: %s\n",m.name.c_str(), refusalReason.c_str()); ++refused; continue; }
    engine.installCompiledCode(std::move(cc));
    ++lowered;
  }
  // Find + execute entry.
  std::string entryDesc;
  if (prog.find("main", "()V")) entryDesc = "()V";
  else if (prog.find("main", "([Ljava/lang/String;)V")) entryDesc = "([Ljava/lang/String;)V";
  else { for (const b2::rbc::Method& m : prog.methods)
    if (m.name=="main" && b2::rbc::paramCount(m.descriptor)==0) { entryDesc=m.descriptor; break; } }
  if (entryDesc.empty()) { std::fprintf(stderr,"b2graph --exec: no main()V\n"); return 1; }
  std::vector<b2::interp::Value> args;
  if (entryDesc == "([Ljava/lang/String;)V")
    args.push_back(b2::interp::Value::refVal(
        engine.interp().runtime().newRefArray(engine.interp().runtime().stringClass(), 0)));
  const b2::codegen::Tier1RunResult r = engine.run("main", entryDesc, args);
  // WHY: println writes to rt.stdout() (an internal string buffer, not real
  // stdout). b2run flushes it after the run; we must too (b2graph --exec
  // mirrors b2run's output discipline).
  std::fwrite(engine.interp().runtime().stdout().data(), 1,
              engine.interp().runtime().stdout().size(), stdout);
  std::fflush(stdout);
  if (!quiet) std::fprintf(stderr, "[b2graph --exec] lowered=%u refused=%u status=%d\n",
                          lowered, refused, static_cast<int>(r.status));
  return r.status == b2::codegen::Tier1Status::Returned ? 0 : 1;
}

} // namespace

// The T0 training run (the --pgo half; ICDG Phase 2): execute the
// program once with b2run-style entry inference (main; ()V, then the
// String[] form, then any zero-parameter main - corpus programs are all
// ()V, and hand-written graphs use ()I-style entries), then snapshot
// the accumulated dispatch profile. Returned AND Threw both keep the
// data (a partially-run program still profiled every site it
// dispatched); a program with no runnable entry keeps an empty profile
// (every site then refuses "no row" - no data, no speculation). The
// program's stdout/stderr side effects stay in the runtime buffers
// (the tool's output surface is the decision log, not the training
// run's output).
[[nodiscard]] const b2::passes::DispatchProfile*
trainAndSnapshot(const b2::rbc::Program& prog,
                 b2::passes::DispatchProfile& storage) {
  b2::interp::Interpreter interp(prog, b2::interp::InterpConfig{});
  std::string entryDesc;
  if (prog.find("main", "()V") != nullptr) {
    entryDesc = "()V";
  } else if (prog.find("main", "([Ljava/lang/String;)V") != nullptr) {
    entryDesc = "([Ljava/lang/String;)V";
  } else {
    for (const b2::rbc::Method& m : prog.methods) {
      if (m.name == "main" && b2::rbc::paramCount(m.descriptor) == 0) {
        entryDesc = m.descriptor;
        break;
      }
    }
  }
  std::vector<b2::interp::Value> args;
  if (entryDesc == "([Ljava/lang/String;)V") {
    args.push_back(b2::interp::Value::refVal(
        interp.runtime().newRefArray(interp.runtime().stringClass(), 0)));
  }
  if (!entryDesc.empty()) {
    (void)interp.run("main", entryDesc, args);
  }
  b2::passes::snapshotDispatchProfile(interp, storage);
  return &storage;
}

int main(int argc, char** argv) {
  bool quiet = false;
  bool optimize = false;
  bool inl = false;
  bool pgo = false;
  bool pea = false;
  bool exec = false;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--quiet" || arg == "-q") {
      quiet = true;
    } else if (arg == "-O" || arg == "--optimize") {
      optimize = true;
    } else if (arg == "--inline" || arg == "-i") {
      inl = true;
    } else if (arg == "--pgo") {
      pgo = true;
      inl = true; // the profile only has a consumer through the engine
    } else if (arg == "--pea") {
      pea = true;
    } else if (arg == "--exec") {
      exec = true; optimize = true; inl = true;
    } else if (arg == "--help" || arg == "-h") {
      std::printf(
          "usage: b2graph [--quiet] [-O] [--inline] [--pgo] [--pea] file.rbc...\n"
          "  parse RBC text, verify, build every method's IR graph,\n"
          "  IR-verify, and print (the builder's debug surface)\n"
          "  --inline: run the ICDG inline engine before the pipeline\n"
          "      (verified after every site; decision log + telemetry line\n"
          "      per method; docs/inlining.md)\n"
          "  --pgo: run the program in T0 FIRST (training run; the entry\n"
          "      inference matches b2run), snapshot the per-site dispatch\n"
          "      profile (icdg.md Phase 1 / interp_contract.md SS8.1), and\n"
          "      feed it to the inline engine - monomorphic virtual /\n"
          "      interface sites GUARD-INLINE behind a TypeProfile guard\n"
          "      (Rule 122 SpecMeta + ClassHierarchy dependency; implies\n"
          "      --inline; the training run's program output is not part\n"
          "      of this tool's output)\n"
          "  --pea: run the CM-PEA engine and print the per-allocation\n"
          "      escape decision log (lattice grade, disposition, refusal\n"
          "      reason, field/forwarding counts; docs/special_passes.md\n"
          "      section 1; runs after --inline - the post-inline graph is\n"
          "      the cross-method data flow)\n"
          "  -O: run the early-cleanup + GVN + PEA pipeline after the\n"
          "      build (verified between passes, deterministic; telemetry\n"
          "      line per method)\n");
      return 0;
    } else {
      files.push_back(arg);
    }
  }
  if (files.empty()) {
    std::fprintf(stderr, "b2graph: no input file (try --help)\n");
    return 1;
  }
  int failures = 0;
  for (const std::string& f : files) {
    std::ifstream in(f);
    if (!in) {
      std::fprintf(stderr, "b2graph: cannot open %s\n", f.c_str());
      ++failures;
      continue;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    if (!quiet) {
      std::printf("# %s\n", f.c_str());
    }
    const auto parsed = b2::rbc::parseRbcText(ss.str());
    if (!parsed) {
      std::fprintf(stderr, "b2graph: parse error at offset %u: %s\n",
                   parsed.error().offset, parsed.error().message.c_str());
      ++failures;
      continue;
    }
    // One training run + snapshot per FILE (the profile is a property of
    // the program, shared by all of its method graphs).
    b2::passes::DispatchProfile storage;
    const b2::passes::DispatchProfile* profile = nullptr;
    if (pgo) {
      profile = trainAndSnapshot(*parsed, storage);
    }
    if (exec) {
      failures += execProgram(*parsed, quiet, optimize, inl, pea, profile) == 0 ? 0 : 1;
    } else {
      failures += run(*parsed, quiet, optimize, inl, pea, profile) == 0 ? 0 : 1;
    }
  }
  return failures == 0 ? 0 : 1;
}
