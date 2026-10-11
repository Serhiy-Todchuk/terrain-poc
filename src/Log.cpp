#include "pch.h"
#include "Log.h"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace Log {
namespace {

std::atomic<int64_t> g_errors{0};
std::atomic<int64_t> g_warnings{0};

// Counts warnings and errors from every logger (including Cesium Native's).
class CountingSink final : public spdlog::sinks::base_sink<std::mutex> {
protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    if (msg.level >= spdlog::level::err) {
      ++g_errors;
    } else if (msg.level == spdlog::level::warn) {
      ++g_warnings;
    }
  }
  void flush_() override {}
};

std::vector<spdlog::sink_ptr> g_sinks;
std::shared_ptr<spdlog::logger> g_app, g_cesium, g_net, g_gpu, g_data;

std::shared_ptr<spdlog::logger> makeLogger(const char* name, spdlog::level::level_enum level) {
  auto logger = std::make_shared<spdlog::logger>(name, g_sinks.begin(), g_sinks.end());
  logger->set_level(level);
  logger->flush_on(spdlog::level::warn);
  spdlog::register_logger(logger);
  return logger;
}

std::shared_ptr<spdlog::logger> fallback() {
  static std::shared_ptr<spdlog::logger> logger = spdlog::default_logger();
  return logger;
}

} // namespace

void init(const std::string& filePath, bool verbose) {
  auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filePath, true);
  fileSink->set_level(spdlog::level::trace);
  auto msvcSink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
  msvcSink->set_level(spdlog::level::info);
  auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  consoleSink->set_level(verbose ? spdlog::level::debug : spdlog::level::info);
  auto countingSink = std::make_shared<CountingSink>();
  countingSink->set_level(spdlog::level::warn);

  g_sinks = {fileSink, msvcSink, consoleSink, countingSink};
  for (auto& s : g_sinks) {
    s->set_pattern("[%H:%M:%S.%e] [%-6n] [%^%L%$] [t%5t] %v");
  }

  const auto level = verbose ? spdlog::level::debug : spdlog::level::info;
  g_app = makeLogger("app", level);
  g_cesium = makeLogger("cesium", level);
  g_net = makeLogger("net", level);
  g_gpu = makeLogger("d3d", level);
  g_data = makeLogger("data", level);
  spdlog::set_default_logger(g_app);
  spdlog::flush_every(std::chrono::seconds(1));
}

void shutdown() {
  if (g_app) {
    g_app->flush();
  }
  spdlog::shutdown();
}

std::shared_ptr<spdlog::logger> app() { return g_app ? g_app : fallback(); }
std::shared_ptr<spdlog::logger> cesium() { return g_cesium ? g_cesium : fallback(); }
std::shared_ptr<spdlog::logger> net() { return g_net ? g_net : fallback(); }
std::shared_ptr<spdlog::logger> gpu() { return g_gpu ? g_gpu : fallback(); }
std::shared_ptr<spdlog::logger> data() { return g_data ? g_data : fallback(); }

int64_t errorCount() { return g_errors.load(); }
int64_t warningCount() { return g_warnings.load(); }

std::string win32ErrorToString(unsigned long code) {
  wchar_t* buffer = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS;
  DWORD len = FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  if (len == 0) {
    // WinHTTP errors live in winhttp.dll's message table.
    HMODULE winhttp = GetModuleHandleW(L"winhttp.dll");
    if (winhttp) {
      len = FormatMessageW(
          FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
          winhttp, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    }
  }
  std::string text;
  if (len > 0 && buffer) {
    text = toUtf8(std::wstring(buffer, len));
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
      text.pop_back();
    }
  } else {
    text = "unknown error";
  }
  if (buffer) {
    LocalFree(buffer);
  }
  return text;
}

std::string hrToString(long hr) {
  char head[32];
  snprintf(head, sizeof(head), "0x%08lX", static_cast<unsigned long>(hr));
  return std::string(head) + " (" + win32ErrorToString(static_cast<unsigned long>(hr)) + ")";
}

bool checkHr(long hr, const char* expr, const char* file, int line) {
  if (SUCCEEDED(hr)) {
    return true;
  }
  const char* shortFile = strrchr(file, '\\');
  shortFile = shortFile ? shortFile + 1 : file;
  SPDLOG_LOGGER_ERROR(gpu(), "{} failed: {} at {}:{}", expr, hrToString(hr), shortFile, line);
  return false;
}

std::string toUtf8(const std::wstring& w) {
  if (w.empty()) {
    return {};
  }
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring toWide(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

} // namespace Log
