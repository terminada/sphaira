#include "ui/menus/mtp_menu.hpp"
#include "usb/usbds.hpp"
#include "app.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "ui/nvg_util.hpp"
#include "ui/error_box.hpp"
#include "ui/option_box.hpp"
#include "utils/thread.hpp"
#include "utils/utils.hpp"
#include "yati/yati.hpp"
#include "i18n.hpp"
#include "haze_helper.hpp"

#include <algorithm>
#include <cstdio>

namespace sphaira::ui::menu::mtp {
namespace {

constexpr s64 MAX_LOG_ENTRIES = 500;
constexpr s64 VISIBLE_LOG_ROWS = 9;
constexpr u64 WORKER_WAIT_TIMEOUT_NS = 100ULL * 1000ULL * 1000ULL;

std::string FormatErrorDetail(Result rc) {
    char buf[128];
    const auto* name = ui::GetResultName(rc);
    if (name && name[0] != '\0') {
        std::snprintf(buf, sizeof(buf), "%s (0x%X)", name, R_VALUE(rc));
    } else {
        std::snprintf(buf, sizeof(buf), "0x%X (Mod %u Desc %u)", R_VALUE(rc), R_MODULE(rc), R_DESCRIPTION(rc));
    }
    return buf;
}

} // namespace

Menu::Menu(u32 flags) : MenuBase{"MTP Install"_i18n, flags} {
    mutexInit(&m_mutex);
    condvarInit(&m_worker_done_cv);

    App::SetAutoSleepDisabled(true);

    if (App::AcquireScreenToggle()) {
        m_screen_toggle_enabled = true;
        SetAction(Button::Y, Action{"Screen Off"_i18n, [](){
            App::SetScreenDisabled(true);
        }});
    }

    SetAction(Button::B, Action{"Back"_i18n, [this](){
        bool installing = false;
        {
            SCOPED_MUTEX(&m_mutex);
            installing = m_installing;
        }

        if (installing) {
            App::Push<OptionBox>("Are you sure you wish to cancel?"_i18n, "No"_i18n, "Yes"_i18n, 1, [this](auto op_index){
                if (op_index && *op_index) {
                    CancelActiveInstall();
                    SetPop();
                }
            });
        } else {
            SetPop();
        }
    }});

    SetAction(Button::X, Action{"Options"_i18n, [this](){
        App::DisplayInstallOptions(false);
    }});

    SetAction(Button::UP, Action{[this](){
        ScrollLog(-1);
    }});

    SetAction(Button::DOWN, Action{[this](){
        ScrollLog(1);
    }});

    m_was_mtp_enabled = libhaze::IsInit();
    if (!m_was_mtp_enabled) {
        log_write("[MTP] wasn't enabled, forcefully enabling\n");
        libhaze::Init();
    }

    libhaze::InitInstallMode(
        [this](const char* path){ return OnInstallStart(path); },
        [this](const void *buf, size_t size){ return OnInstallWrite(buf, size); },
        [this](){ return OnInstallClose(); }
    );
}

Menu::~Menu() {
    m_stop_source.request_stop();
    CancelActiveInstall();
    libhaze::DisableInstallMode();
    JoinWorker();

    if (m_boost_active) {
        App::SetBoostMode(false);
        m_boost_active = false;
    }

    if (!m_was_mtp_enabled) {
        log_write("[MTP] disabling on exit\n");
        libhaze::Exit();
    }

    if (m_screen_toggle_enabled) {
        App::ReleaseScreenToggle();
        m_screen_toggle_enabled = false;
    }

    App::SetAutoSleepDisabled(false);
}

void Menu::CancelActiveInstall() {
    std::shared_ptr<stream::Stream> source;
    std::shared_ptr<ui::ProgressBox> pbox;
    {
        SCOPED_MUTEX(&m_mutex);
        source = m_source;
        pbox = m_pbox;
        condvarWakeAll(&m_worker_done_cv);
    }

    if (pbox) {
        pbox->RequestExit();
    }
    if (source) {
        source->SetInstallFinished();
    }
}

void Menu::JoinWorker() {
    std::unique_ptr<Thread> thread;
    {
        SCOPED_MUTEX(&m_mutex);
        thread = std::move(m_worker_thread);
    }

    if (thread) {
        if (R_FAILED(threadWaitForExit(thread.get()))) {
            log_write("[MTP] failed to join worker thread\n");
        }
        if (R_FAILED(threadClose(thread.get()))) {
            log_write("[MTP] failed to close worker thread\n");
        }
    }
}

void Menu::ScrollLog(s64 delta) {
    SCOPED_MUTEX(&m_mutex);
    const s64 total = static_cast<s64>(m_log.size());
    const s64 max_scroll = std::max<s64>(0, total - VISIBLE_LOG_ROWS);
    m_log_scroll = std::clamp<s64>(m_log_scroll + delta, 0, max_scroll);
    m_auto_scroll = (m_log_scroll >= max_scroll);
}

void Menu::WorkerThreadFunc(void* arg) {
    auto* self = static_cast<Menu*>(arg);

    std::shared_ptr<stream::Stream> source;
    std::shared_ptr<ui::ProgressBox> pbox;
    std::string filename;
    {
        SCOPED_MUTEX(&self->m_mutex);
        source = self->m_source;
        pbox = self->m_pbox;
        filename = self->m_current_file;
    }

    Result rc = Result_TransferCancelled;
    if (source && pbox) {
        // Atomically end the stream when installation completes or fails early
        // so any remaining Bulk OUT bytes drain cleanly in Stream::Push().
        ON_SCOPE_EXIT(source->SetInstallFinished());
        rc = yati::InstallFromSource(pbox.get(), source.get(), source->GetPath());
    } else if (source) {
        source->SetInstallFinished();
    }

    std::string detail;
    const double elapsed = self->m_install_start_ts.GetSecondsD();
    if (R_SUCCEEDED(rc)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1fs", elapsed);
        detail = buf;
    } else {
        detail = FormatErrorDetail(rc);
    }

    {
        SCOPED_MUTEX(&self->m_mutex);
        if (R_SUCCEEDED(rc)) {
            self->m_ok_count++;
        } else {
            self->m_fail_count++;
        }

        if (static_cast<s64>(self->m_log.size()) >= MAX_LOG_ENTRIES) {
            self->m_log.erase(self->m_log.begin());
            if (self->m_log_scroll > 0) {
                self->m_log_scroll--;
            }
        }

        self->m_log.push_back(LogEntry{std::move(filename), std::move(detail), rc, R_SUCCEEDED(rc)});
        if (self->m_auto_scroll) {
            const s64 max_scroll = std::max<s64>(0, static_cast<s64>(self->m_log.size()) - VISIBLE_LOG_ROWS);
            self->m_log_scroll = max_scroll;
        }

        self->m_current_rc = rc;
        self->m_worker_done = true;
        condvarWakeAll(&self->m_worker_done_cv);
    }
}

bool Menu::OnInstallStart(const char* path) {
    log_write("[MTP::OnInstallStart] %s\n", path);
    JoinWorker();

    if (GetToken().stop_requested() || !libhaze::IsInstallModeEnabled()) {
        log_write("[MTP::OnInstallStart] cancelled\n");
        return false;
    }

    auto source = std::make_shared<stream::Stream>(path, GetToken());
    auto pbox = std::make_shared<ui::ProgressBox>(ui::ProgressBox::HeadlessTag{});
    pbox->SetTitle(path);

    if (App::GetApp()->m_progress_boost_mode.Get() && !m_boost_active) {
        App::SetBoostMode(true);
        m_boost_active = true;
    }

    {
        SCOPED_MUTEX(&m_mutex);
        m_source = source;
        m_pbox = pbox;
        m_current_file = path;
        m_current_rc = 0;
        m_worker_done = false;
        m_installing = true;
        m_install_start_ts.Update();
    }

    auto thread = std::make_unique<Thread>();
    Result rc = utils::CreateThread(thread.get(), WorkerThreadFunc, this);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(thread.get());
        if (R_FAILED(rc)) {
            threadClose(thread.get());
        }
    }

