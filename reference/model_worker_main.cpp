// Agent Runtime - reference model worker (independent OS process).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Deterministic reference model backend. It performs no network inference and
// makes no claim about model quality.

#include <cstdio>
#include <cstdlib>
#include <string>

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

int main(int argc, char** argv) {
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
  std::uint64_t generation = 1;
  std::uint64_t fail_every = 0;
  std::uint16_t port = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--generation" && i + 1 < argc) {
      generation = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--port" && i + 1 < argc) {
      port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (argument == "--fail-every" && i + 1 < argc) {
      fail_every = std::strtoull(argv[++i], nullptr, 10);
    } else {
      std::fprintf(stderr, "model worker: unknown argument %s\n", argument.c_str());
      return 2;
    }
  }

  ReferenceModelBackend backend("reference-model-worker", generation);
  SocketSystem::ensure();
  TcpListener listener;
  std::string error;
  if (!listener.listen_loopback(port, error)) {
    std::fprintf(stderr, "model worker: %s\n", error.c_str());
    return 3;
  }
  std::printf("PORT %u\n", static_cast<unsigned>(listener.port()));
  std::fflush(stdout);

  for (;;) {
    std::shared_ptr<TcpConnection> connection = listener.accept(error);
    if (connection == nullptr) {
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
        result.json = std::string("{\"worker\":\"model\",\"generation\":") +
                      std::to_string(generation) + "}";
        reply.payload = encode_query_result(result);
        (void)session.send(reply, error);
        continue;
      }
      if (frame.type != MessageType::MODEL_REQUEST) {
        Frame reply;
        reply.type = MessageType::ERROR_MESSAGE;
        reply.correlation = frame.correlation;
        ErrorMessage message;
        message.code = 1;
        message.detail = "model worker only accepts MODEL_REQUEST";
        reply.payload = encode_error(message);
        (void)session.send(reply, error);
        continue;
      }
      ModelRequest request;
      if (!decode_model_request(frame.payload, request, error)) {
        continue;
      }
      if (fail_every != 0 && backend.invocation_count() % fail_every == fail_every - 1) {
        backend.fail_next(RetryClass::MODEL_UNAVAILABLE, "injected reference model failure");
      }
      ModelResponse response = backend.invoke(request, CancellationProbe());
      Frame reply;
      reply.type = MessageType::MODEL_RESULT;
      reply.correlation = frame.correlation;
      reply.payload = encode_model_response(response);
      if (!session.send(reply, error)) {
        break;
      }
    }
    session.stop();
  }
}
