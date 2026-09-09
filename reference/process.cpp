// Agent Runtime - child process launcher implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "process.hpp"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace agent_runtime::reference {
namespace {

#ifdef _WIN32
[[nodiscard]] std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                       nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

[[nodiscard]] std::string narrow(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string narrow_text(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), narrow_text.data(),
                      size, nullptr, nullptr);
  return narrow_text;
}

[[nodiscard]] std::wstring quote(const std::wstring& value) {
  std::wstring out = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t character : value) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
      continue;
    }
    out.append(backslashes, L'\\');
    backslashes = 0;
    out.push_back(character);
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}
#endif

}  // namespace

std::string executable_directory() {
#ifdef _WIN32
  std::wstring buffer(MAX_PATH, L'\0');
  DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  while (size == buffer.size()) {
    buffer.resize(buffer.size() * 2);
    size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  }
  buffer.resize(size);
  const std::size_t separator = buffer.find_last_of(L"\\/");
  const std::wstring directory =
      separator == std::wstring::npos ? buffer : buffer.substr(0, separator);
  return narrow(directory);
#else
  return ".";
#endif
}

std::string sibling_executable(const std::string& name) {
#ifdef _WIN32
  // The build may place a test binary in a different subdirectory from the
  // reference workers; the configured tools directory is used when present.
#ifdef AGENT_RUNTIME_REFERENCE_TOOLS_DIR
  return std::string(AGENT_RUNTIME_REFERENCE_TOOLS_DIR) + "\\" + name + ".exe";
#else
  return executable_directory() + "\\" + name + ".exe";
#endif
#else
  return executable_directory() + "/" + name;
#endif
}

ChildProcess::~ChildProcess() {
  if (running()) {
    std::string ignored;
    (void)kill(ignored);
  }
  close_handles();
}

bool ChildProcess::spawn(const std::string& executable,
                         const std::vector<std::string>& arguments, std::string& error) {
  if (process_handle_ != 0) {
    error = "process was already spawned";
    return false;
  }
#ifdef _WIN32
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &attributes, 0)) {
    error = "CreatePipe failed";
    return false;
  }
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  HANDLE null_output = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &attributes,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

  std::wstring command_line = quote(widen(executable));
  for (const std::string& argument : arguments) {
    command_line += L' ';
    command_line += quote(widen(argument));
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_pipe;
  startup.hStdError = null_output == INVALID_HANDLE_VALUE ? write_pipe : null_output;
  startup.hStdInput = nullptr;

  PROCESS_INFORMATION information{};
  const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
  std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back(L'\0');
  const BOOL created =
      CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, flags, nullptr,
                     nullptr, &startup, &information);
  CloseHandle(write_pipe);
  if (null_output != INVALID_HANDLE_VALUE) {
    CloseHandle(null_output);
  }
  if (!created) {
    CloseHandle(read_pipe);
    error = "CreateProcess failed with windows error " + std::to_string(GetLastError());
    return false;
  }
  process_handle_ = reinterpret_cast<std::uintptr_t>(information.hProcess);
  thread_handle_ = reinterpret_cast<std::uintptr_t>(information.hThread);
  stdout_read_ = reinterpret_cast<std::uintptr_t>(read_pipe);
  process_id_ = information.dwProcessId;
  return true;
#else
  (void)executable;
  (void)arguments;
  error = "child process launch is implemented for Windows in this build";
  return false;
#endif
}

bool ChildProcess::read_line(std::string& line, std::string& error) {
#ifdef _WIN32
  if (stdout_read_ == 0) {
    error = "stdout pipe is not available";
    return false;
  }
  HANDLE pipe = reinterpret_cast<HANDLE>(stdout_read_);
  for (;;) {
    const std::size_t separator = partial_line_.find('\n');
    if (separator != std::string::npos) {
      line = partial_line_.substr(0, separator);
      partial_line_.erase(0, separator + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return true;
    }
    char buffer[512];
    DWORD read = 0;
    if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
      if (!partial_line_.empty()) {
        line = partial_line_;
        partial_line_.clear();
        return true;
      }
      error = "child stdout closed";
      return false;
    }
    partial_line_.append(buffer, read);
  }
#else
  (void)line;
  error = "not implemented";
  return false;
#endif
}

bool ChildProcess::wait(int& exit_code, std::string& error) {
#ifdef _WIN32
  if (process_handle_ == 0) {
    error = "process was never spawned";
    return false;
  }
  HANDLE process = reinterpret_cast<HANDLE>(process_handle_);
  const DWORD status = WaitForSingleObject(process, INFINITE);
  if (status != WAIT_OBJECT_0) {
    error = "WaitForSingleObject failed";
    return false;
  }
  DWORD code = 0;
  if (!GetExitCodeProcess(process, &code)) {
    error = "GetExitCodeProcess failed";
    return false;
  }
  exit_code = static_cast<int>(code);
  exited_ = true;
  return true;
#else
  (void)exit_code;
  error = "not implemented";
  return false;
#endif
}

bool ChildProcess::kill(std::string& error) {
#ifdef _WIN32
  if (process_handle_ == 0) {
    error = "process was never spawned";
    return false;
  }
  HANDLE process = reinterpret_cast<HANDLE>(process_handle_);
  if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
    exited_ = true;
    return true;
  }
  if (!TerminateProcess(process, 9)) {
    const DWORD failure = GetLastError();
    if (failure == ERROR_ACCESS_DENIED) {
      // The process finished between the check above and the termination
      // request. That is a successful kill: wait for it to settle.
      exited_ = WaitForSingleObject(process, INFINITE) == WAIT_OBJECT_0;
      if (exited_) {
        return true;
      }
    }
    error = "TerminateProcess failed with windows error " + std::to_string(failure);
    return false;
  }
  const DWORD status = WaitForSingleObject(process, INFINITE);
  exited_ = status == WAIT_OBJECT_0;
  if (!exited_) {
    error = "process did not terminate";
    return false;
  }
  return true;
#else
  error = "not implemented";
  return false;
#endif
}

void ChildProcess::close_handles() {
#ifdef _WIN32
  if (process_handle_ != 0) {
    CloseHandle(reinterpret_cast<HANDLE>(process_handle_));
    process_handle_ = 0;
  }
  if (thread_handle_ != 0) {
    CloseHandle(reinterpret_cast<HANDLE>(thread_handle_));
    thread_handle_ = 0;
  }
  if (stdout_read_ != 0) {
    CloseHandle(reinterpret_cast<HANDLE>(stdout_read_));
    stdout_read_ = 0;
  }
#endif
}

}  // namespace agent_runtime::reference
