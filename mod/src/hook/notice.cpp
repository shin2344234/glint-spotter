#include "hook/notice.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>

#include "core/load.h"
#include "core/log.h"

namespace
{
    constexpr UINT kMsgShow = WM_APP + 1;
    constexpr UINT_PTR kHideTimer = 1;
    constexpr UINT kShowMs = 2000;
    constexpr const wchar_t* kText = L"Glint marked";
    constexpr const wchar_t* kClass = L"GlintSpotterNotice";

    std::atomic<bool> g_asked{false};
    std::atomic<HWND> g_wnd{nullptr};
    HANDLE g_thread = nullptr;
    bool g_startTried = false;

    // Only touched on the notice thread.
    HWND g_game = nullptr;
    HFONT g_font = nullptr;
    int g_fontHeight = 0;
    uint64_t g_shown = 0;

    struct Find { HWND best; long long area; HWND skip; };

    BOOL CALLBACK OneWindow(HWND w, LPARAM lp)
    {
        Find& f = *reinterpret_cast<Find*>(lp);
        if (w == f.skip || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;
        RECT r{};
        if (!GetClientRect(w, &r)) return TRUE;
        const long long area = static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
        if (area > f.area) { f.area = area; f.best = w; }
        return TRUE;
    }

    // The game's window: the biggest visible top-level window this process
    // owns that is not the notice itself.
    HWND GameWindow(HWND self)
    {
        if (g_game && IsWindow(g_game) && IsWindowVisible(g_game)) return g_game;
        Find f{nullptr, 0, self};
        EnumWindows(OneWindow, reinterpret_cast<LPARAM>(&f));
        if (f.best != g_game)
        {
            g_game = f.best;
            if (g_game)
            {
                wchar_t title[128]{};
                GetWindowTextW(g_game, title, 128);
                GS_LOG("notice: the game window is 0x%p, \"%ls\"", static_cast<void*>(g_game), title);
            }
        }
        return g_game;
    }

    HFONT FontFor(int height)
    {
        if (g_font && g_fontHeight == height) return g_font;
        if (g_font) DeleteObject(g_font);
        g_font = CreateFontW(-height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_fontHeight = height;
        return g_font;
    }

    void Place(HWND self)
    {
        ++g_shown;
        HWND game = GameWindow(self);
        if (!game)
        {
            GS_LOG_ERR("notice: pin %llu, no visible game window found, so nothing is shown",
                       static_cast<unsigned long long>(g_shown));
            return;
        }
        // Over the game only while the game is the window in front. A notice
        // over the desktop after an alt-tab would be one nobody asked for.
        HWND front = GetForegroundWindow();
        if (front != game)
        {
            GS_LOG("notice: pin %llu, the game is not the window in front (0x%p is), so it gets no notice",
                   static_cast<unsigned long long>(g_shown), static_cast<void*>(front));
            return;
        }
        RECT c{};
        GetClientRect(game, &c);
        POINT origin{0, 0};
        ClientToScreen(game, &origin);
        const int cw = c.right - c.left, ch = c.bottom - c.top;
        if (cw <= 0 || ch <= 0)
        {
            GS_LOG_ERR("notice: pin %llu, the game window reads %dx%d, so nothing is shown",
                       static_cast<unsigned long long>(g_shown), cw, ch);
            return;
        }

        // Sized from the game's own height, so it reads the same at 1080p and
        // 4K whatever the desktop's scaling is.
        int fh = ch / 30;
        if (fh < 16) fh = 16;
        HFONT font = FontFor(fh);
        HDC dc = GetDC(self);
        HGDIOBJ old = SelectObject(dc, font);
        RECT t{0, 0, 0, 0};
        DrawTextW(dc, kText, -1, &t, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(self, dc);

        const int padX = fh, padY = fh / 2;
        const int w = (t.right - t.left) + 2 * padX;
        const int h = (t.bottom - t.top) + 2 * padY;
        const int x = origin.x + (cw - w) / 2;
        const int y = origin.y + ch * 14 / 100;

        SetWindowPos(self, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(self, nullptr, TRUE);
        SetTimer(self, kHideTimer, kShowMs, nullptr);
        GS_LOG("notice: showing \"%ls\" for pin %llu at (%d, %d), %dx%d, over a %dx%d game window",
               kText, static_cast<unsigned long long>(g_shown), x, y, w, h, cw, ch);
    }

    void Paint(HWND self)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(self, &ps);
        RECT r{};
        GetClientRect(self, &r);
        HBRUSH back = CreateSolidBrush(RGB(18, 18, 20));
        FillRect(dc, &r, back);
        DeleteObject(back);
        // A thin warm edge, the colour of a glint, so it reads as the mod's.
        HBRUSH edge = CreateSolidBrush(RGB(214, 178, 92));
        FrameRect(dc, &r, edge);
        DeleteObject(edge);
        if (g_font)
        {
            HGDIOBJ old = SelectObject(dc, g_font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(240, 236, 226));
            DrawTextW(dc, kText, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, old);
        }
        EndPaint(self, &ps);
    }

    LRESULT CALLBACK Proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
    {
        switch (m)
        {
        case kMsgShow: Place(w); return 0;
        case WM_TIMER:
            if (wp == kHideTimer)
            {
                KillTimer(w, kHideTimer);
                ShowWindow(w, SW_HIDE);
                GS_LOG("notice: hidden after %u ms", kShowMs);
            }
            return 0;
        case WM_PAINT: Paint(w); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_CLOSE: DestroyWindow(w); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: return DefWindowProcW(w, m, wp, lp);
        }
    }

    DWORD WINAPI NoticeThread(LPVOID)
    {
        HINSTANCE inst = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Proc), &inst);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.lpszClassName = kClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            GS_LOG_ERR("notice: RegisterClassExW failed with %lu, so Notice=1 shows nothing", GetLastError());
            return 0;
        }
        // Layered for the translucency, transparent so clicks go to the game,
        // a tool window so it is not on the taskbar or in alt-tab, and
        // no-activate so the game never loses focus to it.
        HWND w = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                     WS_EX_NOACTIVATE,
                                 kClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
        if (!w)
        {
            GS_LOG_ERR("notice: CreateWindowExW failed with %lu, so Notice=1 shows nothing", GetLastError());
            return 0;
        }
        SetLayeredWindowAttributes(w, 0, 225, LWA_ALPHA);
        g_wnd.store(w);
        GS_LOG_OK("notice: window ready on thread %lu; a pin shows \"%ls\" for %u ms", GetCurrentThreadId(),
                  kText, kShowMs);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        g_wnd.store(nullptr);
        if (g_font) { DeleteObject(g_font); g_font = nullptr; }
        return 0;
    }
}

namespace gs::notice
{
    void Show() { g_asked.store(true); }

    void Pump()
    {
        if (!g_asked.load()) return;
        if (!g_startTried)
        {
            g_startTried = true;
            g_thread = CreateThread(nullptr, 0, NoticeThread, nullptr, 0, nullptr);
            if (!g_thread)
            {
                GS_LOG_ERR("notice: could not start the notice thread (%lu), so Notice=1 shows nothing",
                           GetLastError());
                g_asked.store(false);
                return;
            }
            gs::load::AddThread("notice", g_thread);
        }
        HWND w = g_wnd.load();
        if (!w)
        {
            // Still starting, or it failed and said so. A thread that has
            // ended will never make the window, so drop the request then.
            if (g_thread && WaitForSingleObject(g_thread, 0) == WAIT_OBJECT_0) g_asked.store(false);
            return;
        }
        g_asked.store(false);
        PostMessageW(w, kMsgShow, 0, 0);
    }

    void Stop(bool processTerminating)
    {
        if (processTerminating || !g_thread) return;
        if (HWND w = g_wnd.load()) PostMessageW(w, WM_CLOSE, 0, 0);
        WaitForSingleObject(g_thread, 1000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}
