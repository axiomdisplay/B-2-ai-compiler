# B-2 baseline: Tier 1 no-IR stencil-plan JIT (selection + plan, pre-machine-code).
# WHY: T1 removes interpreter dispatch overhead without building IR (Amendment
# A). v0 delivers the plan stage of docs/stencils.md SS6-SS7: RBC -> StencilPlan
# against the target-neutral v0 StencilSet manifest. Instantiation (copy, patch,
# link, W^X publish) lands with the codegen team's stencil archive. Links
# b2::rbc (bytecode + verifier) and nothing else; the interpreter is a peer
# consumer of RBC, not a dependency of T1 planning.
add_library(b2_baseline STATIC
  src/StencilSet.cpp
  src/PlanBuilder.cpp
  src/Dump.cpp
)

add_library(b2::baseline ALIAS b2_baseline)

target_include_directories(b2_baseline PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)

target_compile_features(b2_baseline PUBLIC cxx_std_26)

target_link_libraries(b2_baseline PUBLIC b2::rbc)

if(B2_BUILD_TOOLS)
  add_executable(b2plan tools/b2plan.cpp)
  target_link_libraries(b2plan PRIVATE b2::baseline)
endif()
