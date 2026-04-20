/*
    SPDX-FileCopyrightText: 2024 David Redondo <kde@david-redondo>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "eisdevice.h"
#include "keyboard_input.h"

#include <libeis.h>

namespace KWin
{

static std::chrono::microseconds currentTime()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch());
}

EisDevice::EisDevice(eis_device *device, QObject *parent)
    : InputDevice(parent)
    , m_device(device)
{
    eis_device_set_user_data(device, this);
    eis_device_add(device);
}

EisDevice::~EisDevice()
{
    for (const auto button : pressedButtons) {
        Q_EMIT pointerButtonChanged(button, PointerButtonState::Released, currentTime(), this);
    }
    for (const auto key : pressedKeys) {
        Q_EMIT keyChanged(key, KeyboardKeyState::Released, currentTime(), this);
    }
    if (!activeTouches.empty()) {
        Q_EMIT touchCanceled(this);
    }
    eis_device_remove(m_device);
    eis_device_unref(m_device);
}

void EisDevice::changeDevice(eis_device *device)
{
    eis_device_set_user_data(m_device, nullptr);
    eis_device_remove(m_device);
    eis_device_unref(m_device);
    m_device = device;
    eis_device_set_user_data(device, this);
    eis_device_add(device);
    if (m_enabled) {
        eis_device_resume(device);
    }
}

QString EisDevice::name() const
{
    return QString::fromUtf8(eis_device_get_name(m_device));
}

bool EisDevice::isEnabled() const
{
    return m_enabled;
}

void EisDevice::setEnabled(bool enabled)
{
    m_enabled = enabled;
    enabled ? eis_device_resume(m_device) : eis_device_pause(m_device);
}

bool EisDevice::isKeyboard() const
{
    return eis_device_has_capability(m_device, EIS_DEVICE_CAP_KEYBOARD);
}

bool EisDevice::isPointer() const
{
    return eis_device_has_capability(m_device, EIS_DEVICE_CAP_POINTER) || eis_device_has_capability(m_device, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
}

bool EisDevice::isTouchpad() const
{
    return false;
}

bool EisDevice::isTouch() const
{
    return eis_device_has_capability(m_device, EIS_DEVICE_CAP_TOUCH);
}

bool EisDevice::isTabletTool() const
{
    return false;
}

bool EisDevice::isTabletPad() const
{
    return false;
}

bool EisDevice::isTabletModeSwitch() const
{
    return false;
}

bool EisDevice::isLidSwitch() const
{
    return false;
}

void EisDevice::sendKey(uint32_t keyCode, KeyboardKeyState state)
{
    switch (state) {
    case KeyboardKeyState::Pressed:
        if (pressedKeys.contains(keyCode)) {
            return;
        }
        pressedKeys.insert(keyCode);
        break;

    case KeyboardKeyState::Released:
        if (!pressedKeys.remove(keyCode)) {
            return;
        }
        break;
    default:
        return;
    }

    Q_EMIT keyChanged(keyCode, state, currentTime(), this);
}

void EisDevice::sendKeySym(xkb_keysym_t keySym, KeyboardKeyState keyState)
{
    std::optional<Xkb::KeyCode> keyCode = input()->keyboard()->xkb()->keycodeFromKeysym(keySym);
    if (keyCode) {
        // grab the current modifier state, cache it, send our key with our own modifiers at a known state, then reset back
        xkb_state *state = input()->keyboard()->xkb()->state();
        xkb_mod_mask_t formerDepressed = xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED);
        xkb_mod_mask_t formerLatched = xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED);
        xkb_mod_mask_t formerLocked = xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED);
        xkb_layout_index_t formerLayout = xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_EFFECTIVE);

        input()->keyboard()->xkb()->updateModifiers(keyCode->modifiers, 0, 0, 0);
        sendKey(keyCode->keyCode, keyState);

        input()->keyboard()->xkb()->updateModifiers(formerDepressed, formerLatched, formerLocked, formerLayout);
        return;
    }

    // otherwise create a new keymap and send that one key
    // clients don't seem to like having a keymap change whilst a key is pressed
    // for now send a fake release with every press and ignore other releases. We can make the keymap resetting more lazy if it's an issue IRL
    if (keyState == KeyboardKeyState::Pressed) {
        static const uint unmappedKeyCode = 247;
        bool keymapUpdated = input()->keyboard()->xkb()->updateToKeymapForKeySym(unmappedKeyCode, keySym);
        if (!keymapUpdated) {
            return;
        }
        sendKey(unmappedKeyCode, KeyboardKeyState::Pressed);

        for (quint32 key : std::as_const(pressedKeys)) {
            sendKey(key, KeyboardKeyState::Released);
        }
        // reset keyboard back
        input()->keyboard()->xkb()->reconfigure();
    }
}

}
