#include "b2/frontend/Diagnostics.h"

namespace b2::frontend {

namespace {

const char* severityName(Severity s) noexcept {
  switch (s) {
    case Severity::Note:    return "note";
    case Severity::Warning: return "warning";
    case Severity::Error:   return "error";
  }
  return "error";
}

}  // namespace

void DiagnosticEngine::error(std::uint32_t offset, std::string message) {
  emit(Severity::Error, offset, message);
}

void DiagnosticEngine::warning(std::uint32_t offset, std::string message) {
  emit(Severity::Warning, offset, message);
}

void DiagnosticEngine::note(std::uint32_t offset, std::string message) {
  emit(Severity::Note, offset, message);
}

void DiagnosticEngine::emit(Severity s, std::uint32_t offset, std::string& message) {
  if (s == Severity::Error) {
    if (errors_ >= errorLimit_) {
      if (!limitAnnounced_) {
        limitAnnounced_ = true;
        diags_.push_back({Severity::Note, offset,
                          "too many errors; suppressing further diagnostics"});
      }
      return;
    }
    ++errors_;
  }
  Diagnostic d;
  d.severity = s;
  d.offset = offset;
  d.message = std::move(message);
  diags_.push_back(std::move(d));
}

std::string DiagnosticEngine::format(const Diagnostic& d) const {
  const auto lc = sm_->lineCol(d.offset);
  std::string out;
  out.reserve(sm_->fileName().size() + d.message.size() + 32);
  out += sm_->fileName();
  out += ':';
  out += std::to_string(lc.line);
  out += ':';
  out += std::to_string(lc.column);
  out += ": ";
  out += severityName(d.severity);
  out += ": ";
  out += d.message;
  return out;
}

std::string DiagnosticEngine::formatAll() const {
  std::string out;
  for (const Diagnostic& d : diags_) {
    out += format(d);
    out += '\n';
  }
  return out;
}

}  // namespace b2::frontend
