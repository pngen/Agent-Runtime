// Agent Runtime - test entry point.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <cstdlib>

#include "test_framework.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <crtdbg.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
  // Noninteractive execution: never raise a dialog box, never wait for input.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_ASSERT, 0);
  _CrtSetReportMode(_CRT_ERROR, 0);
#endif
  return artest::run_all(argc, argv);
}
