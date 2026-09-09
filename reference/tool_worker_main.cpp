// Agent Runtime - reference tool worker (independent OS process).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// A safe deterministic tool worker. It executes only the built-in reference
// tools; it is not an arbitrary command executor.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agent_runtime/agent_runtime.hpp"
#include "agent_runtime/distributed/protocol.hpp"
#include "agent_runtime/distributed/transport.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace agent_runtime;
using namespace agent_runtime::distributed;

namespace {

struct Options {
  std::uint64_t generation = 1;
  std::string scratch;
  bool crash_after_side_effect = false;
  std::uint16_t port = 0;
  std::string crash_tool;
};

[[nodiscard]] bool parse(int argc, char** argv, Options& options, std::string& error) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--generation" && i + 1 < argc) {
      options.generation = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--port" && i + 1 < argc) {
      options.port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (argument == "--scratch" && i + 1 < argc) {
      options.scratch = argv[++i];
    } else if (argument == "--crash-after-side-effect") {
      options.crash_after_side_effect = true;
    } else if (argument == "--crash-tool" && i + 1 < argc) {
      options.crash_tool = argv[++i];
    } else {
      error = "unknown argument: " + argument;
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool side_effecting(SideEffectClass side_effect) noexcept {
  return side_effect == SideEffectClass::COMMIT_TOKEN_REQUIRED ||
         side_effect == SideEffectClass::NON_REPEATABLE ||
         side_effect == SideEffectClass::IDEMPOTENT ||
         side_effect == SideEffectClass::DEDUPLICATABLE;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
  Options options;
  std::string error;
  if (!parse(argc, argv, options, error)) {
    std::fprintf(stderr, "tool worker: %s\n", error.c_str());
    return 2;
  }

  ReferenceToolBackend backend("reference-tool-worker", options.generation);
  if (!options.scratch.empty()) {
    backend.set_journal_directory(options.scratch);
  }

  SocketSystem::ensure();
  TcpListener listener;
  if (!listener.listen_loopback(options.port, error)) {
    std::fprintf(stderr, "tool worker: %s\n", error.c_str());
    return 3;
  }
  std::printf("PORT %u\n", static_cast<unsigned>(listener.port()));
  std::fflush(stdout);

  for (;;) {
    std::shared_ptr<TcpConnection> connection = listener.accept(error);
    if (connection == nullptr) {
      std::fprintf(stderr, "tool worker: %s\n", error.c_str());
      return 4;
    }
    FrameSession session(connection, 64);
    if (!session.start(error)) {
      continue;
    }
    for (;;) {
      Frame frame;
      if (!session.receive(frame)) {
        break;
      }
      if (frame.type == MessageType::QUERY) {
        QueryMessage query;
        if (!decode_query(frame.payload, query, error)) {
          continue;
        }
        Frame reply;
        reply.type = MessageType::QUERY_RESULT;
        reply.correlation = frame.correlation;
        QueryResultMessage result;
        result.subject = query.subject;
        result.json = std::string("{\"worker\":\"tool\",\"generation\":") +
                      std::to_string(options.generation) +
                      ",\"invocations\":" + std::to_string(backend.invocation_count()) + "}";
        reply.payload = encode_query_result(result);
        (void)session.send(reply, error);
        continue;
      }
      if (frame.type != MessageType::TOOL_REQUEST) {
        Frame reply;
        reply.type = MessageType::ERROR_MESSAGE;
        reply.correlation = frame.correlation;
        ErrorMessage message;
        message.code = 1;
        message.detail = "tool worker only accepts TOOL_REQUEST";
        reply.payload = encode_error(message);
        (void)session.send(reply, error);
        continue;
      }
      ToolRequest request;
      if (!decode_tool_request(frame.payload, request, error)) {
        continue;
      }
      const bool crash_this_call =
          options.crash_after_side_effect &&
          (options.crash_tool.empty() || options.crash_tool == request.tool_name) &&
          side_effecting(request.side_effect);
      if (crash_this_call) {
        backend.crash_after_side_effect_once();
      }
      ToolResponse response = backend.invoke(request, CancellationProbe());
      if (crash_this_call && response.status == CompletionStatus::AMBIGUOUS) {
        // The physical side effect is durable; the worker dies before the
        // response is delivered. This is a real process death, not a flag.
        std::fflush(stdout);
        std::_Exit(70);
      }
      Frame reply;
      reply.type = MessageType::TOOL_RESULT;
      reply.correlation = frame.correlation;
      reply.payload = encode_tool_response(response);
      if (!session.send(reply, error)) {
        break;
      }
    }
    session.stop();
  }
}
