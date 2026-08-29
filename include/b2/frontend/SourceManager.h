#pragma once
// B-2 Frontend - source text ownership and position mapping.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace b2::frontend {

// Byte range into the raw (on-disk) source buffer.
struct SourceRange {
  std::uint32_t offset = 0;
  std::uint32_t length = 0;

  constexpr SourceRange() = default;
  constexpr SourceRange(std::uint32_t off, std::uint32_t len)
      : offset(off), length(len) {}
};

// Owns one source file and answers position queries against it.
//
// The buffer is assumed to be UTF-8 (invalid sequences are tolerated by the
// lexer; they surface as diagnostics, never as crashes).
class SourceManager {
 public:
  void load(std::string fileName, std::string contents);

  [[nodiscard]] std::string_view fileName() const noexcept { return fileName_; }
  [[nodiscard]] std::string_view source() const noexcept { return source_; }
  [[nodiscard]] std::uint32_t sourceSize() const noexcept {
    return static_cast<std::uint32_t>(source_.size());
  }

  struct LineCol {
    std::uint32_t line;   // 1-based
    std::uint32_t column; // 1-based, in bytes
  };

  [[nodiscard]] LineCol lineCol(std::uint32_t offset) const noexcept;
  [[nodiscard]] std::uint32_t lineCount() const noexcept {
    return static_cast<std::uint32_t>(lineStarts_.size());
  }

  // Text of a 1-based line, without its terminator.
  [[nodiscard]] std::string_view lineText(std::uint32_t line) const noexcept;

 private:
  std::string fileName_;
  std::string source_;
  std::vector<std::uint32_t> lineStarts_; // offset of each line start
};

}  // namespace b2::frontend
