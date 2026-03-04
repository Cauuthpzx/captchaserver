#include "captcha_popup.h"
#include "utils/logger.h"
#include <windowsx.h>
#include <commctrl.h>
#include <sstream>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace agent {

static const wchar_t* k_ClassName = L"CaptchaPopupClass";
static bool s_class_registered = false;
std::atomic<bool> CaptchaPopup::s_popup_active{false};
CaptchaPopup* CaptchaPopup::s_current_popup = nullptr;
CaptchaPopup::CloseAlertCallback CaptchaPopup::s_close_alert_cb;

// GDI+ init/shutdown helper
static ULONG_PTR s_gdiplus_token = 0;
static int s_gdiplus_refcount = 0;

static void ensure_gdiplus() {
    if (s_gdiplus_refcount == 0) {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&s_gdiplus_token, &input, nullptr);
    }
    s_gdiplus_refcount++;
}

static void release_gdiplus() {
    s_gdiplus_refcount--;
    if (s_gdiplus_refcount == 0 && s_gdiplus_token) {
        Gdiplus::GdiplusShutdown(s_gdiplus_token);
        s_gdiplus_token = 0;
    }
}

// Load PNG from memory using GDI+
static Gdiplus::Bitmap* load_bitmap_from_memory(const std::vector<uint8_t>& data) {
    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, data.size());
    if (!hGlobal) return nullptr;

    void* ptr = GlobalLock(hGlobal);
    if (!ptr) { GlobalFree(hGlobal); return nullptr; }
    memcpy(ptr, data.data(), data.size());
    GlobalUnlock(hGlobal);

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hGlobal, TRUE, &stream))) {
        GlobalFree(hGlobal);
        return nullptr;
    }

    auto* bitmap = Gdiplus::Bitmap::FromStream(stream);
    stream->Release();

    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) {
        delete bitmap;
        return nullptr;
    }

    return bitmap;
}

CaptchaPopup::CaptchaPopup() = default;

CaptchaPopup::~CaptchaPopup() {
    delete m_gdi_bitmap;
    m_gdi_bitmap = nullptr;
}

void CaptchaPopup::show(const std::vector<uint8_t>& image_data, int timeout_sec,
                         const std::string& popup_message, PopupResultCallback callback) {
    // Prevent multiple popups
    if (s_popup_active.exchange(true)) {
        Logger::info("popup already active, skipping");
        // Send miss for this one since we can't show it
        if (callback) callback("", true, 0);
        return;
    }

    m_callback = std::move(callback);
    m_answered.store(false);
    m_close_warned = false;
    m_timeout_sec = timeout_sec;
    m_remaining_sec = timeout_sec;
    m_popup_message = popup_message;
    s_current_popup = this;

    ensure_gdiplus();
    create_window(image_data, timeout_sec);

    // Message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessage(m_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (m_answered.load()) break;
    }

    if (m_hwnd) {
        KillTimer(m_hwnd, k_TimerID);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }

    // Clean up GDI font handles (M1 fix)
    if (m_font) { DeleteObject(m_font); m_font = nullptr; }
    if (m_msg_font) { DeleteObject(m_msg_font); m_msg_font = nullptr; }

    delete m_gdi_bitmap;
    m_gdi_bitmap = nullptr;

    release_gdiplus();

    s_current_popup = nullptr;
    s_popup_active.store(false);
}

