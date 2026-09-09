# Agent Runtime - reference multiprocess architecture.

add_library(agent_runtime_reference STATIC
  coordinator.cpp
  process.cpp
  ${CMAKE_SOURCE_DIR}/src/distributed/protocol.cpp
  ${CMAKE_SOURCE_DIR}/src/distributed/transport.cpp)
add_library(agent_runtime::reference ALIAS agent_runtime_reference)
target_include_directories(agent_runtime_reference
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(agent_runtime_reference
  PUBLIC agent_runtime
  PRIVATE $<BUILD_INTERFACE:agent_runtime_warnings>)
if(WIN32)
  target_link_libraries(agent_runtime_reference PUBLIC ws2_32)
endif()

# The reference workers are emitted into this directory. Recording it lets the
# launcher find them from any test or consumer binary location.
target_compile_definitions(agent_runtime_reference PRIVATE
  AGENT_RUNTIME_REFERENCE_TOOLS_DIR="${CMAKE_CURRENT_BINARY_DIR}")

add_executable(agent_runtime_tool_worker tool_worker_main.cpp)
target_link_libraries(agent_runtime_tool_worker PRIVATE agent_runtime agent_runtime_reference)

add_executable(agent_runtime_model_worker model_worker_main.cpp)
target_link_libraries(agent_runtime_model_worker PRIVATE agent_runtime agent_runtime_reference)

add_executable(agent_runtime_agent_worker agent_worker_main.cpp)
target_link_libraries(agent_runtime_agent_worker PRIVATE agent_runtime agent_runtime_reference)

add_executable(agent_runtime_coordinator coordinator_main.cpp)
target_link_libraries(agent_runtime_coordinator PRIVATE agent_runtime agent_runtime_reference)

install(TARGETS agent_runtime_reference agent_runtime_tool_worker agent_runtime_model_worker
                agent_runtime_agent_worker agent_runtime_coordinator
  EXPORT agent_runtimeTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
