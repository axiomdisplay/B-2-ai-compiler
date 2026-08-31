# B-2 IR: the T2/T3 sea-of-nodes core (Amendment B.1).
# WHY: the IR library is deliberately dependency-free - it must not link RBC,
# the frontend, or any tier: it only REPRESENTS (the IR team charter's
# representation-not-optimization boundary). The RBC->graph builder that
# feeds it lives with the Passes Team (compiler/passes), and the pass
# pipeline + T2 backend consume both.
#
# Laws honored here: arena storage (Rule 7), NodeId edges (Rule 15), no
# strings (Rule 16), SmallVector use lists (Rule 19), deterministic ids and
# serialization (Rule 124), -fno-exceptions discipline in the IR core (the
# verifier/serializer return result types, never throw - Rule 6).

set(B2_IR_SOURCES
  core/NodeInfo.cpp
  core/Graph.cpp
  core/Verifier.cpp
  core/Printer.cpp
  core/Serialize.cpp
)

add_library(b2_ir STATIC ${B2_IR_SOURCES})
add_library(b2::ir ALIAS b2_ir)

target_include_directories(b2_ir PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)

target_compile_features(b2_ir PUBLIC cxx_std_26)
