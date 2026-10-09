#include "pch.h"
#include "MainWindowInternal.h"
#include "TrayApp.h"
#include "Log.h"
#include <cwctype>

namespace pc::ui {
std::wstring ReadText(HWND window) {
    const auto length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<size_t>(std::max(0, length)) + 1, L'\0');
    const auto count = GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<size_t>(std::max(0, count)));
    return text;
}
std::wstring Trim(std::wstring_view text) {
    const auto first = text.find_first_not_of(L" \r\n\t"), last = text.find_last_not_of(L" \r\n\t");
    return first == std::wstring_view::npos ? L"" : std::wstring(text.substr(first, last - first + 1));
}
void Fill(HDC dc, RECT rectangle, COLORREF color) {
    GdiOwner<HBRUSH> brush(CreateSolidBrush(color)); FillRect(dc, &rectangle, brush.Get());
}
void DrawText(HDC dc, const std::wstring& text, RECT rectangle, HFONT font, COLORREF color, UINT flags) {
    SelectObjectScope selected(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rectangle, flags);
}
} // namespace pc::ui

namespace pc {
MainWindow::Impl::Impl(TrayApp& owner) : app(owner), draft(owner.Config()) {
    backBrush.Reset(CreateSolidBrush(ui::Back)); surfaceBrush.Reset(CreateSolidBrush(ui::Surface));
}
MainWindow::Impl::~Impl() {
    importThread.request_stop();
    if (importThread.joinable()) importThread.join();
    if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
}
int MainWindow::Impl::Scale(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }
void MainWindow::Impl::BuildFonts() {
    auto make = [this](int points, int weight, const wchar_t* face) {
        return CreateFontW(-MulDiv(points, static_cast<int>(dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    baseFont.Reset(make(10, FW_NORMAL, L"Segoe UI")); titleFont.Reset(make(19, FW_SEMIBOLD, L"Segoe UI"));
    smallFont.Reset(make(9, FW_NORMAL, L"Segoe UI")); monoFont.Reset(make(9, FW_NORMAL, L"Consolas"));
    for (const auto& [id, control] : controls) {
        const auto font = id == ui::AnalysisDetails ? monoFont.Get() : baseFont.Get();
        SendMessageW(control.Window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}
void MainWindow::Impl::Create() {
    if (hwnd && IsWindow(hwnd)) return;
    WNDCLASSEXW window{sizeof(WNDCLASSEXW)};
    window.hInstance = app.Instance(); window.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window.lpfnWndProc = WindowProc; window.lpszClassName = L"PlaycastCompanion.SidebarWindow";
    window.hIcon = app.AppIcon(); window.hIconSm = app.AppIcon(); RegisterClassExW(&window);
    window.lpfnWndProc = PageProc; window.lpszClassName = L"PlaycastCompanion.SidebarPage"; RegisterClassExW(&window);
    dpi = GetDpiForSystem(); BuildFonts();
    RECT rectangle{0, 0, Scale(1000), Scale(760)};
    AdjustWindowRectExForDpi(&rectangle, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    hwnd = CreateWindowExW(0, L"PlaycastCompanion.SidebarWindow", L"Playcast Companion", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
        nullptr, nullptr, app.Instance(), this);
    if (!hwnd) throw std::runtime_error("Cannot create Companion window");
    BOOL dark = TRUE; DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    BuildPages(); LoadSettings(); Layout(); RefreshStatus();
    SetTimer(hwnd, 1, 1000, nullptr);
}
void MainWindow::Impl::Layout() {
    if (!hwnd) return;
    RECT client{}; GetClientRect(hwnd, &client);
    const int width = MulDiv(client.right, 96, static_cast<int>(dpi));
    const int height = MulDiv(client.bottom, 96, static_cast<int>(dpi));
    const int sidebar = width < 820 ? 150 : 174;
    contentHeights[ui::Lighting] = advanced ? 710 : 500;
    for (int index = 0; index < ui::PageCount; ++index) {
        const int y = index == ui::Settings ? height - 112 : 108 + index * 46;
        MoveWindow(controls.at(ui::NavBase + index).Window, Scale(12), Scale(y), Scale(sidebar - 24), Scale(42), TRUE);
        MoveWindow(pages[index], Scale(sidebar + 28), Scale(20), Scale(width - sidebar - 54), Scale(height - 92), TRUE);
        RECT page{}; GetClientRect(pages[index], &page);
        const int available = MulDiv(page.bottom, 96, static_cast<int>(dpi));
        offsets[index] = std::clamp(offsets[index], 0, std::max(0, contentHeights[index] - available));
        SCROLLINFO scroll{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, contentHeights[index] - 1),
            static_cast<UINT>(available), offsets[index], 0}; SetScrollInfo(pages[index], SB_VERT, &scroll, TRUE);
    }
    for (const auto& [id, control] : controls) {
        if (id >= ui::NavBase && id < ui::NavBase + static_cast<int>(ui::PageCount)) continue;
        if (control.Page < 0) continue;
        if (id == ui::AnalysisView || id == ui::AnalysisList || id == ui::AnalysisDetails || id == ui::AnalysisFilter
            || id == ui::NextPage || id == ui::PreviousPage || control.Window == importSummary) continue;
        if (control.Page == ui::Lighting && control.Y >= 500) ShowWindow(control.Window, advanced ? SW_SHOW : SW_HIDE);
        RECT page{}; GetClientRect(pages[control.Page], &page);
        const int pageWidth = MulDiv(page.right, 96, static_cast<int>(dpi));
        const int controlWidth = control.W < 0 ? std::max(40, pageWidth - control.X - 10) : std::min(control.W, pageWidth - control.X - 10);
        MoveWindow(control.Window, Scale(control.X), Scale(control.Y - offsets[control.Page]), Scale(controlWidth), Scale(control.H), TRUE);
    }
    MoveWindow(controls.at(ui::Save).Window, Scale(width - 154), Scale(height - 45), Scale(136), Scale(32), TRUE);
    MoveWindow(controls.at(ui::Discard).Window, Scale(width - 264), Scale(height - 45), Scale(98), Scale(32), TRUE);
    MoveWindow(feedback, Scale(18), Scale(height - 43), Scale(std::max(160, width - 310)), Scale(30), TRUE);
    LayoutAnalysis(); InvalidateRect(hwnd, nullptr, FALSE);
}
void MainWindow::Impl::SelectPage(int page) {
    if (page < 0 || page >= ui::PageCount) return;
    selectedPage = page;
    for (int index = 0; index < ui::PageCount; ++index) {
        ShowWindow(pages[index], index == page ? SW_SHOW : SW_HIDE);
        InvalidateRect(controls.at(ui::NavBase + index).Window, nullptr, TRUE);
    }
    RefreshStatus(); Layout();
}
void MainWindow::Impl::ScrollPage(int page, int code, int position) {
    RECT rectangle{}; GetClientRect(pages[page], &rectangle);
    const int height = MulDiv(rectangle.bottom, 96, static_cast<int>(dpi));
    int next = offsets[page];
    if (code == SB_LINEUP) next -= 30;
    if (code == SB_LINEDOWN) next += 30;
    if (code == SB_PAGEUP) next -= height;
    if (code == SB_PAGEDOWN) next += height;
    if (code == SB_THUMBTRACK || code == SB_THUMBPOSITION) next = position;
    if (code == SB_TOP) next = 0;
    if (code == SB_BOTTOM) next = contentHeights[page];
    offsets[page] = std::clamp(next, 0, std::max(0, contentHeights[page] - height)); Layout();
    InvalidateRect(pages[page], nullptr, TRUE);
}
LRESULT CALLBACK MainWindow::Impl::WindowProc(HWND window, UINT message, WPARAM word, LPARAM parameter) {
    auto self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(parameter)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->hwnd = window;
    }
    try { return self ? self->Message(window, message, word, parameter, -1) : DefWindowProcW(window, message, word, parameter); }
    catch (...) { LogInfo(L"Companion UI operation failed"); return 0; }
}
LRESULT CALLBACK MainWindow::Impl::PageProc(HWND window, UINT message, WPARAM word, LPARAM parameter) {
    auto context = reinterpret_cast<PageContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        context = static_cast<PageContext*>(reinterpret_cast<CREATESTRUCTW*>(parameter)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
    }
    try { return context ? context->Owner->Message(window, message, word, parameter, context->Index) : DefWindowProcW(window, message, word, parameter); }
    catch (...) { LogInfo(L"Companion page operation failed"); return 0; }
}
LRESULT MainWindow::Impl::Message(HWND window, UINT message, WPARAM word, LPARAM parameter, int page) {
    switch (message) {
    case WM_CLOSE:
        if (dirty && MessageBoxW(hwnd, L"Discard unsaved changes and hide the window?", L"Unsaved settings", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        if (dirty) { draft = savedDraft; LoadSettings(false); } ShowWindow(hwnd, SW_HIDE); return 0;
    case WM_SIZE: if (page < 0 && !controls.empty()) Layout(); return 0;
    case WM_GETMINMAXINFO: {
        auto size = reinterpret_cast<MINMAXINFO*>(parameter); size->ptMinTrackSize = {Scale(760), Scale(640)}; return 0;
    }
    case WM_DPICHANGED: {
        dpi = HIWORD(word); BuildFonts(); const auto rectangle = reinterpret_cast<RECT*>(parameter);
        SetWindowPos(hwnd, nullptr, rectangle->left, rectangle->top, rectangle->right - rectangle->left,
            rectangle->bottom - rectangle->top, SWP_NOZORDER | SWP_NOACTIVATE); Layout(); return 0;
    }
    case WM_TIMER:
        if (word == 1) RefreshStatus();
        if (word == 2) { previewing = false; KillTimer(hwnd, 2); SetWindowTextW(controls.at(ui::Preview).Window, L"Preview guest mode (10 s)"); EnableWindow(controls.at(ui::Preview).Window, TRUE); }
        return 0;
    case WM_COMMAND: Command(LOWORD(word), HIWORD(word)); return 0;
    case WM_DRAWITEM: DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(parameter)); return TRUE;
    case WM_MEASUREITEM: reinterpret_cast<MEASUREITEMSTRUCT*>(parameter)->itemHeight = Scale(24); return TRUE;
    case WM_VSCROLL: if (page >= 0 && parameter == 0) {
        SCROLLINFO info{sizeof(SCROLLINFO), SIF_TRACKPOS}; GetScrollInfo(window, SB_VERT, &info);
        const int command = LOWORD(word); int position = info.nTrackPos;
        if ((command == SB_THUMBTRACK || command == SB_THUMBPOSITION) && static_cast<WORD>(position) != HIWORD(word)) position = HIWORD(word);
        ScrollPage(page, command, position);
    } return 0;
    case WM_MOUSEWHEEL:
        if (nativeWheelRecipient || RouteWheel(window, word, parameter)) return 0;
        return DefWindowProcW(window, message, word, parameter);
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
        const auto dc = reinterpret_cast<HDC>(word); SetTextColor(dc, message == WM_CTLCOLORSTATIC ? ui::Dim : ui::Text);
        SetBkColor(dc, message == WM_CTLCOLORSTATIC ? ui::Back : ui::Surface);
        if (reinterpret_cast<HWND>(parameter) == overviewStatus) SetTextColor(dc, app.GuestActive() ? ui::Warning : ui::Green);
        return reinterpret_cast<LRESULT>(message == WM_CTLCOLORSTATIC ? backBrush.Get() : surfaceBrush.Get());
    }
    case WM_ERASEBKGND: return TRUE;
    case WM_PRINTCLIENT: {
        RECT client{}; GetClientRect(window, &client); const auto dc = reinterpret_cast<HDC>(word);
        if (page < 0) PaintMain(dc, client); else PaintPage(page, dc, client); return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{}; const auto dc = BeginPaint(window, &paint); RECT client{}; GetClientRect(window, &client);
        if (page < 0) PaintMain(dc, client); else PaintPage(page, dc, client); EndPaint(window, &paint); return 0;
    }
    case ui::ImportComplete: FinishImport(); return 0;
    default: return DefWindowProcW(window, message, word, parameter);
    }
}
MainWindow::MainWindow(TrayApp& app) : impl_(std::make_unique<Impl>(app)) {}
MainWindow::~MainWindow() = default;
void MainWindow::Show() { impl_->Create(); ShowWindow(impl_->hwnd, SW_SHOW); Activate(); }
void MainWindow::Activate() { if (impl_->hwnd) { if (IsIconic(impl_->hwnd)) ShowWindow(impl_->hwnd, SW_RESTORE); SetForegroundWindow(impl_->hwnd); } }
bool MainWindow::IsAlive() const { return impl_->hwnd && IsWindow(impl_->hwnd); }
HWND MainWindow::Handle() const { return impl_->hwnd; }
void MainWindow::SaveSnapshots(const std::wstring& directory) { impl_->Create(); impl_->SaveSnapshots(directory); }
void MainWindow::ImportLogs(const std::vector<std::filesystem::path>& sources) {
    impl_->Create(); impl_->pendingAnalysis = std::make_shared<LogAnalysisResult>(LogAnalysisImporter{}.Import(sources)); impl_->FinishImport();
}
} // namespace pc
