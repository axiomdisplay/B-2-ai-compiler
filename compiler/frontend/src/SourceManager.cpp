#include "b2/frontend/SourceManager.h"

#include <algorithm>

namespace b2::frontend {

void SourceManager::load(std::string fileName, std::string contents) {
  fileName_ = std::move(fileName);
  source_ = std::move(contents);

  lineStarts_.clear();
  lineStarts_.push_back(0);
  for (std::size_t i = 0; i < source_.size(); ++i) {
    const char c = source_[i];
    if (c == '\n') {
      lineStarts_.push_back(static_cast<std::uint32_t>(i + 1));
    } else if (c == '\r') {
      // CRLF counts as a single terminator (the '\n' branch handles it);
      // a lone CR also terminates a line.
      if (i + 1 < source_.size() && source_[i + 1] == '\n') continue;
      lineStarts_.push_back(static_cast<std::uint32_t>(i + 1));
    }
  }
}

SourceManager::LineCol SourceManager::lineCol(std::uint32_t offset) const noexcept {
  if (offset > source_.size()) offset = static_cast<std::uint32_t>(source_.size());
  // First line start strictly greater than offset.
  auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
  const std::uint32_t line = static_cast<std::uint32_t>(it - lineStarts_.begin());
  const std::uint32_t start = *(it - 1);
  return {line, offset - start + 1};
}

std::string_view SourceManager::lineText(std::uint32_t line) const noexcept {
  if (line == 0 || line > lineStarts_.size()) return {};
  const std::uint32_t start = lineStarts_[line - 1];
  const std::uint32_t end = line < lineStarts_.size() ? lineStarts_[line]
                                                      : static_cast<std::uint32_t>(source_.size());
  std::string_view v = source_;
  v = v.substr(start, end - start);
  while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.remove_suffix(1);
  return v;
}

}  // namespace b2::frontend
