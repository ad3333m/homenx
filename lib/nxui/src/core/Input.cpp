#include <nxui/core/Input.hpp>
#include <algorithm>
#include <cmath>

namespace nxui {

namespace {

constexpr float kScreenWidth = 1280.f;
constexpr float kScreenHeight = 720.f;
constexpr float kPointerCenterX = kScreenWidth * 0.5f;
constexpr float kPointerCenterY = kScreenHeight * 0.5f;
constexpr float kGyroDeadzone = 0.002f;
constexpr float kNavStickDisableThreshold = 0.30f;

float applyGyroDeadzone(float value) {
    if (std::abs(value) <= kGyroDeadzone)
        return 0.f;

    return value;
}

} // namespace

void Input::initialize() {
    // Every controller drives the menu (players 1-8 and handheld), not only player 1.
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitializeAny(&m_pad);
    for (int i = 0; i < 8; ++i) padInitialize(&m_pads[i], static_cast<HidNpadIdType>(HidNpadIdType_No1 + i));
    padInitialize(&m_pads[8], HidNpadIdType_Handheld);
    // Single Joy-Cons are turned here (readControllers), from what they say as
    // held upright: the system mustn't turn them as well.
    hidSetNpadJoyHoldType(HidNpadJoyHoldType_Vertical);
    hidInitializeTouchScreen();

    recenterVirtualPointer();
    m_prevUpdateTick = armGetSystemTick();

}

void Input::shutdown() {
    stopVirtualPointerSensors();
    m_virtualPointerEnabled = false;
}

void Input::startVirtualPointerSensors() {
    if (m_sixAxisRunning)
        return;

    m_hasHandheldSixAxis = R_SUCCEEDED(hidGetSixAxisSensorHandles(
        &m_handheldSixAxisHandle, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld));
    m_hasFullKeySixAxis = R_SUCCEEDED(hidGetSixAxisSensorHandles(
        &m_fullKeySixAxisHandle, 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey));
    m_hasJoyDualSixAxis = R_SUCCEEDED(hidGetSixAxisSensorHandles(
        m_joyDualSixAxisHandles, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual));

    if (m_hasHandheldSixAxis)
        hidStartSixAxisSensor(m_handheldSixAxisHandle);
    if (m_hasFullKeySixAxis)
        hidStartSixAxisSensor(m_fullKeySixAxisHandle);
    if (m_hasJoyDualSixAxis) {
        hidStartSixAxisSensor(m_joyDualSixAxisHandles[0]);
        hidStartSixAxisSensor(m_joyDualSixAxisHandles[1]);
    }
    m_sixAxisRunning = m_hasHandheldSixAxis || m_hasFullKeySixAxis || m_hasJoyDualSixAxis;
}

void Input::stopVirtualPointerSensors() {
    if (!m_sixAxisRunning)
        return;
    if (m_hasHandheldSixAxis) {
        hidStopSixAxisSensor(m_handheldSixAxisHandle);
        m_hasHandheldSixAxis = false;
    }
    if (m_hasFullKeySixAxis) {
        hidStopSixAxisSensor(m_fullKeySixAxisHandle);
        m_hasFullKeySixAxis = false;
    }
    if (m_hasJoyDualSixAxis) {
        hidStopSixAxisSensor(m_joyDualSixAxisHandles[0]);
        hidStopSixAxisSensor(m_joyDualSixAxisHandles[1]);
        m_hasJoyDualSixAxis = false;
    }
    m_sixAxisRunning = false;
}

void Input::setVirtualPointerEnabled(bool enabled) {
    if (m_virtualPointerEnabled == enabled)
        return;
    m_virtualPointerEnabled = enabled;
    if (enabled && m_pointerMode == PointerMode::Gyro)
        startVirtualPointerSensors();
    else
        stopVirtualPointerSensors();
}

void Input::setPointerMode(PointerMode mode) {
    if (m_pointerMode == mode)
        return;
    setVirtualPointerEnabled(false);
    m_pointerMode = mode;
}

void Input::setVirtualPointerShortcutsEnabled(bool enabled) {
    m_virtualPointerShortcutsEnabled = enabled;
    if (!enabled)
        setVirtualPointerEnabled(false);
}

void Input::recenterVirtualPointer() {
    m_virtualPointerX = kPointerCenterX;
    m_virtualPointerY = kPointerCenterY;
}

bool Input::readActiveSixAxisState(HidSixAxisSensorState& out) const {
    u32 styleSet = padGetStyleSet(&m_pad);
    if ((styleSet & HidNpadStyleTag_NpadHandheld) && m_hasHandheldSixAxis) {
        return hidGetSixAxisSensorStates(m_handheldSixAxisHandle, &out, 1) > 0;
    }

    if ((styleSet & HidNpadStyleTag_NpadFullKey) && m_hasFullKeySixAxis) {
        return hidGetSixAxisSensorStates(m_fullKeySixAxisHandle, &out, 1) > 0;
    }

    if ((styleSet & HidNpadStyleTag_NpadJoyDual) && m_hasJoyDualSixAxis) {
        u32 attributes = padGetAttributes(&m_pad);
        if ((attributes & HidNpadAttribute_IsRightConnected)
            && hidGetSixAxisSensorStates(m_joyDualSixAxisHandles[1], &out, 1) > 0) {
            return true;
        }
        if ((attributes & HidNpadAttribute_IsLeftConnected)
            && hidGetSixAxisSensorStates(m_joyDualSixAxisHandles[0], &out, 1) > 0) {
            return true;
        }
    }

    return false;
}

namespace {

// The stick's direction buttons from where it points (as HID makes them).
uint64_t stickBits(const HidAnalogStickState& s, bool left) {
    constexpr int kThreshold = 0x4000; // (about half way, as HID's own)
    uint64_t b = 0;
    if (s.x < -kThreshold) b |= left ? HidNpadButton_StickLLeft : HidNpadButton_StickRLeft;
    if (s.x > kThreshold) b |= left ? HidNpadButton_StickLRight : HidNpadButton_StickRRight;
    if (s.y > kThreshold) b |= left ? HidNpadButton_StickLUp : HidNpadButton_StickRUp;
    if (s.y < -kThreshold) b |= left ? HidNpadButton_StickLDown : HidNpadButton_StickRDown;
    return b;
}

void map(uint64_t raw, uint64_t& out, uint64_t from, uint64_t to) {
    if (raw & from) out |= to;
}

} // namespace

// Every controller, each as it is held. A single Joy-Con (no grip, no pair) is
// held sideways, rail up, as on the console's own HOME: its stick is turned a
// quarter, its four buttons are A (right) B (bottom) X (top) Y (left), and SL /
// SR are L / R (they scroll). Its stick drives the menu as the left stick does.
void Input::readControllers(uint64_t& held, HidAnalogStickState& ls, HidAnalogStickState& rs) {
    auto bigger = [](const HidAnalogStickState& a, const HidAnalogStickState& b) {
        return std::abs(static_cast<int>(b.x)) + std::abs(static_cast<int>(b.y)) >
               std::abs(static_cast<int>(a.x)) + std::abs(static_cast<int>(a.y));
    };
    for (PadState& pad : m_pads) {
        padUpdate(&pad);
        if (!padIsConnected(&pad)) continue;
        const uint32_t style = padGetStyleSet(&pad);
        const uint64_t raw = padGetButtons(&pad);
        const HidAnalogStickState l = padGetStickPos(&pad, 0), r = padGetStickPos(&pad, 1);
        const bool joyLeft = (style & HidNpadStyleTag_NpadJoyLeft) && !(style & HidNpadStyleTag_NpadJoyDual);
        const bool joyRight = (style & HidNpadStyleTag_NpadJoyRight) && !(style & HidNpadStyleTag_NpadJoyDual);
        if (joyLeft) {
            // Rail up: the stick on the left, the arrows on the right.
            const HidAnalogStickState s{static_cast<s32>(-l.y), static_cast<s32>(l.x)};
            uint64_t b = stickBits(s, true);
            map(raw, b, HidNpadButton_Down, HidNpadButton_A);
            map(raw, b, HidNpadButton_Left, HidNpadButton_B);
            map(raw, b, HidNpadButton_Right, HidNpadButton_X);
            map(raw, b, HidNpadButton_Up, HidNpadButton_Y);
            map(raw, b, HidNpadButton_LeftSL, HidNpadButton_L);
            map(raw, b, HidNpadButton_LeftSR, HidNpadButton_R);
            map(raw, b, HidNpadButton_Minus, HidNpadButton_Minus);
            map(raw, b, HidNpadButton_StickL, HidNpadButton_Plus); // (it has no +: the stick's click)
            held |= b;
            if (bigger(ls, s)) ls = s;
        } else if (joyRight) {
            // Rail up: the stick on the left, the buttons on the right.
            const HidAnalogStickState s{static_cast<s32>(r.y), static_cast<s32>(-r.x)};
            uint64_t b = stickBits(s, true);
            map(raw, b, HidNpadButton_X, HidNpadButton_A);
            map(raw, b, HidNpadButton_A, HidNpadButton_B);
            map(raw, b, HidNpadButton_Y, HidNpadButton_X);
            map(raw, b, HidNpadButton_B, HidNpadButton_Y);
            map(raw, b, HidNpadButton_RightSL, HidNpadButton_L);
            map(raw, b, HidNpadButton_RightSR, HidNpadButton_R);
            map(raw, b, HidNpadButton_Plus, HidNpadButton_Plus);
            map(raw, b, HidNpadButton_StickR, HidNpadButton_Minus); // (it has no -: the stick's click)
            held |= b;
            if (bigger(ls, s)) ls = s;
        } else {
            held |= raw; // (HID's own stick directions, as before)
            if (bigger(ls, l)) ls = l;
            if (bigger(rs, r)) rs = r;
        }
    }
}

void Input::update() {
    uint64_t nowTick = armGetSystemTick();
    float dt = m_prevUpdateTick
        ? static_cast<float>(nowTick - m_prevUpdateTick) / static_cast<float>(armGetSystemTickFreq())
        : (1.f / 60.f);
    m_prevUpdateTick = nowTick;
    if (dt > 0.05f) dt = 1.f / 60.f;

    padUpdate(&m_pad); // (kept for anything that reads it)
    HidAnalogStickState ls{}, rs{};
    uint64_t held = 0;
    readControllers(held, ls, rs);
    m_kHeld = held;
    m_kDown = held & ~m_prevHeld;
    m_kUp   = m_prevHeld & ~held;
    m_prevHeld = held;

    // Analog sticks
    m_lx = ls.x / 32767.f;
    m_ly = ls.y / 32767.f;
    m_rx = rs.x / 32767.f;
    m_ry = rs.y / 32767.f;

    HidTouchScreenState tState = {};
    bool hardwareTouching = hidGetTouchScreenStates(&tState, 1) && tState.count > 0;
    float hardwareTouchX = hardwareTouching ? tState.touches[0].x : m_touchX;
    float hardwareTouchY = hardwareTouching ? tState.touches[0].y : m_touchY;

    // HomeNX: the left stick moves the stick pointer, so while it's on the
    // stick's directions don't reach the menu (or turn the pointer off).
    const bool stickPointer = m_virtualPointerEnabled && m_pointerMode == PointerMode::Stick;
    if (stickPointer) {
        constexpr uint64_t kLeftStick = HidNpadButton_StickLLeft | HidNpadButton_StickLUp
                                      | HidNpadButton_StickLRight | HidNpadButton_StickLDown;
        m_kDown &= ~kLeftStick;
        m_kUp &= ~kLeftStick;
        m_kHeld &= ~kLeftStick;
    }

    bool navInputUsed =
        isDown(Button::DLeft)  || isDown(Button::DRight) ||
        isDown(Button::DUp)    || isDown(Button::DDown)  ||
        isDown(Button::LStickL)|| isDown(Button::LStickR)||
        isDown(Button::LStickU)|| isDown(Button::LStickD)||
        isDown(Button::RStickL)|| isDown(Button::RStickR)||
        isDown(Button::RStickU)|| isDown(Button::RStickD)||
        (!stickPointer && std::abs(m_lx) >= kNavStickDisableThreshold) ||
        (!stickPointer && std::abs(m_ly) >= kNavStickDisableThreshold) ||
        std::abs(m_rx) >= kNavStickDisableThreshold ||
        std::abs(m_ry) >= kNavStickDisableThreshold;

    if (hardwareTouching || navInputUsed) {
        setVirtualPointerEnabled(false);
    }

    const bool pointerShortcuts = m_virtualPointerShortcutsEnabled && m_pointerMode != PointerMode::Off;
    if (pointerShortcuts && isDown(Button::LStick)) {
        setVirtualPointerEnabled(!m_virtualPointerEnabled);
    }
    if (pointerShortcuts && isDown(Button::RStick)) {
        recenterVirtualPointer();
    }

    if (m_virtualPointerEnabled && m_pointerMode == PointerMode::Stick) {
        // Faster the further it's pushed (squared), nothing in the dead zone.
        constexpr float kDeadzone = 0.12f, kSpeed = 1100.f; // px/s at full tilt
        auto curve = [](float v) {
            const float a = (std::abs(v) - kDeadzone) / (1.f - kDeadzone);
            return a <= 0.f ? 0.f : std::copysign(a * a, v);
        };
        m_virtualPointerX = std::clamp(m_virtualPointerX + curve(m_lx) * kSpeed * dt, 0.f, kScreenWidth - 1.f);
        m_virtualPointerY = std::clamp(m_virtualPointerY - curve(m_ly) * kSpeed * dt, 0.f, kScreenHeight - 1.f);
    } else if (m_virtualPointerEnabled) {
        HidSixAxisSensorState sixAxis = {};
        if (readActiveSixAxisState(sixAxis)) {
            float roll = applyGyroDeadzone(sixAxis.angular_velocity.z);
            float pitch = applyGyroDeadzone(sixAxis.angular_velocity.x);

            m_virtualPointerX = std::clamp(
                m_virtualPointerX - roll * m_virtualPointerSensitivity * dt,
                0.f, kScreenWidth - 1.f);
            m_virtualPointerY = std::clamp(
                m_virtualPointerY - pitch * m_virtualPointerSensitivity * dt,
                0.f, kScreenHeight - 1.f);
        }
    }

    m_wasTouching = m_touching;
    if (hardwareTouching) {
        m_touchX = hardwareTouchX;
        m_touchY = hardwareTouchY;
    } else if (m_virtualPointerEnabled) {
        m_touchX = m_virtualPointerX;
        m_touchY = m_virtualPointerY;
    }

    bool virtualTouching = !hardwareTouching
        && m_virtualPointerEnabled
        && (m_kHeld & static_cast<uint64_t>(Button::A));

    m_touching = hardwareTouching || virtualTouching;

    m_touchDown = (m_touching && !m_wasTouching);
    m_touchUp   = (!m_touching && m_wasTouching);

    if (m_touchDown) {
        m_touchStartX = m_touchX;
        m_touchStartY = m_touchY;
        m_touchStartTick = armGetSystemTick();
    }
}

void Input::mergeRightStick() {
    constexpr uint64_t kRight = HidNpadButton_StickRLeft | HidNpadButton_StickRUp
                              | HidNpadButton_StickRRight | HidNpadButton_StickRDown;
    m_kDown |= (m_kDown & kRight) >> 4; // StickR* sit four bits above StickL*
    m_kUp |= (m_kUp & kRight) >> 4;
    m_kHeld |= (m_kHeld & kRight) >> 4;
}

float Input::touchDuration() const {
    uint64_t now = armGetSystemTick();
    return static_cast<float>(now - m_touchStartTick) / static_cast<float>(armGetSystemTickFreq());
}

bool Input::isDown(Button b) const { return m_kDown & static_cast<uint64_t>(b); }
bool Input::isUp(Button b)   const { return m_kUp   & static_cast<uint64_t>(b); }
bool Input::isHeld(Button b) const { return m_kHeld & static_cast<uint64_t>(b); }

bool Input::hasUserActivity() const {
    constexpr float kActivityStickThreshold = 0.08f;
    return m_kDown != 0 || m_kUp != 0 || m_touching || m_touchDown || m_touchUp
        || m_virtualPointerEnabled
        || std::abs(m_lx) >= kActivityStickThreshold
        || std::abs(m_ly) >= kActivityStickThreshold
        || std::abs(m_rx) >= kActivityStickThreshold
        || std::abs(m_ry) >= kActivityStickThreshold;
}

} // namespace nxui