    if (R_FAILED(rc)) {
        log_write("[MTP::OnInstallStart] failed to start worker: 0x%X\n", R_VALUE(rc));
        source->SetInstallFinished();
        {
            SCOPED_MUTEX(&m_mutex);
            m_source.reset();
            m_pbox.reset();
            m_current_file.clear();
            m_installing = false;
            m_worker_done = false;
        }
        if (m_boost_active) {
            App::SetBoostMode(false);
            m_boost_active = false;
        }
        return false;
    }

    {
        SCOPED_MUTEX(&m_mutex);
        m_worker_thread = std::move(thread);
    }

    return true;
}

bool Menu::OnInstallWrite(const void* buf, size_t size) {
    std::shared_ptr<stream::Stream> source;
    {
        SCOPED_MUTEX(&m_mutex);
        source = m_source;
    }

    if (!source) {
        log_write("[MTP::OnInstallWrite] no source\n");
        return false;
    }

    return source->Push(buf, size);
}

Result Menu::OnInstallClose() {
    log_write("[MTP::OnInstallClose] inside\n");

    std::shared_ptr<stream::Stream> source;
    {
        SCOPED_MUTEX(&m_mutex);
        source = m_source;
    }

    if (source) {
        source->Disable();
    }

    Result rc{};
    {
        SCOPED_MUTEX(&m_mutex);
        while (m_installing && !m_worker_done) {
            if (GetToken().stop_requested() || !libhaze::IsInstallModeEnabled()) {
                if (m_pbox) {
                    m_pbox->RequestExit();
                }
                if (m_source) {
                    m_source->SetInstallFinished();
                }
            }
            condvarWaitTimeout(&m_worker_done_cv, &m_mutex, WORKER_WAIT_TIMEOUT_NS);
        }
        rc = m_current_rc;
    }

    JoinWorker();

    {
        SCOPED_MUTEX(&m_mutex);
        m_source.reset();
        m_pbox.reset();
        m_current_file.clear();
        m_installing = false;
        m_worker_done = false;
    }

    if (m_boost_active) {
        App::SetBoostMode(false);
        m_boost_active = false;
    }

    log_write("[MTP::OnInstallClose] finished rc: 0x%X\n", R_VALUE(rc));
    return rc;
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    if (m_poll_ts.GetSeconds() >= 1) {
        m_poll_ts.Update();

        UsbState state{UsbState_Detached};
        usbDsGetState(&state);

        UsbDeviceSpeed speed{(UsbDeviceSpeed)UsbDeviceSpeed_None};
        usbDsGetSpeed(&speed);

        char buf[128];
        std::snprintf(buf, sizeof(buf), "State: %s | Speed: %s"_i18n.c_str(), i18n::get(GetUsbDsStateStr(state)).c_str(), i18n::get(GetUsbDsSpeedStr(speed)).c_str());
        SetSubHeading(buf);
    }
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    bool installing = false;
    std::shared_ptr<ui::ProgressBox> pbox;
    std::string current_file;
    size_t ok_count = 0;
    size_t fail_count = 0;
    s64 log_scroll = 0;
    std::vector<LogEntry> log_snapshot;

    {
        SCOPED_MUTEX(&m_mutex);
        installing = m_installing;
        pbox = m_pbox;
        current_file = m_current_file;
        ok_count = m_ok_count;
        fail_count = m_fail_count;
        log_scroll = m_log_scroll;
        log_snapshot = m_log;
    }

    const Vec4 status_panel{40.f, 100.f, 1200.f, 140.f};
    gfx::drawRect(vg, status_panel, theme->GetColour(ThemeEntryID_POPUP), 5.f);

    if (installing && pbox) {
        const auto snap = pbox->GetProgressSnapshot();
        const auto& title = snap.title.empty() ? current_file : snap.title;

        m_scroll_title.Draw(vg, true, status_panel.x + 25.f, status_panel.y + 16.f, status_panel.w - 50.f, 22.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), title);

        if (!snap.transfer.empty()) {
            m_scroll_transfer.Draw(vg, true, status_panel.x + 25.f, status_panel.y + 48.f, status_panel.w - 50.f, 18.f,
                NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT_INFO), snap.transfer);
        }

        const Vec4 prog_bar{status_panel.x + 75.f, status_panel.y + 84.f, status_panel.w - 190.f, 12.f};
        gfx::drawRect(vg, prog_bar, theme->GetColour(ThemeEntryID_PROGRESSBAR_BACKGROUND), 5.f);

        if (snap.size > 0) {
            const float ratio = std::clamp<float>(static_cast<float>(snap.offset) / static_cast<float>(snap.size), 0.f, 1.f);
            const u32 percentage = static_cast<u32>(ratio * 100.0f);
            gfx::drawRect(vg, prog_bar.x, prog_bar.y, ratio * prog_bar.w, prog_bar.h, theme->GetColour(ThemeEntryID_PROGRESSBAR), 5.f);
            gfx::drawTextArgs(vg, prog_bar.x + prog_bar.w + 15.f, prog_bar.y + prog_bar.h / 2.f, 18.f,
                NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT), "%u%%", percentage);
        } else {
            gfx::drawTextArgs(vg, prog_bar.x + prog_bar.w + 15.f, prog_bar.y + prog_bar.h / 2.f, 18.f,
                NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "...");
        }

        gfx::drawSpinner(vg, theme, status_panel.x + 42.f, prog_bar.y + prog_bar.h / 2.f, 14.f, armTicksToNs(armGetSystemTick()) / 1e+9);

        if (snap.size > 0 && snap.speed > 0) {
            const s64 left = std::max<s64>(0, snap.size - snap.offset);
            const s64 left_seconds = left / snap.speed;
            const auto hours = left_seconds / (60 * 60);
            const auto minutes = (left_seconds % (60 * 60)) / 60;
            const auto seconds = left_seconds % 60;

            char time_str[64];
            if (hours) {
                std::snprintf(time_str, sizeof(time_str), "%zu hours %zu minutes remaining"_i18n.c_str(), static_cast<size_t>(hours), static_cast<size_t>(minutes));
            } else if (minutes) {
                std::snprintf(time_str, sizeof(time_str), "%zu minutes %zu seconds remaining"_i18n.c_str(), static_cast<size_t>(minutes), static_cast<size_t>(seconds));
            } else {
                std::snprintf(time_str, sizeof(time_str), "%zu seconds remaining"_i18n.c_str(), static_cast<size_t>(seconds));
            }

            gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, prog_bar.y + prog_bar.h + 16.f, 17.f,
                NVG_ALIGN_CENTER | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT_INFO),
                "%s (%s)", time_str, utils::formatSizeNetwork(snap.speed).c_str());
        } else if (snap.size > 0) {
            gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, prog_bar.y + prog_bar.h + 16.f, 17.f,
                NVG_ALIGN_CENTER | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT_INFO),
                "%s / %s", utils::formatSizeStorage(snap.offset).c_str(), utils::formatSizeStorage(snap.size).c_str());
        }
    } else if (log_snapshot.empty()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, status_panel.y + status_panel.h / 2.f, 24.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO),
            "Drag'n'Drop (NSP, XCI, NSZ, XCZ, MSP) to the install folder"_i18n.c_str());
    } else {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, status_panel.y + 46.f, 24.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT),
            "Installed: %zu  |  Failed: %zu", ok_count, fail_count);
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, status_panel.y + 92.f, 19.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO),
            "Drag'n'Drop (NSP, XCI, NSZ, XCZ, MSP) to the install folder"_i18n.c_str());
    }

    const Vec4 log_panel{40.f, 255.f, 1200.f, 375.f};
    gfx::drawRect(vg, log_panel, theme->GetColour(ThemeEntryID_POPUP), 5.f);

    gfx::drawTextArgs(vg, log_panel.x + 25.f, log_panel.y + 14.f, 20.f,
        NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "Session Log");
    gfx::drawTextArgs(vg, log_panel.x + log_panel.w - 25.f, log_panel.y + 16.f, 18.f,
        NVG_ALIGN_RIGHT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT_INFO),
        "OK: %zu  |  Failed: %zu", ok_count, fail_count);
    gfx::drawRect(vg, log_panel.x + 15.f, log_panel.y + 44.f, log_panel.w - 30.f, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));

    const float list_x = log_panel.x + 25.f;
    const float list_y = log_panel.y + 52.f;
    const float list_w = log_panel.w - 65.f;
    const float row_h = 34.f;
    const s64 total = static_cast<s64>(log_snapshot.size());
    const s64 max_scroll = std::max<s64>(0, total - VISIBLE_LOG_ROWS);
    const s64 start_idx = std::clamp<s64>(log_scroll, 0, max_scroll);
    const s64 end_idx = std::min<s64>(total, start_idx + VISIBLE_LOG_ROWS);

    if (log_snapshot.empty()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, list_y + 140.f, 20.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO),
            "Waiting for files...");
    } else {
        nvgSave(vg);
        nvgIntersectScissor(vg, log_panel.x + 15.f, list_y, log_panel.w - 30.f, row_h * VISIBLE_LOG_ROWS);

        for (s64 i = start_idx; i < end_idx; i++) {
            const auto& entry = log_snapshot[static_cast<size_t>(i)];
            const float y = list_y + static_cast<float>(i - start_idx) * row_h + row_h / 2.f;

            const auto status_color = theme->GetColour(entry.ok ? ThemeEntryID_TEXT_SELECTED : ThemeEntryID_ERROR);
            const char* tag = entry.ok ? "[OK]" : "[FAIL]";

            gfx::drawTextArgs(vg, list_x, y, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, status_color, "%s", tag);

            float bounds[4]{};
            nvgFontSize(vg, 17.f);
            gfx::textBounds(vg, 0, 0, bounds, entry.detail.c_str());
            const float detail_w = std::min<float>(std::max<float>(0.f, bounds[2] - bounds[0]), 460.f);
            const float name_x = list_x + 74.f;
            const float name_max_w = std::max<float>(80.f, list_w - 74.f - detail_w - 20.f);

            nvgSave(vg);
            nvgIntersectScissor(vg, name_x, y - row_h / 2.f, name_max_w, row_h);
            gfx::drawTextArgs(vg, name_x, y, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
                theme->GetColour(ThemeEntryID_TEXT), "%s", entry.name.c_str());
            nvgRestore(vg);

            gfx::drawTextArgs(vg, list_x + list_w, y, 17.f, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE,
                entry.ok ? theme->GetColour(ThemeEntryID_TEXT_INFO) : status_color,
                "%s", entry.detail.c_str());
        }

        nvgRestore(vg);

        if (total > VISIBLE_LOG_ROWS) {
            gfx::drawScrollbar(vg, theme, log_panel.x + log_panel.w - 22.f, list_y, row_h * VISIBLE_LOG_ROWS,
                static_cast<u32>(start_idx), static_cast<u32>(total), static_cast<u32>(VISIBLE_LOG_ROWS));
        }
    }
}

} // namespace sphaira::ui::menu::mtp
