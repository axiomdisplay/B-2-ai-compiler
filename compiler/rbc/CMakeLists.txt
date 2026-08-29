# B-2 RBC: register bytecode core (opcodes, builder, verifier, text format).
# WHY: RBC is the shared contract consumed by T0/T1/T2/T3; it must build as a
# small, dependency-free static library so every tier and the AOT toolchain
# can link it without pulling in frontend or IR code.
add_library(b2_rbc STATIC
  src/Opcode.cpp
  src/Rbc.cpp
  src/RbcBuilder.cpp
  src/Verifier.cpp
  src/RbcText.cpp
)

add_library(b2::rbc ALIAS b2_rbc)

target_include_directories(b2_rbc PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)

target_compile_features(b2_rbc PUBLIC cxx_std_26)

if(B2_BUILD_TOOLS)
  add_executable(b2rbc tools/b2rbc.cpp)
  target_link_libraries(b2rbc PRIVATE b2::rbc)
endif()
