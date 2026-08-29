# B-2 frontend: Java source -> tokens -> AST.
add_library(b2_frontend STATIC
  src/SourceManager.cpp
  src/Diagnostics.cpp
  src/Token.cpp
  src/Lexer.cpp
  src/Parser.cpp
  src/ParserStmt.cpp
  src/ParserExpr.cpp
  src/AstDumper.cpp
)

add_library(b2::frontend ALIAS b2_frontend)

target_include_directories(b2_frontend PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)

target_compile_features(b2_frontend PUBLIC cxx_std_26)

if(B2_BUILD_TOOLS)
  add_executable(b2parse tools/b2parse.cpp)
  target_link_libraries(b2parse PRIVATE b2::frontend)
endif()
