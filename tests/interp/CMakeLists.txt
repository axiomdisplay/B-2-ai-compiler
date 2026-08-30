add_executable(b2_interp_tests
  TestMain.cpp
  InterpTests.cpp
  CorpusTest.cpp
)

target_link_libraries(b2_interp_tests PRIVATE b2::interp)
target_compile_features(b2_interp_tests PRIVATE cxx_std_26)

# Corpus location as a compile definition: the corpus test must not depend on
# the process working directory (mirrors tests/rbc/CMakeLists.txt; an absolute
# definition is the more robust variant).
target_compile_definitions(b2_interp_tests PRIVATE
  B2_INTERP_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")

add_test(NAME interp_tests COMMAND b2_interp_tests)
set_tests_properties(interp_tests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ---------------------------------------------------------------------------
# The portable-dispatch differential build (Law 36; interp_contract.md SS4):
# the portable switch-core library (b2_interp_portable, defined alongside
# b2_interp in compiler/interp) runs the SAME corpus against the SAME goldens.
# The computed-goto core above is only ever accepted because this binary
# proves the two dispatchers byte-identical on every corpus program.
add_executable(b2_interp_portable_tests
  TestMain.cpp
  InterpTests.cpp
  CorpusTest.cpp
)
target_link_libraries(b2_interp_portable_tests PRIVATE b2_interp_portable)
target_compile_features(b2_interp_portable_tests PRIVATE cxx_std_26)
target_compile_definitions(b2_interp_portable_tests PRIVATE
  B2_INTERP_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")

add_test(NAME interp_portable_tests COMMAND b2_interp_portable_tests)
set_tests_properties(interp_portable_tests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
