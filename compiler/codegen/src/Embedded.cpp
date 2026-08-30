// B-2 codegen - the embedded archive access (libb2_codegen side).
//
// WHY THIS FILE EXISTS:
// The archive bytes live in the GENERATED ArchiveData.cpp (tools/stencilgen
// output, Stencil Rule 1); only libb2_codegen links it. stencilgen itself
// must NOT link this file (it would be circular: the tool generates the
// bytes it would link). Keeping the accessor here gives the runtime exactly
// one seam to the embedded artifact, validated on first use.

#include "b2/codegen/Archive.h"

namespace b2::codegen {

// Defined in the generated ArchiveData.cpp.
std::span<const std::uint8_t> embeddedArchiveX86_64Bytes();

std::span<const std::uint8_t> embeddedArchiveX86_64() {
  return embeddedArchiveX86_64Bytes();
}

ArchiveCheckResult loadEmbeddedX86_64(const baseline::StencilSet& set,
                                      Archive& out) {
  out = loadArchive(embeddedArchiveX86_64());
  if (out.records.empty()) {
    ArchiveCheckResult bad;
    bad.ok = false;
    bad.error = "embedded archive failed to load";
    return bad;
  }
  return validateArchive(out, set);
}

} // namespace b2::codegen
