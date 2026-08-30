# B-2 interpreter: Tier 0 direct-threaded RBC executor + v0 reference runtime.
# WHY: T0 is the universal correctness fallback (Rule 96) and the deopt target
# of every tier (Rule 4). It links b2::rbc (the bytecode contract) and nothing
# else; the future real runtime replaces the reference services in this
# library behind the same headers (Runtime.h seam).
#
# Dispatch form (interp_contract.md SS4): B2_INTERP_COMPUTED_GOTO selects the
# v1 computed-goto core (labels-as-values) over the portable switch core -
# per-handler-tail dispatch, the threaded-code win. The extension needs
# gnu++26, so the whole LIBRARY builds as gnu++26 with -Wno-pedantic (CMake
# source-file properties are global per path, which would leak into the
# portable differential build; the target-level form does not). Only
# Interp.cpp uses extensions; the strict -Wpedantic check for every TU of
# this library lives on in b2_interp_portable below, which compiles the same
# sources strict-c++26. MSVC/other compilers: the option self-disables (no
# labels-as-values); the portable core is the architecturally-required form
# anyway (Amendment B.1: minimal dispatch overhead, not a particular
# compiler extension).
option(B2_INTERP_COMPUTED_GOTO
       "T0 dispatch via computed goto (labels-as-values; GNU/Clang only)" ON)

set(B2_INTERP_GOTO_SUPPORTED OFF)
if(B2_INTERP_COMPUTED_GOTO)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(B2_INTERP_GOTO_SUPPORTED ON)
  else()
    message(STATUS
        "b2_interp: computed goto unavailable on ${CMAKE_CXX_COMPILER_ID}; "
        "building the portable switch core")
  endif()
endif()

set(B2_INTERP_SOURCES
  src/Heap.cpp
  src/Runtime.cpp
  src/Interp.cpp
  src/Frame.cpp
)

add_library(b2_interp STATIC ${B2_INTERP_SOURCES})
add_library(b2::interp ALIAS b2_interp)

target_include_directories(b2_interp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)

target_compile_features(b2_interp PUBLIC cxx_std_26)

target_link_libraries(b2_interp PUBLIC b2::rbc)

if(B2_INTERP_GOTO_SUPPORTED)
  target_compile_definitions(b2_interp PRIVATE B2_INTERP_COMPUTED_GOTO=1)
  # gnu++26 (CXX_EXTENSIONS replaces the strict -std=c++26) and -Wno-pedantic
  # for exactly the GNU constructs (labels-as-values, designated dispatch
  # table initializers). -Wall -Wextra -Wshadow from the top level stay on.
  set_target_properties(b2_interp PROPERTIES CXX_EXTENSIONS ON)
  target_compile_options(b2_interp PRIVATE -Wno-pedantic)
endif()

# The portable switch core, ALWAYS built strict (Law 36 differential twin of
# the library above + the -Wswitch exhaustiveness proof for the shared
# handler bodies). tests/interp links it for the corpus differential test;
# b2bench-portable links it for the dispatch A/B measurement. Compiling the
# same sources twice is deliberate: the two dispatchers cannot coexist in
# one binary (identical symbols), so the differential is inter-process
# against the same goldens.
add_library(b2_interp_portable STATIC ${B2_INTERP_SOURCES})
add_library(b2::interp::portable ALIAS b2_interp_portable)
target_include_directories(b2_interp_portable PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)
target_compile_features(b2_interp_portable PUBLIC cxx_std_26)
target_link_libraries(b2_interp_portable PUBLIC b2::rbc)
target_compile_definitions(b2_interp_portable PRIVATE
  B2_INTERP_COMPUTED_GOTO=0)

if(B2_BUILD_TOOLS)
  add_executable(b2run tools/b2run.cpp)
  target_link_libraries(b2run PRIVATE b2::interp)

  # The dispatch micro-benchmark twins (MSG-20260830-005): same source, the
  # two dispatch forms. B2_BENCH_DISPATCH_NAME only labels the output.
  add_executable(b2bench tools/b2bench.cpp)
  target_link_libraries(b2bench PRIVATE b2::interp)
  target_compile_definitions(b2bench PRIVATE
    B2_BENCH_DISPATCH_NAME="computed-goto")

  add_executable(b2bench-portable tools/b2bench.cpp)
  target_link_libraries(b2bench-portable PRIVATE b2_interp_portable)
  target_compile_definitions(b2bench-portable PRIVATE
    B2_BENCH_DISPATCH_NAME="portable-switch")
endif()
