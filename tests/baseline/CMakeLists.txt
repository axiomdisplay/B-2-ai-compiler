# B-2 baseline (T1) tests: the charter-mandated plan-stage suite (Task T1-B).
#
# Mirrors tests/interp/CMakeLists.txt: one test executable (TestMain +
# unit suite + corpus sweep) linking the tier under test, with the corpus
# directory pinned as an ABSOLUTE compile definition so the sweep never
# depends on the process working directory. The swept corpus is the
# interpreter's tests/interp/corpus (the documented v0 reality: those are
# interpreter-oriented programs, so most mains refuse no-stencil-for-op at
# their first un-quickened invoke/ldc - the honest v0 availability map).
add_executable(b2_baseline_tests
  TestMain.cpp
  BaselineTests.cpp
  CorpusTest.cpp
)

target_link_libraries(b2_baseline_tests PRIVATE b2::baseline)
target_compile_features(b2_baseline_tests PRIVATE cxx_std_26)

# Corpus location as a compile definition (mirrors B2_INTERP_CORPUS_DIR).
target_compile_definitions(b2_baseline_tests PRIVATE
  B2_BASELINE_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../interp/corpus")

add_test(NAME baseline_tests COMMAND b2_baseline_tests)
set_tests_properties(baseline_tests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
