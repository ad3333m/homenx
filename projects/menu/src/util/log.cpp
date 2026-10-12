#include "util/log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace Log {
namespace {
std::mutex g_mu;
FILE* g_file = nullptr;
}  // namespace

void Init(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) fclose(g_file);
    // Keep only the current session: the previous one is renamed to .old.
    std::string old = path + ".old";
    std::remove(old.c_str());
    std::rename(path.c_str(), old.c_str());
    g_file = fopen(path.c_str(), "w");
}

void Close() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) fclose(g_file);
    g_file = nullptr;
}

void Write(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    time_t t = time(nullptr);
    struct tm tmv {};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    std::lock_guard<std::mutex> lk(g_mu);
    fprintf(stderr, "[%02d:%02d:%02d] %s\n", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, buf);
    if (g_file) {
        fprintf(g_file, "[%02d:%02d:%02d] %s\n", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, buf);
        fflush(g_file);
    }
}

}  // namespace Log
