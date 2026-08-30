# B-2 frontend: Java source -> tokens -> AST -> (lowering) RBC.
add_library(b2_frontend STATIC
  src/SourceManager.cpp
  src/Diagnostics.cpp
  src/Token.cpp
  src/Lexer.cpp
  src/Parser.cpp
  src/ParserStmt.cpp
  src/ParserExpr.cpp
  src/AstDumper.cpp
  src/Lower.cpp
  src/LowerExpr.cpp
  src/LowerStmt.cpp
)

add_library(b2::frontend ALIAS b2_frontend)

target_include_directories(b2_frontend PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)

target_compile_features(b2_frontend PUBLIC cxx_std_26)

# WHY rbc: the lowering emits Register Bytecode through the frozen
# middle-end contracts (RbcBuilder / Method / Program). Read-only
# consumption; opcode/format changes go through messages per messaging.md.
target_link_libraries(b2_frontend PUBLIC b2::rbc)

if(B2_BUILD_TOOLS)
  add_executable(b2parse tools/b2parse.cpp)
  target_link_libraries(b2parse PRIVATE b2::frontend)
endif()
