#pragma once

#include <switch.h>

namespace switchu::smi { enum class SystemMessage { StartMedia }; }
namespace switchu::menu::smi_cmd {
inline Result sendSimple(smi::SystemMessage) {
    ++switube_fake::starts;
    return 0;
}
}