void CaptchaPopup::create_window(const std::vector<uint8_t>& image_data, int timeout_sec) {
    // Register window class only once (M2 fix — avoids brush handle leak)
    if (!s_class_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 40));
        wc.lpszClassName = k_ClassName;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        if (RegisterClassExW(&wc)) {
            s_class_registered = true;
        }
    }

    // Load image via GDI+
    m_gdi_bitmap = load_bitmap_from_memory(image_data);
    if (m_gdi_bitmap) {
        m_img_width = static_cast<int>(m_gdi_bitmap->GetWidth());
        m_img_height = static_cast<int>(m_gdi_bitmap->GetHeight());
        Logger::info("captcha image loaded: " + std::to_string(m_img_width) + "x" + std::to_string(m_img_height));
    } else {
        Logger::error("failed to load captcha image from PNG data (" + std::to_string(image_data.size()) + " bytes)");
        m_img_width = 0;
        m_img_height = 0;
    }

    // Center on screen
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int x = (screen_w - k_WindowWidth) / 2;
    int y = (screen_h - k_WindowHeight) / 2;

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        k_ClassName, L"Verification Required",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, k_WindowWidth, k_WindowHeight,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

    if (!m_hwnd) {
        Logger::error("failed to create popup window: " + std::to_string(GetLastError()));
        // C5 fix: trigger miss callback instead of leaving popup in limbo
        m_answered.store(true);
        if (m_callback) {
            m_callback("", true, 0);
        }
        return;
    }

    // Popup message label at top
    std::wstring wide_msg;
    if (!m_popup_message.empty()) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, m_popup_message.c_str(), -1, nullptr, 0);
        wide_msg.resize(wlen - 1);
        MultiByteToWideChar(CP_UTF8, 0, m_popup_message.c_str(), -1, wide_msg.data(), wlen);
    }
    m_message_label = CreateWindowExW(0, L"STATIC",
        wide_msg.empty() ? L"" : wide_msg.c_str(),
        WS_CHILD | (wide_msg.empty() ? 0 : WS_VISIBLE) | SS_CENTER,
        15, 5, 340, 22, m_hwnd, nullptr, nullptr, nullptr);

    // Edit box for answer
    m_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        25, 145, 220, 30, m_hwnd, nullptr, nullptr, nullptr);

    // Submit button
    m_button = CreateWindowExW(0, L"BUTTON", L"Submit",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        255, 145, 90, 30, m_hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(k_SubmitBtnID)),
        nullptr, nullptr);

    // Timer label
    m_timer_label = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        25, 190, 320, 25, m_hwnd, nullptr, nullptr, nullptr);

    // Set font — stored as members for proper cleanup (M1 fix)
    m_font = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    m_msg_font = CreateFontW(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    SendMessage(m_message_label, WM_SETFONT, reinterpret_cast<WPARAM>(m_msg_font), TRUE);
    SendMessage(m_edit, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
    SendMessage(m_button, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
    SendMessage(m_timer_label, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

    // Focus on edit
    SetFocus(m_edit);

    // Start countdown timer
    m_start_tick = GetTickCount64();
    m_remaining_sec = timeout_sec;
    update_timer_label();
    m_timer_id = SetTimer(m_hwnd, k_TimerID, 1000, nullptr);

    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
    UpdateWindow(m_hwnd);

    // Play notification sound
    MessageBeep(MB_ICONEXCLAMATION);
}

void CaptchaPopup::update_timer_label() {
    if (!m_timer_label) return;
    std::wstring text = L"Time remaining: " + std::to_wstring(m_remaining_sec) + L"s";
    SetWindowTextW(m_timer_label, text.c_str());
}

void CaptchaPopup::submit_answer() {
    if (m_answered.load()) return;
    m_answered.store(true);

    wchar_t buf[256] = {};
    GetWindowTextW(m_edit, buf, 255);

    std::string answer;
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (len > 0) {
        answer.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, answer.data(), len, nullptr, nullptr);
    }

    auto elapsed_ms = static_cast<int64_t>(GetTickCount64() - m_start_tick);
    bool is_miss = answer.empty();

    if (m_callback) {
        m_callback(answer, is_miss, elapsed_ms);
    }

    PostMessage(m_hwnd, WM_CLOSE, 0, 0);
}

void CaptchaPopup::on_timeout() {
    if (m_answered.load()) return;
    m_answered.store(true);

    auto elapsed_ms = static_cast<int64_t>(GetTickCount64() - m_start_tick);

    if (m_callback) {
        m_callback("", true, elapsed_ms);
    }

    PostMessage(m_hwnd, WM_CLOSE, 0, 0);
}

LRESULT CALLBACK CaptchaPopup::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* popup = s_current_popup;

    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == k_SubmitBtnID && popup) {
            popup->submit_answer();
            return 0;
        }
        break;

    case WM_TIMER:
        if (wp == k_TimerID && popup) {
            popup->m_remaining_sec--;
            popup->update_timer_label();
            if (popup->m_remaining_sec <= 0) {
                KillTimer(hwnd, k_TimerID);
                popup->on_timeout();
            }
            return 0;
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        if (popup && popup->m_gdi_bitmap) {
            Gdiplus::Graphics graphics(hdc);
            // Draw image centered in the area (25, 28) to (355, 135)
            int area_w = 330;
            int area_h = 105;
            int img_w = popup->m_img_width;
            int img_h = popup->m_img_height;

            // Scale to fit
            float scale = 1.0f;
            if (img_w > area_w || img_h > area_h) {
                float sx = static_cast<float>(area_w) / static_cast<float>(img_w);
                float sy = static_cast<float>(area_h) / static_cast<float>(img_h);
                scale = (sx < sy) ? sx : sy;
            }
            int draw_w = static_cast<int>(static_cast<float>(img_w) * scale);
            int draw_h = static_cast<int>(static_cast<float>(img_h) * scale);
            int draw_x = 25 + (area_w - draw_w) / 2;
            int draw_y = 28 + (area_h - draw_h) / 2;

            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.DrawImage(popup->m_gdi_bitmap, draw_x, draw_y, draw_w, draw_h);
        } else {
            // Draw placeholder text if no image
            SetTextColor(hdc, RGB(200, 200, 200));
            SetBkMode(hdc, TRANSPARENT);
            RECT rc = {25, 28, 355, 135};
            DrawTextW(hdc, L"[CAPTCHA image]", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CLOSE:
        if (popup && !popup->m_answered.load()) {
            if (!popup->m_close_warned) {
                // First close attempt: show warning
                popup->m_close_warned = true;
                MessageBoxW(hwnd,
                    L"Closing without answering will be recorded as MISS.\n"
                    L"Press X again to confirm.",
                    L"Warning", MB_OK | MB_ICONWARNING);
                return 0; // Don't close yet
            }
            // Second close attempt: treat as miss + send close_popup alert
            popup->on_timeout();
            if (s_close_alert_cb) {
                s_close_alert_cb();
            }
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace agent
