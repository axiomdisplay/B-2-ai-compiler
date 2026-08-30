add_executable(b2_frontend_tests
  TestMain.cpp
  LexerTests.cpp
  ParserTests.cpp
  CorpusTest.cpp
  LowerTests.cpp
  LowerCorpus.cpp
)

target_link_libraries(b2_frontend_tests PRIVATE b2::frontend b2::interp)
target_compile_features(b2_frontend_tests PRIVATE cxx_std_26)

# Lowering corpus: .java + .java.expected pairs, executed on T0.
target_compile_definitions(b2_frontend_tests PRIVATE
  B2_LOWER_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/lower_corpus")

add_test(NAME frontend_tests COMMAND b2_frontend_tests)
set_tests_properties(frontend_tests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
