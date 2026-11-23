#pragma once

#include <functional>
#include <pthread.h>
#include "NXConsoleLog.h"

namespace switube {

inline void launchWorkerWithStack(std::function<void()> work, size_t stackSize = 512 * 1024) {
  struct Context {
    std::function<void()> work;
  };
  auto* ctx = new Context{std::move(work)};

  pthread_t pt;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stackSize);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  int err = pthread_create(&pt, &attr, [](void* arg) -> void* {
    auto* c = static_cast<Context*>(arg);
    try {
      c->work();
    } catch (const std::exception& e) {
      nxlog::pushf("Thread exception: %s", e.what());
    } catch (...) {
      nxlog::push("Thread unknown exception");
    }
    delete c;
    return nullptr;
  }, ctx);

  pthread_attr_destroy(&attr);
  if (err != 0) {
    nxlog::pushf("pthread_create failed: %d, executing inline", err);
    try {
      ctx->work();
    } catch (...) {}
    delete ctx;
  }
}

} // namespace switube
