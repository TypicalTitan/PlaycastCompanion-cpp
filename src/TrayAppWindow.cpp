#include "pch.h"
#include "TrayAppInternal.h"

namespace pc {
void TrayApp::Impl::AddTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = static_cast<DWORD>(sizeof(nid));
    nid.hWnd = Hwnd();
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_PC_TRAY;
    nid.hIcon = guestActive.load() ? iconStealth.h : iconIdle.h;
    wcsncpy_s(nid.szTip, ARRAYSIZE(nid.szTip), trayTip.c_str(), _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        trayAdded = false;
        LogInfo(std::format(L"tray icon add failed (error {})", GetLastError()));
        return;
    }
    trayAdded = true;
    nid.uVersion = NOTIFYICON_VERSION_4;
    trayV4 = Shell_NotifyIconW(NIM_SETVERSION, &nid) != FALSE;
}

void TrayApp::Impl::RemoveTrayIcon() {
    if (!trayAdded)
        return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = static_cast<DWORD>(sizeof(nid));
    nid.hWnd = Hwnd();
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded = false;
}

void TrayApp::Impl::UpdateTray(bool active) {
    trayTip = active ? kTipStealth : kTipQuiet;
    if (!trayAdded)
        return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = static_cast<DWORD>(sizeof(nid));
    nid.hWnd = Hwnd();
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = active ? iconStealth.h : iconIdle.h;
    wcsncpy_s(nid.szTip, ARRAYSIZE(nid.szTip), trayTip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayApp::Impl::ShowTrayMenu(POINT anchor) {
    MenuHandle menu(CreatePopupMenu());
    if (menu.h == nullptr)
        return;
    AppendMenuW(menu.h, MF_STRING, kMenuOpen, L"Open Playcast Companion");
    AppendMenuW(menu.h, MF_STRING, kMenuPreview, L"Preview guest mode (10 s)");
    AppendMenuW(menu.h, MF_STRING, kMenuCheck, L"Check again now");
    AppendMenuW(menu.h, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.h, MF_STRING, kMenuExit, L"Exit");
    SetMenuDefaultItem(menu.h, kMenuOpen, FALSE);  // bold, also the double-click action

    // Standard tray-menu dance: the menu closes when the window loses
    // foreground status; the WM_NULL nudge afterwards avoids a stuck menu.
    SetForegroundWindow(Hwnd());
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN;
    flags |= (GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const int command = TrackPopupMenuEx(menu.h, flags, anchor.x, anchor.y, Hwnd(), nullptr);
    PostMessageW(Hwnd(), WM_NULL, 0, 0);

    switch (static_cast<UINT>(command)) {
    case kMenuOpen:
        OpenSettings();
        break;
    case kMenuPreview:
        TestGuestMode();
        break;
    case kMenuCheck:
        RequestEvaluate(L"manual");
        break;
    case kMenuExit:
        Exit();
        break;
    default:
        break;
    }
}

void TrayApp::Impl::OnTrayMessage(WPARAM wParam, LPARAM lParam) {
    // NOTIFYICON_VERSION_4: LOWORD(lParam) = event, wParam = anchor point.
    // Legacy (SETVERSION failed): lParam = event, no coordinates.
    const UINT event = trayV4 ? LOWORD(lParam) : static_cast<UINT>(lParam);
    switch (event) {
    case WM_LBUTTONDBLCLK:
    case NIN_KEYSELECT:
        OpenSettings();
        break;
    case WM_CONTEXTMENU:
        if (trayV4)
            ShowTrayMenu(POINT{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)});
        break;
    case WM_RBUTTONUP:
        if (!trayV4) {
            POINT pt{};
            GetCursorPos(&pt);
            ShowTrayMenu(pt);
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// hidden window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK TrayApp::Impl::WndProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    Impl* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<Impl*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (self == nullptr)
        return DefWindowProcW(h, msg, wParam, lParam);

    // Never let an exception escape a window procedure (rule 10).
    try {
        return self->HandleMessage(h, msg, wParam, lParam);
    } catch (const std::exception& ex) {
        LogInfo(L"UI exception: " + Describe(ex));
    } catch (const winrt::hresult_error& e) {
        LogInfo(L"UI exception: " + std::wstring(e.message()));
    } catch (...) {
        LogInfo(L"UI exception: unknown error");
    }
    return 0;
}

LRESULT TrayApp::Impl::HandleMessage(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCDESTROY) {
        // Last message: detach before touching any member (the watchers may
        // already be gone when the window is destroyed during teardown).
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return DefWindowProcW(h, msg, wParam, lParam);
    }
    if (sessions && sessions->HandleMessage(msg, wParam, lParam))
        return 0;

    if (taskbarCreatedMsg != 0 && msg == taskbarCreatedMsg) {
        // Explorer restarted: re-add the icon (NotifyIcon did this for us in C#).
        if (!headless && !exiting.load()) {
            trayAdded = false;
            AddTrayIcon();
        }
        return 0;
    }

    switch (msg) {
    case WM_PC_TRAY:
        OnTrayMessage(wParam, lParam);
        return 0;
    case WM_PC_EVALUATED:
        UpdateTray(wParam != 0);
        return 0;
    case WM_PC_OPEN_SETTINGS:
        OpenSettings();
        return 0;
    case WM_PC_PREVIEW:
        SetTimer(h, kPreviewTimerId, kPreviewMs, nullptr);
        return 0;
    case WM_PC_APPLY_SETTINGS:
        // The eval worker has quiesced every config reader and is waiting on
        // settingsCv; nothing on this thread is mid-read between messages.
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            cfg = pending;
            pendingValid = false;
            const bool secrets = sessionLogger && sessionLogger->IncludeSecrets();
            try {
                sessionLogger = std::make_unique<SessionLogger>(cfg, headless, g_harnessMode.load());
                sessionLogger->SetIncludeSecrets(secrets);
            } catch (...) { sessionLogger.reset(); LogInfo(L"Session logger settings could not be applied."); }
            sessions = std::make_unique<SessionWatcher>(cfg, Hwnd(), [this] { RequestEvaluate(L"session change"); });
            registry.reset();
            if (cfg.RegistryWatch.Enabled) registry = std::make_unique<RegistryWatcher>(cfg.RegistryWatch, [this] { RequestEvaluate(L"registry change"); });
            settingsApplied = true;
        }
        settingsCv.notify_all();
        return 0;
    case WM_TIMER:
        if (wParam == kPollTimerId) {
            RequestEvaluate(L"poll");
        } else if (wParam == kPreviewTimerId) {
            KillTimer(h, kPreviewTimerId);
            RequestEvaluate(L"preview end");
        }
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;  // never block logoff/shutdown; the work happens on WM_ENDSESSION
    case WM_ENDSESSION:
        // Release promptly when this session logs off or the machine shuts
        // down (SystemEvents.SessionEnded in C#).
        if (wParam != 0)
            Exit();
        return 0;
    case WM_CLOSE:
        Exit();  // e.g. a polite taskkill
        return 0;
    default:
        return DefWindowProcW(h, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// evaluation worker
// ---------------------------------------------------------------------------


}
