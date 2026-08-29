#pragma once
// B-2 RBC - text format: printable, parseable RBC (golden tests, b2rbc tool).
//
// WHY THIS FILE EXISTS:
// Golden tests are the middle end's stability oracle (docs/cpp26_standards.md,
// Golden IR Tests): byte-stable text round-trips catch unintended RBC changes
// before they become Java-visible. The text format is also the human debug
// surface for frontend lowering reviews.
//
// GRAMMAR (whitespace-separated, '#' starts a line comment):
//   program   := { method }
//   method    := ".method" [flags...] name descriptor
//                {".regs" N} {".locals" N}
//                {directive | label | insn} ".end"
//   flags     := "static" | "public" | "private" | "protected" | "final"
//                | "synchronized" | "native" | "abstract" | "varargs"
//   directive := ".const" cid "=" kind payload
//                  kind := "int" i32 | "long" i64 | "float" f | "double" d
//                        | "utf8" s | "string" s | "class" s
//                        | "nametype" n s | "field" c n s | "method" c n s
//                        | "imethod" c n s | "methodtype" s
//                        | "methodhandle" k s | "indy" n s
//                        | "switch" "{" [int ":" label]* "}"
//   label     := ident ":"
//   insn      := mnemonic [operands]
//   operands  := "r"dec | "l"dec | "c"dec | label | int | atype
//
// Registers are r0.., local slots l0.., constants c0... Branch operands are
// labels; the printer emits one label per branch target.

#include <expected>
#include <string>
#include <string_view>

#include "b2/rbc/Rbc.h"

namespace b2::rbc {

// Parse error: byte offset into the input + message. Offsets make golden
// negative tests exact (frontend diagnostic discipline, Rule 47).
struct TextError {
  std::uint32_t offset = 0;
  std::string message;
};

[[nodiscard]] std::expected<Program, TextError> parseRbcText(std::string_view text);

// Deterministic printer: same Program -> byte-identical output. One
// instruction per line, labels only for branch targets, constants emitted
// first in index order.
[[nodiscard]] std::string printRbcText(const Program& p);
[[nodiscard]] std::string printRbcText(const Method& m);

} // namespace b2::rbc
