#include "pch.h"

#include "App.h"
#include "Config.h"
#include "Log.h"
#include "Settings.h"

#include <DbgHelp.h>

#include <csignal>
#include <exception>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

namespace {

std::filesystem::path g_exeDir;

std::filesystem::path exeDirectory() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(path).parent_path();
}

// Walks and symbolizes the faulting thread's stack (PDB next to the exe).
std::string symbolizedStack(EXCEPTION_POINTERS* info) {
  HANDLE process = GetCurrentProcess();
  HANDLE thread = GetCurrentThread();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  if (!SymInitialize(process, g_exeDir.string().c_str(), TRUE)) {
    return "  (SymInitialize failed)";
  }
  CONTEXT context = *info->ContextRecord;
  STACKFRAME64 frame{};
  frame.AddrPC.Offset = context.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
  std::string out;
  for (int i = 0; i < 48; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr, SymFunctionTableAccess64,
                     SymGetModuleBase64, nullptr) ||
        frame.AddrPC.Offset == 0) {
      break;
    }
    char symbolBuffer[sizeof(SYMBOL_INFO) + 512] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    std::string name = "?";
    if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) {
      name = symbol->Name;
    }
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    char buf[1024];
    if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line)) {
      snprintf(buf, sizeof(buf), "  #%02d %s  (%s:%lu)\n", i, name.c_str(), line.FileName, line.LineNumber);
    } else {
      snprintf(buf, sizeof(buf), "  #%02d %s  [0x%llx]\n", i, name.c_str(),
               static_cast<unsigned long long>(frame.AddrPC.Offset));
    }
    out += buf;
  }
  SymCleanup(process);
  return out;
}

LONG WINAPI crashHandler(EXCEPTION_POINTERS* info) {
  const std::string stack = symbolizedStack(info);
  const std::filesystem::path dumpPath = g_exeDir / "TerrainPOC_crash.dmp";
  HANDLE file = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpWithThreadInfo, &mei, nullptr,
                      nullptr);
    CloseHandle(file);
  }
  SPDLOG_LOGGER_CRITICAL(Log::app(), "CRASH: exception 0x{:08X} at {} (thread {}) - minidump written to {}\n{}",
                         static_cast<unsigned>(info->ExceptionRecord->ExceptionCode),
                         info->ExceptionRecord->ExceptionAddress, GetCurrentThreadId(), dumpPath.string(), stack);
  Log::shutdown();
  return EXCEPTION_EXECUTE_HANDLER;
}

#ifdef _DEBUG
// Debug CRT reports (assert() failures, including Cesium Native's
// CESIUM_ASSERT) go to the log instead of a modal dialog; execution continues
// so a harness run reports every failure.
int __cdecl crtReportHook(int reportType, wchar_t* message, int* returnValue) {
  const char* kind = reportType == _CRT_ASSERT ? "ASSERT" : (reportType == _CRT_ERROR ? "CRT ERROR" : "CRT WARNING");
  std::string text = Log::toUtf8(message ? message : L"");
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  if (reportType == _CRT_WARN) {
    SPDLOG_LOGGER_WARN(Log::app(), "{}: {}", kind, text);
  } else {
    SPDLOG_LOGGER_ERROR(Log::app(), "{}: {}", kind, text);
  }
  if (returnValue) {
    *returnValue = 0; // do not break into the debugger
  }
  return TRUE; // handled
}
#endif

void abortHandler(int) {
  SPDLOG_LOGGER_CRITICAL(Log::app(), "abort() called");
  Log::shutdown();
}

void terminateHandler() {
  std::string what = "unknown";
  if (auto e = std::current_exception()) {
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& ex) {
      what = ex.what();
    } catch (...) {
    }
  }
  SPDLOG_LOGGER_CRITICAL(Log::app(), "std::terminate called: {}", what);
  Log::shutdown();
  std::abort();
}

} // namespace

int main(int argc, char** argv) {
  g_exeDir = exeDirectory();
  SetUnhandledExceptionFilter(crashHandler);
  std::set_terminate(terminateHandler);
  std::signal(SIGABRT, abortHandler);
#ifdef _DEBUG
  _set_error_mode(_OUT_TO_MSGBOX); // route assert() through _CrtDbgReport (and our hook)
  _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL, crtReportHook);
#endif
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  SetConsoleOutputCP(CP_UTF8);

  std::optional<AppConfig> parsed = parseCommandLine(argc, argv);
  if (!parsed) {
    return 2;
  }
  AppConfig cfg = *parsed;
  // Persisted UI settings: the base the command line is applied over. Harness
  // runs stay deterministic (never read, never written) unless a file is named
  // explicitly (--settings file: the settings tests).
  std::string settingsReport = "settings not used (harness run)";
  const bool explicitFile = !cfg.settingsPath.empty();
  if (cfg.settingsPath.empty()) {
    cfg.settingsPath = (g_exeDir / "TerrainPOC.ini").string();
  }
  if (cfg.useSettings && (explicitFile || (cfg.script.empty() && cfg.screenshot.empty()))) {
    AppConfig base;
    Settings::load(cfg.settingsPath, base, settingsReport);
    parsed = parseCommandLine(argc, argv, base);
    if (!parsed) {
      return 2;
    }
    cfg = *parsed;
    cfg.settingsActive = true;
  } else if (!cfg.useSettings) {
    settingsReport = "settings not used (--no-settings)";
  }
  if (cfg.logPath.empty()) {
    cfg.logPath = (g_exeDir / "TerrainPOC.log").string();
  }
  if (cfg.cacheDir.empty()) {
    cfg.cacheDir = (g_exeDir / "cache").string();
  }
  if (cfg.shaderDir.empty()) {
    cfg.shaderDir = (g_exeDir / "shaders").string();
  }

  Log::init(cfg.logPath, cfg.verbose);
  std::string cmdline;
  for (int i = 1; i < argc; ++i) {
    cmdline += std::string(" ") + argv[i];
  }
  LOG_I("TerrainPOC starting.{}", cmdline);
  LOG_I("Log: {} | cache: {} | shaders: {}", cfg.logPath, cfg.useCache ? cfg.cacheDir : "disabled", cfg.shaderDir);
  LOG_I("Settings: {}", settingsReport);

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr)) {
    LOG_E("CoInitializeEx failed: {}", Log::hrToString(hr));
  }

  int rc = 0;
  {
    App app(cfg);
    rc = app.run();
  }
  LOG_I("TerrainPOC exiting with code {} ({} errors, {} warnings logged)", rc, Log::errorCount(),
        Log::warningCount());
  Log::shutdown();
  CoUninitialize();
  return rc;
}
