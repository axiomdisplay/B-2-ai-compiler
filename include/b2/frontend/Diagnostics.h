#pragma once
// B-2 Frontend - diagnostic collection and formatting.

#include <cstdint>
#include <string>
#include <vector>

#include "b2/frontend/SourceManager.h"

namespace b2::frontend {

enum class Severity : std::uint8_t { Note, Warning, Error };

struct Diagnostic {
  Severity severity = Severity::Error;
  std::uint32_t offset = 0;  // raw byte offset into the source buffer
  std::string message;
};

// Collects diagnostics for one compilation. Errors are capped (default 100)
// so arbitrary garbage input cannot exhaust memory.
class DiagnosticEngine {
 public:
  explicit DiagnosticEngine(const SourceManager& sm) : sm_(&sm) {}

  void error(std::uint32_t offset, std::string message);
  void warning(std::uint32_t offset, std::string message);
  void note(std::uint32_t offset, std::string message);

  [[nodiscard]] bool hasErrors() const noexcept { return errors_ != 0; }
  [[nodiscard]] std::uint32_t errorCount() const noexcept { return errors_; }
  [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept {
    return diags_;
  }
  [[nodiscard]] const Diagnostic* last() const noexcept {
    return diags_.empty() ? nullptr : &diags_.back();
  }

  void setErrorLimit(std::uint32_t limit) noexcept { errorLimit_ = limit; }

  // "File.java:12:5: error: message" - line/column come from the RAW source.
  [[nodiscard]] std::string format(const Diagnostic& d) const;
  [[nodiscard]] std::string formatAll() const;

 private:
  void emit(Severity s, std::uint32_t offset, std::string& message);

  const SourceManager* sm_;
  std::vector<Diagnostic> diags_;
  std::uint32_t errors_ = 0;
  std::uint32_t errorLimit_ = 100;
  bool limitAnnounced_ = false;
};

}  // namespace b2::frontend
