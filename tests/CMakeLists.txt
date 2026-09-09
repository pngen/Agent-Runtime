# Agent Runtime - test suite.

add_library(agent_runtime_test_support STATIC support/test_framework.cpp)
target_include_directories(agent_runtime_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(agent_runtime_test_support PUBLIC agent_runtime
  PRIVATE $<BUILD_INTERFACE:agent_runtime_warnings>)

function(agent_runtime_add_test target)
  add_executable(${target} ${ARGN} support/test_main.cpp)
  target_link_libraries(${target} PRIVATE agent_runtime agent_runtime_test_support)
  if(AGENT_RUNTIME_ENABLE_ASAN AND MSVC)
    target_compile_options(${target} PRIVATE /fsanitize=address)
    target_link_options(${target} PRIVATE /INCREMENTAL:NO)
  endif()
  add_test(NAME ${target} COMMAND ${target})
endfunction()

agent_runtime_add_test(ar_test_core test_core.cpp)
agent_runtime_add_test(ar_test_persistence test_persistence.cpp)
agent_runtime_add_test(ar_test_backends test_backends.cpp)
agent_runtime_add_test(ar_test_lifetime test_lifetime.cpp)
agent_runtime_add_test(ar_test_property test_property.cpp)
agent_runtime_add_test(ar_test_adversarial test_adversarial.cpp)
agent_runtime_add_test(ar_test_races test_races.cpp)
agent_runtime_add_test(ar_test_shutdown test_shutdown.cpp)

if(TARGET agent_runtime_reference)
  agent_runtime_add_test(ar_test_protocol test_protocol.cpp)
  target_link_libraries(ar_test_protocol PRIVATE agent_runtime_reference)
  agent_runtime_add_test(ar_test_distributed test_distributed.cpp)
  target_link_libraries(ar_test_distributed PRIVATE agent_runtime_reference)
endif()

if(AGENT_RUNTIME_CUDA_FOUND)
  agent_runtime_add_test(ar_test_cuda test_cuda.cpp)
  target_link_libraries(ar_test_cuda PRIVATE agent_runtime_cuda_reference)
endif()
