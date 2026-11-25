#pragma once

#include "ui/menus/menu_base.hpp"
#include "ui/menus/install_stream_menu_base.hpp"
#include "ui/progress_box.hpp"
#include "ui/scrolling_text.hpp"

#include <memory>
#include <string>
#include <vector>

namespace sphaira::ui::menu::mtp {

struct Menu final : MenuBase {
    Menu(u32 flags);
    ~Menu();

    auto GetShortTitle() const -> const char* override { return "MTP"; };
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;

private:
    struct LogEntry {
        std::string name{};
        std::string detail{};
        Result rc{};
        bool ok{};
    };

    bool OnInstallStart(const char* path);
    bool OnInstallWrite(const void* buf, size_t size);
    Result OnInstallClose();

    void CancelActiveInstall();
    void JoinWorker();
    void ScrollLog(s64 delta);

    static void WorkerThreadFunc(void* arg);

private:
    Mutex m_mutex{};
    CondVar m_worker_done_cv{};
    std::unique_ptr<Thread> m_worker_thread{};
    bool m_worker_done{};
    bool m_installing{};
    bool m_boost_active{};
    bool m_screen_toggle_enabled{};
    bool m_was_mtp_enabled{};

    std::shared_ptr<stream::Stream> m_source{};
    std::shared_ptr<ui::ProgressBox> m_pbox{};
    std::string m_current_file{};
    Result m_current_rc{};
    TimeStamp m_install_start_ts{};
    TimeStamp m_poll_ts{};

    std::vector<LogEntry> m_log{};
    size_t m_ok_count{};
    size_t m_fail_count{};
    s64 m_log_scroll{};
    bool m_auto_scroll{true};

    ScrollingText m_scroll_title{};
    ScrollingText m_scroll_transfer{};
};

} // namespace sphaira::ui::menu::mtp
