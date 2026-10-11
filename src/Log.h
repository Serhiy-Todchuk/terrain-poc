#pragma once
#include <spdlog/spdlog.h>

#include <atomic>
#include <memory>
#include <string>

// Central logging for the app. Every subsystem logs through a named spdlog
// logger that writes to the log file, the debugger output and stdout. All
// warnings/errors are counted so the harness can report them at exit.
namespace Log {

void init(const std::string& filePath, bool verbose);
void shutdown();

std::shared_ptr<spdlog::logger> app();    // application / harness
std::shared_ptr<spdlog::logger> cesium(); // handed to Cesium Native
std::shared_ptr<spdlog::logger> net();    // HTTP traffic
std::shared_ptr<spdlog::logger> gpu();    // D3D11 / DXGI / D2D
std::shared_ptr<spdlog::logger> data();   // elevation / vector data

int64_t errorCount();
int64_t warningCount();

// Formats an HRESULT as "0x80070005 (Access is denied.)".
std::string hrToString(long hr);
// Formats GetLastError()-style codes.
std::string win32ErrorToString(unsigned long code);

std::string toUtf8(const std::wstring& w);
std::wstring toWide(const std::string& s);

// Logs a failed HRESULT with the failing expression and location.
bool checkHr(long hr, const char* expr, const char* file, int line);

} // namespace Log

#define LOG_I(...) SPDLOG_LOGGER_INFO(::Log::app(), __VA_ARGS__)
#define LOG_W(...) SPDLOG_LOGGER_WARN(::Log::app(), __VA_ARGS__)
#define LOG_E(...) SPDLOG_LOGGER_ERROR(::Log::app(), __VA_ARGS__)
#define LOG_D(...) SPDLOG_LOGGER_DEBUG(::Log::app(), __VA_ARGS__)

// Evaluates an HRESULT-returning expression, logs on failure, yields true on
// success.
#define HR_OK(expr) ::Log::checkHr(static_cast<long>(expr), #expr, __FILE__, __LINE__)
