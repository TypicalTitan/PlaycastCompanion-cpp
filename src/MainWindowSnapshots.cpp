#include "pch.h"
#include "MainWindowInternal.h"
#include "Log.h"

namespace pc {
namespace {
struct WindowDc {
    HWND Window; HDC Dc;
    explicit WindowDc(HWND window) : Window(window), Dc(GetDC(window)) {}
    ~WindowDc() { if (Dc) ReleaseDC(Window, Dc); }
};
struct MemoryDc {
    HDC Dc;
    explicit MemoryDc(HDC source) : Dc(CreateCompatibleDC(source)) {}
    ~MemoryDc() { if (Dc) DeleteDC(Dc); }
};
std::optional<CLSID> PngEncoder() {
    UINT count = 0, bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || !bytes) return std::nullopt;
    std::vector<std::byte> storage(bytes);
    auto codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) return std::nullopt;
    for (UINT i = 0; i < count; ++i) if (wcscmp(codecs[i].MimeType, L"image/png") == 0) return codecs[i].Clsid;
    return std::nullopt;
}
void Repaint(HWND window) {
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}
}
bool MainWindow::Impl::CapturePng(const std::wstring& path) {
    RECT client{}; GetClientRect(hwnd, &client);
    WindowDc window(hwnd); MemoryDc memory(window.Dc);
    if (!window.Dc || !memory.Dc || client.right <= 0 || client.bottom <= 0) return false;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = -client.bottom; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    ui::GdiOwner<HBITMAP> image(CreateDIBSection(window.Dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0));
    if (!image.Get() || !pixels) return false;
    ui::SelectObjectScope selected(memory.Dc, image.Get()); Repaint(hwnd);
    PaintMain(memory.Dc, client);
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if ((GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) == 0) continue;
        RECT rectangle{}; GetWindowRect(child, &rectangle); MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&rectangle), 2);
        const int saved = SaveDC(memory.Dc); SetViewportOrgEx(memory.Dc, rectangle.left, rectangle.top, nullptr);
        IntersectClipRect(memory.Dc, 0, 0, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(memory.Dc), PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        RestoreDC(memory.Dc, saved);
    }
    Gdiplus::Bitmap bitmap(client.right, client.bottom, client.right * 4, PixelFormat32bppRGB, static_cast<BYTE*>(pixels));
    const auto codec = PngEncoder();
    return codec && bitmap.Save(path.c_str(), &*codec, nullptr) == Gdiplus::Ok;
}
void MainWindow::Impl::SaveSnapshots(const std::wstring& directory) {
    const std::filesystem::path destination(directory); std::filesystem::create_directories(destination);
    const auto originalPage = selectedPage; const auto originalTab = analysisTab; RECT original{}; GetWindowRect(hwnd, &original);
    constexpr const wchar_t* slugs[] = {L"overview", L"lighting", L"discord", L"session-logs", L"analyze-logs", L"settings"};
    auto capture = [&](const std::wstring& filename) {
        if (!CapturePng((destination / filename).wstring())) LogInfo(L"UI snapshot could not be saved: " + filename);
    };
    for (int page = 0; page < ui::PageCount; ++page) {
        offsets[page] = 0; SelectPage(page); capture(std::format(L"native-{}.png", slugs[page]));
    }
    SelectPage(ui::Lighting); advanced = true; Layout(); capture(L"native-lighting-advanced.png"); advanced = false;
    SelectPage(ui::Settings); offsets[ui::Settings] = contentHeights[ui::Settings]; Layout(); capture(L"native-settings-limits.png");
    SelectPage(ui::AnalyzeLogs);
    for (int view = 0; view < 5; ++view) {
        analysisTab = view; offsets[ui::AnalyzeLogs] = 0;
        SendMessageW(controls.at(ui::AnalysisView).Window, CB_SETCURSEL, view, 0);
        RefreshAnalysis(); Layout(); capture(std::format(L"native-analysis-{}.png", ui::AnalysisNames[view]));
    }
    RECT compact{0, 0, Scale(760), Scale(640)}; AdjustWindowRectExForDpi(&compact, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    SetWindowPos(hwnd, nullptr, 0, 0, compact.right - compact.left, compact.bottom - compact.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SelectPage(ui::Lighting); capture(L"native-small-lighting.png");
    SelectPage(ui::AnalyzeLogs); analysisTab = ui::Performance; RefreshAnalysis(); Layout(); capture(L"native-small-performance.png");
    ScrollPage(ui::AnalyzeLogs, SB_BOTTOM); capture(L"native-small-performance-details.png");
    SetWindowPos(hwnd, nullptr, original.left, original.top, original.right - original.left, original.bottom - original.top, SWP_NOZORDER | SWP_NOACTIVATE);
    analysisTab = originalTab; SendMessageW(controls.at(ui::AnalysisView).Window, CB_SETCURSEL, originalTab, 0); RefreshAnalysis();
    SelectPage(originalPage);
}
} // namespace pc
