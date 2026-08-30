# B-2 codegen (T1 instantiation) tests: the machine-code oracle.
#
# Mirrors tests/baseline: one test executable (TestMain + unit suite +
# differential corpus sweep) linking the tier under test, with both corpus
# directories pinned as ABSOLUTE compile definitions (no cwd dependence).
# The sweeps are the Rule-36 differential: T1 machine code vs the T0 golden
# .expected twins, byte-exact.
add_executable(b2_codegen_tests
  TestMain.cpp
  CodeGenTests.cpp
  CorpusTest.cpp
)

target_link_libraries(b2_codegen_tests PRIVATE b2::codegen b2::frontend)
target_compile_features(b2_codegen_tests PRIVATE cxx_std_26)

target_compile_definitions(b2_codegen_tests PRIVATE
  B2_CODEGEN_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../interp/corpus"
  B2_CODEGEN_FRONTEND_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../frontend/lower_corpus")

add_test(NAME codegen_tests COMMAND b2_codegen_tests)
set_tests_properties(codegen_tests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
