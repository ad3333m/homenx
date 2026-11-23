#pragma once

#include "service_core.hpp"

#include <switch.h>

namespace switube::phase2::switch_service {

Result initialize(ServiceCore& core);
Result exit();
constexpr int MaxWaiters = 5;
int appendWaiters(Waiter* waiters);
void poll();

}  // namespace switube::phase2::switch_service
