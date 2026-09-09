// Agent Runtime - noninteractive child process launcher for the reference topology.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_REFERENCE_PROCESS_HPP
#define AGENT_RUNTIME_REFERENCE_PROCESS_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace agent_runtime::reference {

/// Spawns and controls a real operating-system process. Child processes are
/// created with no console window, no inherited interactive handles and no
/// error dialog boxes, so a failure never blocks a test run.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  [[nodiscard]] bool spawn(const std::string& executable,
                           const std::vector<std::string>& arguments, std::string& error);
  [[nodiscard]] bool running() const noexcept { return process_handle_ != 0 && !exited_; }
  [[nodiscard]] std::uint32_t process_id() const noexcept { return process_id_; }

  /// Reads one line from the child's stdout, blocking until a newline arrives
  /// or the pipe closes. Used to learn an ephemeral listening port.
  [[nodiscard]] bool read_line(std::string& line, std::string& error);

  /// Waits for the process to exit and reports its exit code.
  [[nodiscard]] bool wait(int& exit_code, std::string& error);

  /// Terminates the process immediately. Returns true when the process is no
  /// longer running.
  [[nodiscard]] bool kill(std::string& error);

  /// Exits the process without any cleanup, modelling a hard crash. Used only
  /// by tests that need a real process death.
  [[nodiscard]] bool terminate_hard(std::string& error) { return kill(error); }

 private:
  void close_handles();

  std::uintptr_t process_handle_ = 0;
  std::uintptr_t thread_handle_ = 0;
  std::uintptr_t stdout_read_ = 0;
  std::uint32_t process_id_ = 0;
  bool exited_ = false;
  std::string partial_line_;
};

/// Returns the directory containing the currently running executable.
[[nodiscard]] std::string executable_directory();

/// Builds the path of a sibling executable produced by the same build.
[[nodiscard]] std::string sibling_executable(const std::string& name);

}  // namespace agent_runtime::reference

#endif  // AGENT_RUNTIME_REFERENCE_PROCESS_HPP
