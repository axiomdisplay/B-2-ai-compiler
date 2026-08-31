add_executable(b2_passes_tests
  TestMain.cpp
  GraphBuilderTests.cpp
)

target_link_libraries(b2_passes_tests PRIVATE b2::passes b2::ir b2::rbc)
target_compile_features(b2_passes_tests PRIVATE cxx_std_26)

add_test(NAME passes_tests COMMAND b2_passes_tests)
set_tests_properties(passes_tests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
