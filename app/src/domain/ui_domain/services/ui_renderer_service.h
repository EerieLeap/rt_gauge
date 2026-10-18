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
    static constexpr int STACK_SIZE = CONFIG_EERIE_LEAP_UI_RENDERER_THREAD_STACK_SIZE;
    static constexpr int PRIORITY = 8;

    std::unique_ptr<Thread> thread_;

    std::atomic<bool> running_ = false;

    void ThreadEntry() override;

    static void Render();
    static void DisplayInvalidateCb(lv_event_t* e);

public:
    UiRendererService();
    ~UiRendererService() override;
    UiRendererService(const UiRendererService&) = delete;
    UiRendererService& operator=(const UiRendererService&) = delete;
    UiRendererService(UiRendererService&&) = delete;
    UiRendererService& operator=(UiRendererService&&) = delete;

    int Initialize();

    void Start();
    void Stop();
};

} // namespace eerie_leap::domain::ui_domain::services
