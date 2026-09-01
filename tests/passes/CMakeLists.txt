add_executable(b2_passes_tests
  TestMain.cpp
  GraphBuilderTests.cpp
  PassTests.cpp
)

target_link_libraries(b2_passes_tests PRIVATE b2::passes b2::ir b2::rbc)
target_compile_features(b2_passes_tests PRIVATE cxx_std_26)

add_test(NAME passes_tests COMMAND b2_passes_tests)
set_tests_properties(passes_tests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# The CM-PEA engine tests (keys 65/66/67/69): hand-built graphs pin the
# classification table, the materialization shape, and the scalar
# replacement; the corpus gate pins verified/idempotent/deterministic.
add_executable(b2_pea_tests
  TestMain.cpp
  EscapeTests.cpp
)

target_link_libraries(b2_pea_tests PRIVATE b2::passes b2::ir b2::rbc)
target_compile_features(b2_pea_tests PRIVATE cxx_std_26)

add_test(NAME pea_tests COMMAND b2_pea_tests)
set_tests_properties(pea_tests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# The inline test binary links b2::interp for the end-to-end ICDG Phase 2
# test (T0 training run -> DispatchProfileSnapshot.h -> guard-inline):
# a test harness may integrate tiers like the tools do (b2jit, b2graph);
# the b2_passes LIBRARY never links interp (the charter boundary).
add_executable(b2_inline_tests
  TestMain.cpp
  InlineTests.cpp
)

target_link_libraries(b2_inline_tests PRIVATE b2::passes b2::ir b2::rbc b2::interp)
target_include_directories(b2_inline_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/compiler/passes/tools)
target_compile_features(b2_inline_tests PRIVATE cxx_std_26)

add_test(NAME inline_tests COMMAND b2_inline_tests)
set_tests_properties(inline_tests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
