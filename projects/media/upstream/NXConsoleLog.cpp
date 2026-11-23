#include "NXConsoleLog.h"

#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <malloc.h>
#include <mutex>
#include <thread>

namespace {

std::mutex logMutex;
std::deque<std::string> logLines;
FILE* logFile = nullptr;
std::string logPath;
size_t logFileBytes = 0;
size_t logFileLimit = 0;
bool queueEnabled = true;
constexpr size_t MAX_QUEUED = 2048;
const auto logEpoch = std::chrono::steady_clock::now();

}  // namespace

namespace nxlog {

void setQueueEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(logMutex);
  queueEnabled = enabled;
  if (!queueEnabled)
    logLines.clear();
}

void push(const std::string& line) {
  const auto now = std::chrono::steady_clock::now();
  const auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - logEpoch).count();
  char prefix[32];
  snprintf(prefix, sizeof(prefix), "[%6lld.%03lld] ",
           static_cast<long long>(elapsedMs / 1000),
           static_cast<long long>(elapsedMs % 1000));
  const std::string decorated = std::string(prefix) + line;

  std::lock_guard<std::mutex> lock(logMutex);
  if (queueEnabled) {
    if (logLines.size() >= MAX_QUEUED)
      logLines.pop_front();
    logLines.push_back(decorated);
  }
  if (logFile) {
    const size_t bytes = decorated.size() + 1;
    if (logFileLimit != 0 && logFileBytes + bytes > logFileLimit &&
        !logPath.empty()) {
      fclose(logFile);
      logFile = nullptr;
      const std::string previous = logPath + ".previous";
      std::remove(previous.c_str());
      std::rename(logPath.c_str(), previous.c_str());
      logFile = fopen(logPath.c_str(), "w");
      logFileBytes = 0;
    }
    if (!logFile) return;
    fputs(decorated.c_str(), logFile);
    fputc('\n', logFile);
    fflush(logFile);
    logFileBytes += bytes;
  }
}

void pushf(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  push(buf);
}

bool drain() {
  std::deque<std::string> pending;
  {
    std::lock_guard<std::mutex> lock(logMutex);
    pending.swap(logLines);
  }
  for (auto& line : pending) {
    printf("%s\n", line.c_str());
  }
  return !pending.empty();
}

void installBellLogger() {
  // No-op en Switube
}

void setLogFile(const char* path, size_t maxBytes) {
  std::lock_guard<std::mutex> lock(logMutex);
  if (logFile)
    fclose(logFile);
  const std::string previous = std::string(path) + ".previous";
  std::remove(previous.c_str());
  std::rename(path, previous.c_str());

  logPath = path ? path : "";
  logFileLimit = maxBytes;
  logFileBytes = 0;
  logFile = fopen(path, "w");
}

void pushHeapUsage(const char* tag) {
#if defined(__SWITCH__)
  struct mallinfo info = mallinfo();
  pushf("Heap [%s]: uordblks=%zu fordblks=%zu arena=%zu",
        tag ? tag : "unknown",
        static_cast<size_t>(info.uordblks),
        static_cast<size_t>(info.fordblks),
        static_cast<size_t>(info.arena));
#else
  (void)tag;
#endif
}

}  // namespace nxlog
