add_executable(b2_ir_tests
  TestMain.cpp
  GraphTests.cpp
  EffectTests.cpp
  VerifierTests.cpp
  PrinterTests.cpp
  V2Tests.cpp
)

target_link_libraries(b2_ir_tests PRIVATE b2::ir)
target_compile_features(b2_ir_tests PRIVATE cxx_std_26)

add_test(NAME ir_tests COMMAND b2_ir_tests)
set_tests_properties(ir_tests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
