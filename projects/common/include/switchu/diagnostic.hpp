#pragma once

#include <switch.h>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <sys/stat.h>

namespace switchu::diagnostic {

enum class Level { Error, Warn, Info, Debug, Trace };
#ifndef HOMENX_DIAGNOSTIC_LEVEL
#define HOMENX_DIAGNOSTIC_LEVEL 2
#endif

// Constant initialization: appInit can report errors before C++ constructors.
inline constinit Mutex lock{};
inline constinit Thread worker{};
inline constinit std::atomic<bool> stopping{};
inline constinit std::atomic<bool> started{};
inline constinit unsigned head{}, count{}, dropped{};
inline constinit char queue[48][240]{};
inline constinit u64 origin{};
inline constinit const char* component{};
inline constinit const char* path{};
inline constinit const char* archive{};
inline constinit std::atomic<const char*> stage{};
inline constinit std::atomic<u32> serial{};
inline constinit unsigned timeoutMs{5000};
inline constinit const std::atomic<const char*>* loopStage{};
inline constinit const std::atomic<u32>* loopSerial{};

// Slots are idle while their owner waits intentionally.
struct WorkerProgress {
    std::atomic<u64> since{};
    const char* name;
};
inline constinit WorkerProgress eventProgress{{}, "daemon event dispatch"};
inline constinit WorkerProgress cacheProgress{{}, "daemon control cache"};

struct ScopedProgress {
    WorkerProgress& worker;
    explicit ScopedProgress(WorkerProgress& value) : worker(value) {
        worker.since.store(armGetSystemTick());
    }
    ~ScopedProgress() { worker.since.store(0); }
    ScopedProgress(const ScopedProgress&) = delete;
    ScopedProgress& operator=(const ScopedProgress&) = delete;
};

inline u64 milliseconds() {
    return armTicksToNs(armGetSystemTick() - origin) / 1000000;
}

inline void log(Level level, const char* subsystem, const char* message, Result rc = 0,
                bool showResult = false) {
    if (static_cast<int>(level) > HOMENX_DIAGNOSTIC_LEVEL) return;
    constexpr const char* levels[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
    char line[sizeof(queue[0])]{};
    char result[20]{};
    if (showResult || rc) std::snprintf(result, sizeof(result), " rc=0x%08X", rc);
    u64 tid{};
    svcGetThreadId(&tid, CUR_THREAD_HANDLE);
    std::snprintf(line, sizeof(line), "[%llu ms] [%s] [%s/%s] [T%llu]%s %.140s\n",
                  static_cast<unsigned long long>(milliseconds()),
                  levels[static_cast<int>(level)], component, subsystem,
                  static_cast<unsigned long long>(tid), result, message);
    const auto length = std::strlen(line);
    for (size_t i = 0; i + 1 < length; ++i)
        if (line[i] == '\n' || line[i] == '\r') line[i] = ' ';
    if (!started || level == Level::Error)
        svcOutputDebugString(line, length);

    mutexLock(&lock);
    if (count == 48) {
        head = (head + 1) % 48;
        --count;
        ++dropped;
    }
    std::memcpy(queue[(head + count) % 48], line, sizeof(line));
    ++count;
    mutexUnlock(&lock);
}

inline void begin(const char* subsystem, const char* operation) {
    stage.store(operation);
    ++serial;
    log(Level::Info, subsystem, operation);
}

inline Result end(const char* subsystem, const char* operation, Result rc) {
    log(R_FAILED(rc) ? Level::Error : Level::Info, subsystem, operation, rc, true);
    return rc;
}

inline void run(void*) {
    constexpr size_t limit = 512 * 1024;
    mkdir("sdmc:/config", 0777);
    mkdir("sdmc:/config/SwitchU", 0777);
    FILE* file = nullptr;
    size_t bytes = limit; // Check the existing log before opening it.
    struct stat status{};
    if (stat(path, &status) == 0) bytes = status.st_size;
    else bytes = 0;
    auto previous = milliseconds();
    auto progress = previous;
    u32 lastSerial = serial.load(), lastLoop = loopSerial ? loopSerial->load() : 0;
    bool warned = false;
    u64 warnedAt[2]{};
    u64 resumedAt{};
    WorkerProgress* const workers[] = {&eventProgress, &cacheProgress};
    for (;;) {
        auto now = milliseconds();
        const auto current = serial.load();
        const auto loop = loopSerial ? loopSerial->load() : 0;
        const auto* currentStage = stage.load();
        const auto* detail = currentStage && loopStage && std::strcmp(currentStage, "UI loop") == 0
            ? loopStage->load() : currentStage;
        if (!detail || std::strcmp(detail, "intentional sleep wait") == 0 ||
            current != lastSerial || loop != lastLoop || now - previous > 2000) {
            progress = now;
            warned = false;
        } else if (!warned && now - progress >= timeoutMs) {
            char message[140];
            std::snprintf(message, sizeof(message), "No progress for %llu ms in %s",
                          static_cast<unsigned long long>(now - progress), detail);
            log(Level::Warn, "WATCHDOG", message);
            warned = true;
        }
        if (now - previous > 2000) resumedAt = armGetSystemTick();
        for (unsigned i = 0; i < 2; ++i) {
            const auto since = workers[i]->since.load();
            if (!since || warnedAt[i] == since) continue;
            const auto stamp = since > resumedAt ? since : resumedAt;
            const auto elapsed = armTicksToNs(armGetSystemTick() - stamp) / 1000000;
            if (elapsed >= timeoutMs) {
                char message[136];
                std::snprintf(message, sizeof(message), "No progress for %llu ms in %s",
                              static_cast<unsigned long long>(elapsed), workers[i]->name);
                log(Level::Warn, "WATCHDOG", message);
                warnedAt[i] = since;
            }
        }
        previous = now;
        lastSerial = current;
        lastLoop = loop;

        char batch[8][240];
        for (unsigned batches = 0; batches < 6; ++batches) {
            mutexLock(&lock);
            unsigned n = count < 8 ? count : 8;
            unsigned lost = dropped;
            dropped = 0;
            for (unsigned i = 0; i < n; ++i) {
                std::memcpy(batch[i], queue[head], sizeof(batch[i]));
                head = (head + 1) % 48;
            }
            count -= n;
            mutexUnlock(&lock);
            if (!n) break;
            if (!file || bytes + sizeof(batch) + 80 > limit) {
                if (file) std::fclose(file);
                if (bytes + sizeof(batch) + 80 > limit) {
                    std::remove(archive);
                    std::rename(path, archive);
                    file = std::fopen(path, "w");
                    bytes = 0;
                } else {
                    file = std::fopen(path, "a");
                }
            }
            if (!file) {
                constexpr char error[] = "HomeNX diagnostic log open failed";
                svcOutputDebugString(error, sizeof(error) - 1);
                break;
            }
            if (lost) {
                int written = std::fprintf(file, "[WARN] Dropped %u diagnostic records\n", lost);
                if (written > 0) bytes += written;
            }
            for (unsigned i = 0; i < n; ++i) {
                const auto length = std::strlen(batch[i]);
                bytes += std::fwrite(batch[i], 1, length, file);
            }
            if (std::fflush(file) != 0 || std::ferror(file)) {
                constexpr char error[] = "HomeNX diagnostic log write failed";
                svcOutputDebugString(error, sizeof(error) - 1);
                std::fclose(file);
                file = nullptr;
                break;
            }
        }
        if (stopping.load()) break;
        svcSleepThread(250'000'000);
    }
    if (file) std::fclose(file);
}

inline void start(const char* logPath, const char* oldPath,
                  const std::atomic<const char*>* uiStage = nullptr,
                  const std::atomic<u32>* uiSerial = nullptr) {
    path = logPath;
    archive = oldPath;
    loopStage = uiStage;
    loopSerial = uiSerial;
#ifdef HOMENX_DIAGNOSTIC_TIMEOUT_MS
    timeoutMs = HOMENX_DIAGNOSTIC_TIMEOUT_MS;
#endif
    Result rc = threadCreate(&worker, run, nullptr, nullptr, 0x4000, 0x2c, -2);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&worker);
        if (R_FAILED(rc)) threadClose(&worker);
    }
    if (R_FAILED(rc)) end("THREAD", "Diagnostic worker creation/start failed", rc);
    else started = true;
}

inline void stop() {
    if (!started) return;
    stage.store(nullptr);
    stopping.store(true);
    threadWaitForExit(&worker);
    threadClose(&worker);
    started = false;
}

} // namespace switchu::diagnostic
