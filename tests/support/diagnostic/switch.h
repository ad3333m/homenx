#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

using u64 = std::uint64_t;
using u32 = std::uint32_t;
using Result = u32;
#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)
inline constexpr int CUR_THREAD_HANDLE = 0;
using Mutex = std::mutex;
struct Thread { std::thread* value{}; };
inline std::atomic<unsigned> debugRecords{};

inline u64 armGetSystemTick() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline u64 armTicksToNs(u64 ticks) { return ticks; }
inline void svcSleepThread(u64 ns) { std::this_thread::sleep_for(std::chrono::nanoseconds(ns)); }
inline void svcGetThreadId(u64* id, int) { *id = 42; }
inline void svcOutputDebugString(const char*, size_t) { ++debugRecords; }
inline void mutexLock(Mutex* lock) { lock->lock(); }
inline void mutexUnlock(Mutex* lock) { lock->unlock(); }
inline Result threadCreate(Thread* thread, void (*run)(void*), void* arg,
                           void*, size_t, int, int) {
    thread->value = new std::thread(run, arg);
    return 0;
}
inline Result threadStart(Thread*) { return 0; }
inline void threadWaitForExit(Thread* thread) { thread->value->join(); }
inline void threadClose(Thread* thread) { delete thread->value; }
