add_executable(b2_frontend_tests
  TestMain.cpp
  LexerTests.cpp
  ParserTests.cpp
  CorpusTest.cpp
)

target_link_libraries(b2_frontend_tests PRIVATE b2::frontend)
target_compile_features(b2_frontend_tests PRIVATE cxx_std_26)

add_test(NAME frontend_tests COMMAND b2_frontend_tests)
set_tests_properties(frontend_tests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
