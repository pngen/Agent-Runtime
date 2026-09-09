# Agent Runtime - inspection tool.

add_executable(agent_runtime_inspect agent_runtime_inspect.cpp)
target_link_libraries(agent_runtime_inspect PRIVATE agent_runtime)

install(TARGETS agent_runtime_inspect
  EXPORT agent_runtimeTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
