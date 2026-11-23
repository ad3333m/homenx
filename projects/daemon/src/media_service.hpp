#pragma once

#include "switube_service.hpp"

namespace switchu::daemon::media {

void start();
void stop();
using switube::phase2::switch_service::MaxWaiters;
using switube::phase2::switch_service::appendWaiters;
using switube::phase2::switch_service::poll;

}
