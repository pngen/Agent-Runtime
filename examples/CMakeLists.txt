# Agent Runtime - runnable examples.

add_executable(agent_runtime_example_embedded embedded_runtime.cpp)
target_link_libraries(agent_runtime_example_embedded PRIVATE agent_runtime)

add_executable(agent_runtime_example_safe_retry safe_retry.cpp)
target_link_libraries(agent_runtime_example_safe_retry PRIVATE agent_runtime)

add_executable(agent_runtime_example_ambiguous ambiguous_side_effect.cpp)
target_link_libraries(agent_runtime_example_ambiguous PRIVATE agent_runtime)

add_executable(agent_runtime_example_suspend_resume suspend_resume.cpp)
target_link_libraries(agent_runtime_example_suspend_resume PRIVATE agent_runtime)

if(TARGET agent_runtime_reference)
  add_executable(agent_runtime_example_distributed distributed.cpp)
  target_link_libraries(agent_runtime_example_distributed PRIVATE agent_runtime
    agent_runtime_reference)
  target_compile_definitions(agent_runtime_example_distributed PRIVATE
    AGENT_RUNTIME_REFERENCE_TOOLS_DIR="${CMAKE_BINARY_DIR}/reference")
endif()

install(TARGETS agent_runtime_example_embedded agent_runtime_example_safe_retry
                agent_runtime_example_ambiguous agent_runtime_example_suspend_resume
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
