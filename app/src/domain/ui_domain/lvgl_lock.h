#pragma once

#include <zephyr/kernel.h>

namespace eerie_leap::domain::ui_domain {

// Serializes all access to LVGL.
//
// LVGL's object/animation/event data structures are not thread-safe. In this
// app, lv_timer_handler() runs on the UI renderer thread while event bus
// subscribers touch LVGL objects and animations (see indicator/icon widgets) on
// a bus worker thread. Every thread that can call into LVGL must hold this lock
// for the duration of that call, or LVGL's internal lists can be corrupted by
// concurrent access. The bus does not take it for you: each subscriber does.
class LvglLock {
private:
    k_mutex lock_{};

    LvglLock();
    ~LvglLock() = default;

public:
    LvglLock(const LvglLock&) = delete;
    LvglLock& operator=(const LvglLock&) = delete;

    static LvglLock& GetInstance();

    void Lock();
    void Unlock();
};

class ScopedLvglLock {
public:
    ScopedLvglLock() {
        LvglLock::GetInstance().Lock();
    }
    ~ScopedLvglLock() {
        LvglLock::GetInstance().Unlock();
    }

    ScopedLvglLock(const ScopedLvglLock&) = delete;
    ScopedLvglLock& operator=(const ScopedLvglLock&) = delete;
};

} // namespace eerie_leap::domain::ui_domain
