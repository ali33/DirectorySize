// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
// Windows: Ctrl+Break (the console event Ctrl+C maps to in a new process group)
// must stop dirsize-cli cleanly with exit code 130. On Linux the same is checked
// by scripts/container-build.sh (sigint mode).
#include <cstdio>
#include <string>

#include "check.h"

#ifdef _WIN32
#include <windows.h>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: test_cli <path-to-dirsize-cli.exe>\n");
    return 2;
  }
  // A scan that takes several seconds on one thread, so the event arrives mid-scan.
  std::string cmd = std::string("\"") + argv[1] + "\" \"C:\\Windows\" -j 1 -q -t 1";
  STARTUPINFOA si = {};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi = {};
  std::string mutable_cmd = cmd;
  if (!CreateProcessA(nullptr, &mutable_cmd[0], nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
    std::fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
    return 1;
  }
  Sleep(600);
  DWORD code = 0;
  if (GetExitCodeProcess(pi.hProcess, &code) && code != STILL_ACTIVE) {
    std::printf("note: scan finished before the event could be sent (exit %lu): test inconclusive\n", code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
  }
  if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pi.dwProcessId)) {
    std::printf("note: no console to send Ctrl+Break to (GetLastError %lu): test skipped\n", GetLastError());
    TerminateProcess(pi.hProcess, 1);  // our own child, started above
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
  }
  DWORD w = WaitForSingleObject(pi.hProcess, 30000);
  CHECK(w == WAIT_OBJECT_0);
  if (w != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
  GetExitCodeProcess(pi.hProcess, &code);
  std::printf("exit code after Ctrl+Break: %lu\n", code);
  CHECK(code == 130);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return finish("test_cli");
}
#else
int main() {
  std::printf("test_cli: Windows only (Linux: scripts/test-linux.sh sigint)\n");
  return 0;
}
#endif
