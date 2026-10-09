#include "pch.h"
#include "MainWindowInternal.h"

namespace pc {
namespace {
bool IsClass(HWND window, const wchar_t* name) {
    wchar_t actual[32]{}; GetClassNameW(window, actual, static_cast<int>(std::size(actual)));
    return CompareStringOrdinal(actual, -1, name, -1, TRUE) == CSTR_EQUAL;
}
bool IsOpenCombo(HWND window) {
    return IsClass(window, L"COMBOBOX") && SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0) != 0;
}
bool HasInnerScrolling(HWND window) {
    return IsClass(window, L"LISTBOX") || (IsClass(window, L"EDIT") && (GetWindowLongPtrW(window, GWL_STYLE) & ES_MULTILINE));
}
bool CanScroll(HWND window, int delta) {
    SCROLLINFO scroll{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS};
    if (!GetScrollInfo(window, SB_VERT, &scroll)) return false;
    const auto maximum = std::max(scroll.nMin, scroll.nMax - static_cast<int>(scroll.nPage ? scroll.nPage - 1 : 0));
    return delta > 0 ? scroll.nPos > scroll.nMin : scroll.nPos < maximum;
}
struct NativeWheelScope {
    HWND& Recipient;
    NativeWheelScope(HWND& recipient, HWND target) : Recipient(recipient) { Recipient = target; }
    ~NativeWheelScope() { Recipient = nullptr; }
};
}
bool MainWindow::Impl::RouteWheel(HWND receiver, WPARAM word, LPARAM screenCoordinates) {
    if (!hwnd || nativeWheelRecipient || selectedPage < 0 || selectedPage >= ui::PageCount) return false;
    if (IsOpenCombo(receiver)) return false;
    POINT point{GET_X_LPARAM(screenCoordinates), GET_Y_LPARAM(screenCoordinates)};
    RECT client{}; GetClientRect(hwnd, &client); ScreenToClient(hwnd, &point);
    if (!PtInRect(&client, point)) return false;
    const auto page = pages[selectedPage];
    MapWindowPoints(hwnd, page, &point, 1);
    HWND target = ChildWindowFromPointEx(page, point, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
    const int delta = GET_WHEEL_DELTA_WPARAM(word);
    if (!delta) return true;
    if (target && target != page && (IsOpenCombo(target) || (HasInnerScrolling(target) && CanScroll(target, delta)))) {
        NativeWheelScope forwarding(nativeWheelRecipient, target);
        SendMessageW(target, WM_MOUSEWHEEL, word, screenCoordinates);
        return true;
    }
    ScrollWheelPage(selectedPage, delta);
    return true;
}
void MainWindow::Impl::ScrollWheelPage(int page, int delta) {
    UINT lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    if (!lines) { wheelRemainders[page] = 0; return; }
    auto& remainder = wheelRemainders[page]; remainder += delta;
    const int notches = remainder / WHEEL_DELTA; remainder %= WHEEL_DELTA;
    if (!notches) return;
    RECT rectangle{}; GetClientRect(pages[page], &rectangle);
    const int height = MulDiv(rectangle.bottom, 96, static_cast<int>(dpi));
    const int64_t distance = lines == WHEEL_PAGESCROLL ? height : static_cast<int64_t>(lines) * 30;
    const auto maximum = std::max(0, contentHeights[page] - height);
    const auto position = std::clamp(static_cast<int64_t>(offsets[page]) - notches * distance, int64_t{0}, static_cast<int64_t>(maximum));
    ScrollPage(page, SB_THUMBPOSITION, static_cast<int>(position));
}
LRESULT CALLBACK MainWindow::Impl::ScrollControlProc(HWND window, UINT message, WPARAM word, LPARAM parameter, UINT_PTR id, DWORD_PTR owner) {
    auto self = reinterpret_cast<Impl*>(owner);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ScrollControlProc, id);
    if (message == WM_MOUSEWHEEL && self && self->nativeWheelRecipient != window) {
        try { if (self->RouteWheel(window, word, parameter)) return 0; } catch (...) { return 0; }
    }
    return DefSubclassProc(window, message, word, parameter);
}
} // namespace pc
