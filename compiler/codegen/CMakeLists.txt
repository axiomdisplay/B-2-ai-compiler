# B-2 codegen: T1 stencil instantiation, code cache, execution engine.
# WHY: the backend milestone of docs/stencils.md SS6 - a StencilPlan becomes
# executable x86-64 code (build-time archive, copy-and-patch instantiation,
# W^X publish) and deoptimizes to T0 per Amendment B.3. Stencil Rule 1: the
# archive is a BUILD-time artifact - stencilgen runs in the build, the
# runtime only copies and patches.

# ---------------------------------------------------------------------------
# stencilgen (build-time tool, docs/stencils.md SS5 / Stencil Rule 1).
# Emits the archive + the embedded ArchiveData.cpp. Links ONLY the manifest
# (b2::baseline) and the body emitter - never the runtime pieces (no cycle:
# the tool generates the bytes it must not link).
# ---------------------------------------------------------------------------
add_executable(b2stencilgen EXCLUDE_FROM_ALL
  ../../tools/stencilgen/stencilgen.cpp
  src/StencilBodies.cpp
  src/Archive.cpp
)
target_link_libraries(b2stencilgen PRIVATE b2::baseline)
target_include_directories(b2stencilgen PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR})
target_compile_features(b2stencilgen PRIVATE cxx_std_26)
set_target_properties(b2stencilgen PROPERTIES
  CXX_EXTENSIONS OFF
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools/stencilgen
)

# The generated embedded TU (regenerated on every build; deterministic).
set(B2_CODEGEN_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${B2_CODEGEN_GEN_DIR})
add_custom_command(
  OUTPUT ${B2_CODEGEN_GEN_DIR}/ArchiveData.cpp
         ${B2_CODEGEN_GEN_DIR}/x86_64_v1.archive
  COMMAND $<TARGET_FILE:b2stencilgen> --out-dir ${B2_CODEGEN_GEN_DIR}
  DEPENDS b2stencilgen
  COMMENT "b2stencilgen: generating the x86-64 v1 stencil archive"
  VERBATIM
)
add_custom_target(b2_codegen_archive_data
  DEPENDS ${B2_CODEGEN_GEN_DIR}/ArchiveData.cpp
)

# ---------------------------------------------------------------------------
# libb2_codegen: archive + instantiator + engine + the embedded bytes.
# ---------------------------------------------------------------------------
add_library(b2_codegen STATIC
  src/Archive.cpp
  src/Embedded.cpp
  src/StencilBodies.cpp
  src/Instantiate.cpp
  src/Engine.cpp
  src/T2Lowering.cpp
  ${B2_CODEGEN_GEN_DIR}/ArchiveData.cpp
)
add_dependencies(b2_codegen b2_codegen_archive_data)
add_library(b2::codegen ALIAS b2_codegen)
target_include_directories(b2_codegen PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}> # "compiler/codegen/src/..." includes
)
target_compile_features(b2_codegen PUBLIC cxx_std_26)
set_target_properties(b2_codegen PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(b2_codegen PUBLIC b2::baseline b2::interp b2::ir)

if(B2_BUILD_TOOLS)
  add_executable(b2jit tools/b2jit.cpp)
  target_link_libraries(b2jit PRIVATE b2::codegen)
  target_compile_features(b2jit PRIVATE cxx_std_26)
  set_target_properties(b2jit PROPERTIES CXX_EXTENSIONS OFF)
endif()
