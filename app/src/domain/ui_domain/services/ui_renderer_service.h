#pragma once

#include <atomic>
#include <memory>

#include <lvgl.h>
#include <zephyr/kernel.h>

#include <subsys/threading/thread.h>

namespace eerie_leap::domain::ui_domain::services {

using eerie_leap::subsys::threading::IThread;
using eerie_leap::subsys::threading::Thread;

class UiRendererService : public IThread {
private:
    static constexpr int k_stack_size_ = CONFIG_EERIE_LEAP_UI_RENDERER_THREAD_STACK_SIZE;
    static constexpr int k_priority_ = 8;

    std::unique_ptr<Thread> thread_;

    std::atomic<bool> running_ = false;

    void ThreadEntry() override;

    void Render();
    static void DisplayInvalidateCb(lv_event_t* e);

public:
    UiRendererService();
    ~UiRendererService();

    int Initialize();

    void Start();
    void Stop();
};

} // namespace eerie_leap::domain::ui_domain::services
