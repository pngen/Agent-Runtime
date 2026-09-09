# Agent Runtime - benchmarks.

add_executable(agent_runtime_benchmark benchmark_main.cpp)
target_link_libraries(agent_runtime_benchmark PRIVATE agent_runtime)

install(TARGETS agent_runtime_benchmark
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
