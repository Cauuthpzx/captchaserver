#pragma once

#include "agent/types.h"
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <atomic>
#include <mutex>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

namespace agent {

using PopupResultCallback = std::function<void(const std::string& answer, bool is_miss, int64_t response_ms)>;

class CaptchaPopup {
public:
    CaptchaPopup();
    ~CaptchaPopup();

    CaptchaPopup(const CaptchaPopup&) = delete;
    CaptchaPopup& operator=(const CaptchaPopup&) = delete;

    // Show popup with captcha image, blocks until answered or timeout
    void show(const std::vector<uint8_t>& image_data, int timeout_sec,
              const std::string& popup_message, PopupResultCallback callback);

    // Check if popup is currently active
    static bool is_active() { return s_popup_active.load(); }

    // Set callback for close alerts (called when user closes popup without answering)
    using CloseAlertCallback = std::function<void()>;
    static void set_close_alert_callback(CloseAlertCallback cb) { s_close_alert_cb = std::move(cb); }

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void create_window(const std::vector<uint8_t>& image_data, int timeout_sec);
    void submit_answer();
    void on_timeout();
    void update_timer_label();

    HWND m_hwnd = nullptr;
    HWND m_edit = nullptr;
    HWND m_button = nullptr;
    HWND m_timer_label = nullptr;
    HWND m_message_label = nullptr;
    std::string m_popup_message;
    Gdiplus::Bitmap* m_gdi_bitmap = nullptr;
    HFONT m_font = nullptr;
    HFONT m_msg_font = nullptr;
    int m_img_width = 0;
    int m_img_height = 0;

    PopupResultCallback m_callback;
    std::atomic<bool> m_answered{false};
    bool m_close_warned = false;
    ULONGLONG m_start_tick = 0;
    int m_timeout_sec = 60;
    UINT_PTR m_timer_id = 0;
    int m_remaining_sec = 0;

    static constexpr int k_WindowWidth = 380;
    static constexpr int k_WindowHeight = 290;
    static constexpr UINT_PTR k_TimerID = 1001;
    static constexpr int k_SubmitBtnID = 2001;

    static std::atomic<bool> s_popup_active;
    static CaptchaPopup* s_current_popup;
    static CloseAlertCallback s_close_alert_cb;
};

} // namespace agent
