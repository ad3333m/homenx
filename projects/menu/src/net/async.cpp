#include "net/async.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "net/http.h"

#include <nxui/core/ThreadPool.hpp>

namespace Async {
namespace {

struct WorkerPool {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::function<void()>> jobs;
    std::vector<std::thread> threads;
    bool stop = false;

    void Start(int n) {
        stop = false;
        for (int i = 0; i < n; i++) {
            threads.emplace_back([this] {
                nxui::moveToWorkerCores(); // HomeNX: keep core 0 for the menu's frames
                while (true) {
                    std::function<void()> job;
                    {
                        std::unique_lock<std::mutex> lk(mu);
                        cv.wait(lk, [this] { return stop || !jobs.empty(); });
                        if (stop) break;
                        job = std::move(jobs.front());
                        jobs.pop_front();
                    }
                    job();
                }
                Http::ThreadCleanup();
            });
        }
    }

    void RequestStop() {
        {
            std::lock_guard<std::mutex> lk(mu);
            stop = true;
            jobs.clear();
        }
        cv.notify_all();
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lk(mu);
            stop = true;
            jobs.clear();
        }
        cv.notify_all();
        for (auto& t : threads) t.join();
        threads.clear();
    }
};

WorkerPool g_api, g_images;
std::mutex g_mainMu;
std::vector<std::function<void()>> g_main;

WorkerPool& Get(Pool p) { return p == Pool::Api ? g_api : g_images; }

}  // namespace

void Start() {
    g_api.Start(2);
    g_images.Start(3);
}

void Stop() {
    g_api.Stop();
    g_images.Stop();
    std::lock_guard<std::mutex> lk(g_mainMu);
    g_main.clear();
}

void RequestStop() {
    g_api.RequestStop();
    g_images.RequestStop();
}

void Run(Pool pool, std::function<void()> work) {
    WorkerPool& p = Get(pool);
    {
        std::lock_guard<std::mutex> lk(p.mu);
        p.jobs.push_back(std::move(work));
    }
    p.cv.notify_one();
}

void Post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_mainMu);
    g_main.push_back(std::move(fn));
}

int Pump() {
    std::vector<std::function<void()>> todo;
    {
        std::lock_guard<std::mutex> lk(g_mainMu);
        todo.swap(g_main);
    }
    for (auto& fn : todo) fn();
    return (int)todo.size();
}

void ClearPending(Pool pool) {
    WorkerPool& p = Get(pool);
    std::lock_guard<std::mutex> lk(p.mu);
    p.jobs.clear();
}

int PendingCount(Pool pool) {
    WorkerPool& p = Get(pool);
    std::lock_guard<std::mutex> lk(p.mu);
    return (int)p.jobs.size();
}

}  // namespace Async
