// B-2 codegen - archive serialization, loading, validation, and the embedded
// build-time artifact.
//
// WHY THIS FILE EXISTS:
// The archive is a build-time artifact (Stencil Rule 1) that must travel in
// a stable, inspectable binary form (stencilgen output) AND be available to
// the runtime without file I/O (embedded bytes in a generated TU). Both
// consumers share this one serializer/loader, and both the tool and Tier1
// gate on the same validateArchive (Stencil Rule 4: identity + alignment
// checks happen at load AND at use, never best-effort).
//
// SERIALIZED LAYOUT (little-endian; docs/codegen_contract.md SS4):
//   header:  u32 magic, version, target_arch, abi_hash, manifest_count,
//            record_count
//   record:  u32 name_len, name bytes, u32 code_size, code bytes,
//            u32 hole_count, holes (u32 code_offset, u8 kind, u8 width,
//            u8 source, u8 tag, u8 helper_id, u8 step, u8 pad,
//            u32 imm(delta))
// Deterministic: identical manifest -> identical bytes (Rule 124).

#include "b2/codegen/Archive.h"

#include <cstring>

namespace b2::codegen {

namespace {

struct Reader {
  const std::uint8_t* p;
  const std::uint8_t* end;
  bool bad = false;

  [[nodiscard]] std::uint32_t u32() {
    if (bad || end - p < 4) {
      bad = true;
      return 0;
    }
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    p += 4;
    return v;
  }
  [[nodiscard]] std::uint8_t u8() {
    if (bad || p == end) {
      bad = true;
      return 0;
    }
    return *p++;
  }
  [[nodiscard]] std::span<const std::uint8_t> bytes(std::size_t n) {
    if (bad || static_cast<std::size_t>(end - p) < n) {
      bad = true;
      return {};
    }
    const std::uint8_t* out = p;
    p += n;
    return {out, n};
  }
};

struct Writer {
  std::vector<std::uint8_t>& out;
  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
  }
  void u8(std::uint8_t v) { out.push_back(v); }
  void str(std::string_view s) {
    u32(static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
  }
};

} // namespace

std::vector<std::uint8_t> serializeArchive(const Archive& archive) {
  std::vector<std::uint8_t> out;
  Writer w{out};
  w.u32(archive.header.magic);
  w.u32(archive.header.version);
  w.u32(archive.header.target_arch);
  w.u32(archive.header.abi_hash);
  w.u32(archive.header.manifest_count);
  w.u32(archive.header.record_count);
  for (const ArchiveRecord& rec : archive.records) {
    w.str(rec.name);
    w.u32(static_cast<std::uint32_t>(rec.code.size()));
    out.insert(out.end(), rec.code.begin(), rec.code.end());
    w.u32(static_cast<std::uint32_t>(rec.holes.size()));
    for (const ArchiveHole& h : rec.holes) {
      w.u32(h.code_offset);
      w.u8(static_cast<std::uint8_t>(h.kind));
      w.u8(h.width);
      w.u8(static_cast<std::uint8_t>(h.source));
      w.u8(static_cast<std::uint8_t>(h.tag));
      w.u8(h.helper_id);
      w.u8(h.step);
      w.u8(0); // pad (reserved; keeps records 4-aligned)
      w.u32(h.imm);
    }
  }
  return out;
}

Archive loadArchive(std::span<const std::uint8_t> bytes) {
  Archive out;
  Reader r{bytes.data(), bytes.data() + bytes.size()};
  out.header.magic = r.u32();
  out.header.version = r.u32();
  out.header.target_arch = r.u32();
  out.header.abi_hash = r.u32();
  out.header.manifest_count = r.u32();
  out.header.record_count = r.u32();
  if (r.bad) {
    return {};
  }
  out.records.reserve(out.header.record_count);
  for (std::uint32_t i = 0; i < out.header.record_count && !r.bad; ++i) {
    ArchiveRecord rec;
    const std::uint32_t nameLen = r.u32();
    const std::span<const std::uint8_t> nameBytes = r.bytes(nameLen);
    if (r.bad) {
      return {};
    }
    // Names point into the input buffer, which callers keep alive for the
    // archive's lifetime (embedded bytes are static; stencilgen serializes
    // immediately). Copying would allocate per record (Rule 16 discipline:
    // names are diagnostics, not hot-path data).
    rec.name = std::string_view(
        reinterpret_cast<const char*>(nameBytes.data()), nameBytes.size());
    const std::uint32_t codeSize = r.u32();
    const std::span<const std::uint8_t> code = r.bytes(codeSize);
    if (r.bad) {
      return {};
    }
    rec.code.assign(code.begin(), code.end());
    const std::uint32_t holeCount = r.u32();
    rec.holes.resize(holeCount);
    for (ArchiveHole& h : rec.holes) {
      h.code_offset = r.u32();
      h.kind = static_cast<PatchKind>(r.u8());
      h.width = r.u8();
      h.source = static_cast<PatchSource>(r.u8());
      h.tag = static_cast<HoleTag>(r.u8());
      h.helper_id = r.u8();
      h.step = r.u8();
      (void)r.u8(); // pad
      h.imm = r.u32();
    }
    out.records.push_back(std::move(rec));
  }
  if (r.bad) {
    return {};
  }
  return out;
}

ArchiveCheckResult validateArchive(const Archive& archive,
                                   const baseline::StencilSet& set) {
  ArchiveCheckResult res;
  const auto fail = [&res](std::string msg) {
    res.ok = false;
    res.error = std::move(msg);
    return res;
  };

  if (archive.header.magic != kStencilArchiveMagic) {
    return fail("archive magic mismatch");
  }
  if (archive.header.version != kStencilArchiveVersion) {
    return fail("archive version " + std::to_string(archive.header.version) +
                " != supported " + std::to_string(kStencilArchiveVersion));
  }
  if (archive.header.target_arch != kTargetArchX86_64) {
    return fail("archive target arch is not x86-64");
  }
  if (archive.header.abi_hash != kT1AbiHashV1) {
    return fail("archive abi hash mismatch (frame contract changed?)");
  }
  if (archive.header.manifest_count != set.stencils.size()) {
    return fail("archive manifest count " +
                std::to_string(archive.header.manifest_count) +
                " != set size " + std::to_string(set.stencils.size()));
  }
  if (archive.records.size() < archive.header.manifest_count) {
    return fail("archive has fewer records than its manifest count");
  }

  // Manifest-aligned records: names + Plan-hole subsequence + hole bounds.
  for (std::size_t i = 0; i < set.stencils.size(); ++i) {
    const baseline::StencilDesc& d = set.stencils[i];
    const ArchiveRecord& rec = archive.records[i];
    if (rec.name != d.name) {
      return fail("record " + std::to_string(i) + " name '" +
                  std::string(rec.name) + "' != manifest '" +
                  std::string(d.name) + "'");
    }
    // Plan-tagged holes must be an IN-ORDER SUBSEQUENCE of the manifest's
    // declared holes, matched by SOURCE (the manifest kind names the
    // semantic value - DeoptId, FieldOffset - while the archive kind names
    // the ENCODING - BranchRel32, Imm32 - so kind equality is not required;
    // each source appears at most in-order-consumable per record in v0).
    std::size_t mi = 0;
    for (const ArchiveHole& h : rec.holes) {
      if (h.code_offset + h.width > rec.code.size()) {
        return fail("record '" + std::string(rec.name) + "' hole at " +
                    std::to_string(h.code_offset) + " exceeds code size " +
                    std::to_string(rec.code.size()));
      }
      if (h.width != 1 && h.width != 2 && h.width != 4 && h.width != 8) {
        return fail("record '" + std::string(rec.name) + "' hole width " +
                    std::to_string(h.width) + " is not 1/2/4/8");
      }
      if (h.tag != HoleTag::Plan) {
        continue;
      }
      while (mi < d.patch_count && d.patches[mi].source != h.source) {
        ++mi;
      }
      if (mi >= d.patch_count) {
        return fail("record '" + std::string(rec.name) +
                    "' Plan hole (source " +
                    std::to_string(static_cast<unsigned>(h.source)) +
                    ") not present in the manifest hole list in order");
      }
      ++mi;
    }
    // Available opcode/super stencils must carry real bytes (the two
    // documented exceptions: the switch ops, expanded at instantiation).
    if (d.availability == baseline::StencilAvailability::Available &&
        rec.code.empty() && d.pattern_len >= 1 &&
        d.pattern[0].op != rbc::Op::Tableswitch &&
        d.pattern[0].op != rbc::Op::Lookupswitch) {
      return fail("Available stencil '" + std::string(rec.name) +
                  "' has an empty archive record");
    }
  }

  // Internal templates must all exist.
  for (const std::string_view name :
       {kInternalEntry, kInternalDeoptThunk, kInternalDeoptExit,
        kInternalSwitchCase}) {
    if (archive.internal(name) == nullptr) {
      return fail("internal template '" + std::string(name) + "' missing");
    }
  }
  const ArchiveRecord* entry = archive.internal(kInternalEntry);
  if (entry->holes.size() != 1) {
    return fail("__entry must carry exactly one target hole");
  }

  res.ok = true;
  return res;
}

} // namespace b2::codegen
