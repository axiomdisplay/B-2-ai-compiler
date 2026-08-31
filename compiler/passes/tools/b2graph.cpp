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
// Exit codes: 0 = all methods built and verified; 1 = any parse/verify/
// build/IR-verify failure (diagnostics on stderr).

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "b2/ir/Printer.h"
#include "b2/ir/Verifier.h"
#include "b2/passes/GraphBuilder.h"
#include "b2/passes/Inline.h"
#include "b2/passes/Passes.h"
#include "b2/rbc/RbcText.h"
#include "b2/rbc/Verifier.h"

namespace {

int run(const std::string& text, bool quiet, bool optimize, bool inl) {
  const auto parsed = b2::rbc::parseRbcText(text);
  if (!parsed) {
    std::fprintf(stderr, "b2graph: parse error at offset %u: %s\n",
                 parsed.error().offset, parsed.error().message.c_str());
    return 1;
  }
  const b2::rbc::Program& prog = *parsed;

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
      // The ICDG direct-inline engine (docs/inlining.md): the same
      // resolver built the graph (the unified id space), the verifier
      // runs after every site, decisions + telemetry per method.
      // Fail-closed on any failure - the review surface never shows an
      // unverified graph.
      const b2::passes::InlineResult ir =
          b2::passes::runInlining(g, resolver);
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
        std::printf("  # inline: sites=%u inlined=%u refused=%u nodes=+%u "
                    "deopts=+%u merges=%u depth=%u converged=%d\n",
                    ir.telemetry.sitesConsidered, ir.telemetry.sitesInlined,
                    ir.telemetry.sitesRefused, ir.telemetry.nodesAdded,
                    ir.telemetry.deoptsEmitted, ir.telemetry.exitMerges,
                    ir.telemetry.maxDepthReached,
                    ir.telemetry.converged ? 1 : 0);
        for (const b2::passes::InlineDecision& d : ir.decisions) {
          std::printf("  # inline n%u -> m%u d%u: %s (%s; insns=%u slots=%u "
                      "nodes=%u)\n",
                      d.call, d.target, d.depth,
                      d.action == b2::passes::InlineAction::DirectInline
                          ? "DIRECT-INLINE"
                          : "KEEP-INDIRECT",
                      d.reason, d.calleeInsns, d.calleeSlots, d.calleeNodes);
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
                    "folds=%u gvn=%u converged=%d\n",
                    pr.telemetry.rounds, pr.telemetry.rewrites,
                    pr.telemetry.removals, pr.telemetry.folds,
                    pr.telemetry.gvnDedups,
                    pr.telemetry.converged ? 1 : 0);
      }
    }
    if (!quiet) {
      std::fputs(b2::ir::print(g).c_str(), stdout);
    }
  }
  return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
  bool quiet = false;
  bool optimize = false;
  bool inl = false;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--quiet" || arg == "-q") {
      quiet = true;
    } else if (arg == "-O" || arg == "--optimize") {
      optimize = true;
    } else if (arg == "--inline" || arg == "-i") {
      inl = true;
    } else if (arg == "--help" || arg == "-h") {
      std::printf(
          "usage: b2graph [--quiet] [-O] [--inline] file.rbc...\n"
          "  parse RBC text, verify, build every method's IR graph,\n"
          "  IR-verify, and print (the builder's debug surface)\n"
          "  --inline: run the ICDG direct-inline engine before the\n"
          "      pipeline (verified after every site; decision log +\n"
          "      telemetry line per method; docs/inlining.md)\n"
          "  -O: run the early-cleanup + GVN pipeline after the build\n"
          "      (verified between passes, deterministic; telemetry line\n"
          "      per method)\n");
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
    failures += run(ss.str(), quiet, optimize, inl) == 0 ? 0 : 1;
  }
  return failures == 0 ? 0 : 1;
}
