// B-2 RBC driver: parse, verify, and dump .rbc text files.
//
// WHY THIS FILE EXISTS:
// The middle end needs a human-facing entry point for golden-test triage and
// lowering reviews, exactly like b2parse is for the frontend. b2rbc reads a
// text-format RBC program, verifies every method, prints the verified (or
// failed) program, and exits non-zero when any error was reported - the same
// contract as b2parse (Rule 47: diagnostics must be actionable).
//
// Usage:
//   b2rbc [--dump] file.rbc        parse + verify (+ print back)
//   b2rbc --verify-only file.rbc   parse + verify, stay quiet on success

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "b2/rbc/RbcText.h"
#include "b2/rbc/Verifier.h"

namespace {

int fail(const std::string& what) {
  std::fprintf(stderr, "b2rbc: error: %s\n", what.c_str());
  return 1;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: b2rbc [--dump|--verify-only] file.rbc\n");
    return 2;
  }

  bool dump = true;
  const char* path = nullptr;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--dump") {
      dump = true;
    } else if (arg == "--verify-only") {
      dump = false;
    } else if (path == nullptr) {
      path = argv[i];
    } else {
      return fail("multiple input files are not supported yet");
    }
  }
  if (path == nullptr) {
    return fail("no input file");
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fail(std::string("cannot open ") + path);
  }
  std::ostringstream ss;
  ss << in.rdbuf();

  const std::string text = ss.str();
  auto parsed = b2::rbc::parseRbcText(text);
  if (!parsed) {
    const auto& err = parsed.error();
    std::fprintf(stderr, "%s:%u: error: %s\n", path, err.offset + 1,
                 err.message.c_str());
    return 1;
  }

  bool anyError = false;
  for (const b2::rbc::Method& m : parsed->methods) {
    // Abstract methods carry no body by contract; verify() accepts them.
    const auto result = b2::rbc::verify(m);
    if (!result.ok) {
      anyError = true;
      for (const auto& d : result.diags) {
        std::fprintf(stderr, "%s [%s%s]: pc %u: error: %s\n", path,
                     m.name.c_str(), m.descriptor.c_str(), d.pc,
                     d.message.c_str());
      }
    }
  }

  if (dump && !anyError) {
    std::fputs(b2::rbc::printRbcText(*parsed).c_str(), stdout);
  }
  return anyError ? 1 : 0;
}
