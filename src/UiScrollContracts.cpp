#include "pch.h"
#include "UiScrollContracts.h"
#include "MainWindow.h"
#include "TrayApp.h"
#include "Log.h"
#include <cstdio>

namespace pc {
namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::wstring WindowText(HWND window) {
    wchar_t text[512]{}; GetWindowTextW(window, text, static_cast<int>(std::size(text))); return text;
}
bool ClassIs(HWND window, const wchar_t* expected) {
    wchar_t name[128]{}; GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return CompareStringOrdinal(name, -1, expected, -1, TRUE) == CSTR_EQUAL;
}
std::vector<HWND> Children(HWND parent) {
    std::vector<HWND> result;
    EnumChildWindows(parent, [](HWND child, LPARAM context) -> BOOL {
        reinterpret_cast<std::vector<HWND>*>(context)->push_back(child); return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}
HWND CaptionControl(HWND root, const wchar_t* caption) {
    for (auto child : Children(root))
        if (ClassIs(child, L"BUTTON") && WindowText(child) == caption) return child;
    throw std::runtime_error("Expected public button not found");
}
HWND ClassControl(HWND page, const wchar_t* name) {
    for (HWND child = GetWindow(page, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        if (ClassIs(child, name)) return child;
    throw std::runtime_error("Expected public control not found");
}
RECT Bounds(HWND window) { RECT bounds{}; GetWindowRect(window, &bounds); return bounds; }
bool SameBounds(RECT a, RECT b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
POINT Center(HWND window) {
    const auto rectangle = Bounds(window);
    return {(rectangle.left + rectangle.right) / 2, (rectangle.top + rectangle.bottom) / 2};
}
POINT Background(HWND page) { POINT point{24, 60}; ClientToScreen(page, &point); return point; }
int Position(HWND page) {
    SCROLLINFO scroll{sizeof(SCROLLINFO), SIF_POS};
    Require(GetScrollInfo(page, SB_VERT, &scroll) != FALSE, "Page has no public vertical scroll state");
    return scroll.nPos;
}
void Pump(HWND root) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message != WM_QUIT && !IsDialogMessageW(root, &message)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
}
void PageEdge(HWND page, int edge) {
    SendMessageW(page, WM_VSCROLL, MAKEWPARAM(edge, 0), 0); Pump(GetAncestor(page, GA_ROOT));
}
int Maximum(HWND page) {
    SCROLLINFO scroll{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE};
    Require(GetScrollInfo(page, SB_VERT, &scroll) != FALSE, "Page has no public scroll range");
    return std::max(scroll.nMin, scroll.nMax - static_cast<int>(scroll.nPage ? scroll.nPage - 1 : 0));
}

class ScrollFixture {
public:
    explicit ScrollFixture(HINSTANCE instance) : app_(instance, false, nullptr), window_(app_) {
        foreground_ = GetForegroundWindow(); previousFocus_ = GetFocus();
        window_.ImportLogs({}); root_ = window_.Handle();
        Require(root_ && IsWindow(root_), "Public MainWindow did not create a window");
        SetWindowLongPtrW(root_, GWL_EXSTYLE, GetWindowLongPtrW(root_, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
        const UINT dpi = GetDpiForWindow(root_);
        RECT client{0, 0, MulDiv(760, static_cast<int>(dpi), 96), MulDiv(640, static_cast<int>(dpi), 96)};
        AdjustWindowRectExForDpi(&client, static_cast<DWORD>(GetWindowLongPtrW(root_, GWL_STYLE)), FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(root_, GWL_EXSTYLE)), dpi);
        SetWindowPos(root_, HWND_BOTTOM, -12000, -12000, client.right - client.left, client.bottom - client.top,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(root_, SW_SHOWNOACTIVATE); Pump(root_);
        Require(!foreground_ || GetForegroundWindow() == foreground_, "Diagnostic window stole foreground activation");
        save_ = CaptionControl(root_, L"Save changes"); discard_ = CaptionControl(root_, L"Discard");
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheelLines_, 0);
    }
    ~ScrollFixture() {
        SetFocus(previousFocus_ && IsWindow(previousFocus_) ? previousFocus_ : nullptr);
    }
    void FractionalAndMultipleDetents() {
        const auto page = Navigate(L"Settings"); PageEdge(page, SB_TOP);
        const auto point = Background(page); const int initial = Position(page);
        for (int i = 0; i < 3; ++i) {
            Wheel(page, -30, point);
            Require(Position(page) == initial, "Fractional wheel delta scrolled before one detent accumulated");
        }
        Wheel(page, -30, point); const int accumulated = Position(page);
        PageEdge(page, SB_TOP); Wheel(page, -120, point); const int one = Position(page);
        Require(accumulated == one, "Four quarter-detents differ from one full detent");
        if (wheelLines_) Require(one > initial, "Settings background did not scroll for a full detent");
        PageEdge(page, SB_TOP); Wheel(page, -240, point); const int multiple = Position(page);
        PageEdge(page, SB_TOP); Wheel(page, -120, point); Wheel(page, -120, point);
        Require(Position(page) == multiple, "Multi-detent message lost wheel magnitude");
        Require(multiple <= Maximum(page), "Multiple detents scrolled beyond the page range");
        if (wheelLines_ && one < Maximum(page)) Require(multiple > one, "Two detents did not advance beyond one detent");
        PageEdge(page, SB_TOP); Wheel(page, -60, point); Wheel(page, 60, point);
        Require(Position(page) == initial, "Opposing half-detents failed to cancel");
        Wheel(page, -120, point);
        Require(Position(page) == one, "Cancelled half-detents left a residual wheel delta");
    }
    void DirectSettingsChildAndFooter() {
        const auto page = Navigate(L"Settings"); PageEdge(page, SB_TOP);
        const auto edit = ClassControl(page, L"EDIT"); Focus(edit);
        const auto before = Bounds(edit); const auto save = Bounds(save_), discard = Bounds(discard_);
        const BOOL saveEnabled = IsWindowEnabled(save_); const auto text = WindowText(edit);
        Wheel(edit, -120, Center(edit));
        if (wheelLines_) {
            Require(Position(page) > 0, "Direct settings edit wheel did not reach page scrolling");
            Require(Bounds(edit).top < before.top, "Page scroll did not move its visible child control");
        }
        Require(WindowText(edit) == text, "Wheel changed settings text");
        Require(GetFocus() == edit, "Wheel changed keyboard focus");
        Require(SameBounds(save, Bounds(save_)) && SameBounds(discard, Bounds(discard_)), "Wheel moved footer actions");
        Require(IsWindowEnabled(save_) == saveEnabled, "Wheel marked clean settings dirty");
    }
    void BackgroundWinsOverFocusedInnerControls() {
        const auto page = PrepareAnalysis(); const auto list = ClassControl(page, L"LISTBOX");
        const auto details = ClassControl(page, L"EDIT"); PageEdge(page, SB_TOP); Focus(list);
        const auto selected = SendMessageW(list, LB_GETCURSEL, 0, 0);
        const auto top = SendMessageW(list, LB_GETTOPINDEX, 0, 0);
        Wheel(list, -120, Background(page), true);
        if (wheelLines_) Require(Position(page) > 0, "Queued background wheel was consumed by focused analyzer list");
        Require(SendMessageW(list, LB_GETTOPINDEX, 0, 0) == top, "Background wheel scrolled focused list instead of page");
        Require(SendMessageW(list, LB_GETCURSEL, 0, 0) == selected, "Background wheel changed list selection");
        PageEdge(page, SB_TOP); Focus(details);
        const auto first = SendMessageW(details, EM_GETFIRSTVISIBLELINE, 0, 0);
        Wheel(details, -120, Background(page));
        if (wheelLines_) Require(Position(page) > 0, "Background wheel was consumed by focused inspector");
        Require(SendMessageW(details, EM_GETFIRSTVISIBLELINE, 0, 0) == first, "Background wheel scrolled focused inspector");
        Require(GetFocus() == details, "Background scrolling changed inspector focus");
    }
    void ClosedComboPreservesSelection() {
        const auto page = PrepareAnalysis(); PageEdge(page, SB_TOP);
        const auto combo = ClassControl(page, L"COMBOBOX"); Focus(combo);
        SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
        const auto selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        Wheel(combo, -120, Center(combo));
        Require(SendMessageW(combo, CB_GETCURSEL, 0, 0) == selected, "Closed combo changed selection on page wheel");
        if (wheelLines_) Require(Position(page) > 0, "Closed combo did not route wheel to its page");
        Require(GetFocus() == combo, "Closed combo wheel changed focus");
    }
    void NativeInnerScrolling() {
        const auto page = PrepareAnalysis(); PageEdge(page, SB_TOP);
        const auto list = ClassControl(page, L"LISTBOX"); Focus(list);
        Wheel(list, -120, Center(list));
        if (wheelLines_) Require(SendMessageW(list, LB_GETTOPINDEX, 0, 0) > 0, "Analyzer list lost native inner scrolling");
        Require(Position(page) == 0, "Scrolling inside a movable list also moved the page");
        PageEdge(page, SB_BOTTOM); const int bottom = Position(page);
        const auto details = ClassControl(page, L"EDIT"); Focus(details);
        Wheel(details, -120, Center(details));
        if (wheelLines_) Require(SendMessageW(details, EM_GETFIRSTVISIBLELINE, 0, 0) > 0, "Inspector lost native inner scrolling");
        Require(Position(page) == bottom, "Scrolling inside a movable inspector also moved the page");
    }
    void OpenDropdownKeepsNativeHandling() {
        const auto page = PrepareAnalysis(); PageEdge(page, SB_TOP);
        const auto combo = ClassControl(page, L"COMBOBOX"); Focus(combo);
        for (int i = 0; i < 100; ++i) {
            const auto item = std::format(L"Synthetic dropdown row {}", i);
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        }
        const auto bounds = Bounds(combo);
        SetWindowPos(combo, nullptr, 0, 0, bounds.right - bounds.left, 72,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);
        COMBOBOXINFO info{sizeof(COMBOBOXINFO)};
        if (!GetComboBoxInfo(combo, &info) || !info.hwndList) {
            SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
            throw std::runtime_error("Open combo did not expose its native popup");
        }
        // Windows may clamp the popup onto a monitor; move it before pumping.
        SetWindowPos(info.hwndList, HWND_BOTTOM, -12100, -12200, 200, 80, SWP_NOACTIVATE);
        try {
            Require(!foreground_ || GetForegroundWindow() == foreground_, "Opening diagnostic dropdown stole foreground activation");
            Require(SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != 0, "Combo dropdown did not remain open");
            const auto selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
            const auto top = SendMessageW(info.hwndList, LB_GETTOPINDEX, 0, 0);
            Wheel(combo, -120, Center(info.hwndList));
            Require(Position(page) == 0, "Open dropdown wheel scrolled the containing page");
            Require(SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != 0, "Open dropdown wheel closed the popup");
            Wheel(info.hwndList, -120, Center(info.hwndList));
            Require(Position(page) == 0, "Native dropdown list wheel scrolled the containing page");
            if (wheelLines_) Require(SendMessageW(combo, CB_GETCURSEL, 0, 0) != selected
                || SendMessageW(info.hwndList, LB_GETTOPINDEX, 0, 0) != top, "Open dropdown list lost native wheel handling");
        } catch (...) { SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0); throw; }
        SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
    }
    void InnerEdgesFallBackToPage() {
        const auto page = PrepareAnalysis(); PageEdge(page, SB_TOP);
        const auto list = ClassControl(page, L"LISTBOX"); Focus(list);
        SendMessageW(list, LB_SETTOPINDEX, Maximum(list), 0);
        Require(Position(list) >= Maximum(list), "List fixture is not at its bottom edge");
        const auto listTop = SendMessageW(list, LB_GETTOPINDEX, 0, 0);
        Wheel(list, -120, Center(list));
        const int listFallback = Position(page);
        if (wheelLines_) Require(Position(page) > 0, "List bottom edge did not fall back to page scrolling");
        Require(SendMessageW(list, LB_GETTOPINDEX, 0, 0) == listTop, "List edge fallback moved an already exhausted inner list");
        PageEdge(page, SB_TOP); Wheel(page, -120, Background(page));
        Require(Position(page) == listFallback, "List edge fallback scrolled page more than once");
        PageEdge(page, SB_BOTTOM); const int bottom = Position(page);
        const auto details = ClassControl(page, L"EDIT");
        SendMessageW(details, EM_LINESCROLL, 0, -10000); Focus(details);
        Require(SendMessageW(details, EM_GETFIRSTVISIBLELINE, 0, 0) == 0, "Inspector fixture is not at its top edge");
        Wheel(details, 120, Center(details));
        const int detailFallback = Position(page);
        if (wheelLines_) Require(Position(page) < bottom, "Inspector top edge did not fall back to page scrolling");
        PageEdge(page, SB_BOTTOM); Wheel(page, 120, Background(page));
        Require(Position(page) == detailFallback, "Inspector edge fallback scrolled page more than once");
        Require(GetFocus() == details, "Inner edge fallback changed focus");
    }
private:
    TrayApp app_; MainWindow window_;
    HWND root_ = nullptr, foreground_ = nullptr, previousFocus_ = nullptr, save_ = nullptr, discard_ = nullptr;
    UINT wheelLines_ = 3;
    HWND Navigate(const wchar_t* caption) {
        const auto button = CaptionControl(root_, caption);
        SendMessageW(root_, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(button), BN_CLICKED), reinterpret_cast<LPARAM>(button));
        Pump(root_);
        for (HWND child = GetWindow(root_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            if (ClassIs(child, L"PlaycastCompanion.SidebarPage") && IsWindowVisible(child)) return child;
        throw std::runtime_error("Navigation did not show a page");
    }
    HWND PrepareAnalysis() {
        const auto page = Navigate(L"Analyze logs");
        const auto list = ClassControl(page, L"LISTBOX");
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (int i = 0; i < 500; ++i) {
            const auto text = std::format(L"Synthetic public-control wheel fixture row {}", i);
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        }
        SendMessageW(list, LB_SETCURSEL, 0, 0); SendMessageW(list, LB_SETTOPINDEX, 0, 0);
        std::wstring text;
        for (int i = 0; i < 300; ++i) text += std::format(L"Synthetic inspector line {}\r\n", i);
        SetWindowTextW(ClassControl(page, L"EDIT"), text.c_str()); Pump(root_); return page;
    }
    void Focus(HWND control) {
        SetFocus(control); Require(GetFocus() == control, "Cannot establish test control keyboard focus");
        Require(!foreground_ || GetForegroundWindow() == foreground_, "Diagnostic focus stole foreground activation");
    }
    void Wheel(HWND receiver, int delta, POINT point, bool queued = false) {
        const auto focus = GetFocus(); const auto word = MAKEWPARAM(0, static_cast<WORD>(delta));
        const auto coordinates = MAKELPARAM(static_cast<SHORT>(point.x), static_cast<SHORT>(point.y));
        if (queued) Require(PostMessageW(receiver, WM_MOUSEWHEEL, word, coordinates) != FALSE, "Cannot queue wheel input");
        else SendMessageW(receiver, WM_MOUSEWHEEL, word, coordinates);
        Pump(root_);
        Require(GetFocus() == focus, "Wheel input changed keyboard focus");
        Require(!foreground_ || GetForegroundWindow() == foreground_, "Wheel input stole foreground activation");
    }
};
}

int RunUiScrollContracts(HINSTANCE instance) {
    TrayApp::SetHarnessMode(true);
    try {
        ScrollFixture fixture(instance); int failed = 0;
        const std::pair<const char*, void (ScrollFixture::*)()> contracts[] = {
            {"fractional and multiple detents", &ScrollFixture::FractionalAndMultipleDetents},
            {"direct settings child and fixed footer", &ScrollFixture::DirectSettingsChildAndFooter},
            {"background routing with focused analyzer controls", &ScrollFixture::BackgroundWinsOverFocusedInnerControls},
            {"closed combo selection", &ScrollFixture::ClosedComboPreservesSelection},
            {"native inner control scrolling", &ScrollFixture::NativeInnerScrolling},
            {"open dropdown keeps native handling", &ScrollFixture::OpenDropdownKeepsNativeHandling},
            {"inner edges fall back to page", &ScrollFixture::InnerEdgesFallBackToPage}};
        for (const auto& [name, method] : contracts) {
            try { (fixture.*method)(); std::fprintf(stdout, "PASS %s\n", name); }
            catch (const std::exception& error) {
                ++failed; std::fprintf(stderr, "FAIL %s: %s\n", name, error.what());
                LogInfo(std::string("UI scroll contract failed: ") + name + ": " + error.what());
            }
        }
        std::fprintf(stdout, "UI scroll contracts: %zu total, %d failed\n", std::size(contracts), failed);
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL UI scroll setup: %s\n", error.what());
        LogInfo(std::string("UI scroll setup failed: ") + error.what()); return 1;
    }
}
} // namespace pc
