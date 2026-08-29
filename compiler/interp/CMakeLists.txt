# B-2 interpreter: Tier 0 direct-threaded RBC executor + v0 reference runtime.
# WHY: T0 is the universal correctness fallback (Rule 96) and the deopt target
# of every tier (Rule 4). It links b2::rbc (the bytecode contract) and nothing
# else; the future real runtime replaces the reference services in this
# library behind the same headers (Runtime.h seam).
add_library(b2_interp STATIC
  src/Heap.cpp
  src/Runtime.cpp
  src/Interp.cpp
  src/Frame.cpp
)

add_library(b2::interp ALIAS b2_interp)

target_include_directories(b2_interp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)

target_compile_features(b2_interp PUBLIC cxx_std_26)

target_link_libraries(b2_interp PUBLIC b2::rbc)

if(B2_BUILD_TOOLS)
  add_executable(b2run tools/b2run.cpp)
  target_link_libraries(b2run PRIVATE b2::interp)
endif()
