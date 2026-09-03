# B-2 Passes: the T2/T3 optimization pipeline (Amendment B.4).
# WHY: the RBC->IR graph builder is the pipeline's front door and the first
# consumer of b2::ir - it links b2_rbc (the input format) and b2_ir (the
# output), and nothing else. Pass transforms arrive behind it under
# compiler/passes/ per the 120-pass suite; the pass registry + pipeline
# driver are later deliverables.
set(B2_PASSES_SOURCES
  src/GraphBuilder.cpp
  src/Passes.cpp
  src/PassSupport.cpp
  src/Simplify.cpp
  src/SCCP.cpp
  src/ControlFlow.cpp
  src/DCE.cpp
  src/GVN.cpp
  src/Inline.cpp
  src/Escape.cpp
)

add_library(b2_passes STATIC ${B2_PASSES_SOURCES})
add_library(b2::passes ALIAS b2_passes)

target_include_directories(b2_passes PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)

target_link_libraries(b2_passes PUBLIC b2::ir b2::rbc)
target_compile_features(b2_passes PUBLIC cxx_std_26)

# b2graph: parse .rbc text -> build -> verify + print per method (the
# builder's debug surface; mirrors b2parse/b2run tooling conventions).
# --pgo additionally runs T0 and snapshots the dispatch profile: the
# TOOL links interp (an interp-linking integrator, like b2jit), the
# b2_passes LIBRARY never does (the charter boundary - the converter
# lives in tools/DispatchProfileSnapshot.h).
if(B2_BUILD_TOOLS)
  add_executable(b2graph tools/b2graph.cpp)
  target_link_libraries(b2graph PRIVATE b2::passes b2::ir b2::rbc b2::interp b2::codegen)
  target_compile_features(b2graph PRIVATE cxx_std_26)
endif()
