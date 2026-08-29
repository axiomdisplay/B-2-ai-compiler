add_executable(b2_rbc_tests
  TestMain.cpp
  OpcodeTests.cpp
  BuilderTests.cpp
  VerifierTests.cpp
  TextTests.cpp
  CorpusTest.cpp
)

target_link_libraries(b2_rbc_tests PRIVATE b2::rbc)
target_compile_features(b2_rbc_tests PRIVATE cxx_std_26)

# Corpus location as a compile definition: the corpus test must not depend on
# the process working directory (the frontend suite instead pins
# WORKING_DIRECTORY; an absolute definition is the more robust variant).
target_compile_definitions(b2_rbc_tests PRIVATE
  B2_RBC_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")

add_test(NAME rbc_tests COMMAND b2_rbc_tests)
set_tests_properties(rbc_tests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
