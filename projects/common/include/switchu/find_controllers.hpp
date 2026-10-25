#pragma once
#include <switch.h>

namespace switchu {

// Three short rumbles on every connected controller, like the system's "Find
// Controllers" (HomeNX's Settings and CardsNX's).
inline void findControllers() {
    static const HidNpadIdType ids[] = {
        HidNpadIdType_Handheld, HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3,
        HidNpadIdType_No4, HidNpadIdType_No5, HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8,
    };
    HidVibrationDeviceHandle handles[18];
    int count = 0;
    for (HidNpadIdType id : ids) {
        const u32 styles = hidGetNpadStyleSet(id);
        HidNpadStyleTag style;
        int n = 2;
        if (styles & HidNpadStyleTag_NpadHandheld) style = HidNpadStyleTag_NpadHandheld;
        else if (styles & HidNpadStyleTag_NpadFullKey) style = HidNpadStyleTag_NpadFullKey;
        else if (styles & HidNpadStyleTag_NpadJoyDual) style = HidNpadStyleTag_NpadJoyDual;
        else if (styles & HidNpadStyleTag_NpadJoyLeft) { style = HidNpadStyleTag_NpadJoyLeft; n = 1; }
        else if (styles & HidNpadStyleTag_NpadJoyRight) { style = HidNpadStyleTag_NpadJoyRight; n = 1; }
        else continue;
        if (R_SUCCEEDED(hidInitializeVibrationDevices(handles + count, n, id, style)))
            count += n;
    }
    if (count == 0) return;
    HidVibrationValue on[18], off[18];
    for (int i = 0; i < count; ++i) {
        on[i] = {0.7f, 160.f, 0.7f, 320.f};
        off[i] = {0.f, 160.f, 0.f, 320.f};
    }
    // ponytail: blocks the caller ~1 s (CardsNX runs it on a worker); fine for a button pressed on purpose.
    for (int pulse = 0; pulse < 3; ++pulse) {
        hidSendVibrationValues(handles, on, count);
        svcSleepThread(220'000'000ull);
        hidSendVibrationValues(handles, off, count);
        svcSleepThread(140'000'000ull);
    }
}

} // namespace switchu
