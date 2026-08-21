#pragma once
// Palette + the original generated app mark. The project ships NO vendor
// logos, fonts, or Playcast artwork: cards use plain brand names in brand
// colours and drawn marks (colours and nominative names are not protectable).
#include <gdiplus.h>
#include <windows.h>

namespace pc::brand {
// Header / theme
constexpr COLORREF Navy = RGB(29, 52, 77);
constexpr COLORREF UiBack = RGB(16, 16, 16);
constexpr COLORREF UiSurface = RGB(30, 30, 30);
constexpr COLORREF UiBorder = RGB(45, 45, 45);
constexpr COLORREF UiText = RGB(225, 225, 225);
constexpr COLORREF UiTextDim = RGB(150, 150, 150);
constexpr COLORREF Gold = RGB(246, 197, 72);
// Status dots
constexpr COLORREF Green = RGB(0, 190, 110);
constexpr COLORREF Amber = RGB(235, 150, 30);
constexpr COLORREF Slate = RGB(120, 130, 145);
// Vendor card palettes
constexpr COLORREF RazerGreen = RGB(68, 214, 44);
constexpr COLORREF RazerBlack = RGB(0, 0, 0);
constexpr COLORREF RazerSurface = RGB(34, 34, 34);
constexpr COLORREF RazerText = RGB(204, 204, 204);
constexpr COLORREF RazerTextDim = RGB(153, 153, 153);
constexpr COLORREF SteelOrange = RGB(255, 82, 0);
constexpr COLORREF SteelDark = RGB(20, 22, 25);
constexpr COLORREF LogiBlue = RGB(0, 184, 252);
constexpr COLORREF LogiDark = RGB(13, 16, 20);
constexpr COLORREF CorsairYellow = RGB(236, 232, 26);
constexpr COLORREF CorsairBlack = RGB(10, 10, 10);
constexpr COLORREF OpenRgbDark = RGB(23, 25, 28);
constexpr COLORREF WindowsBlue = RGB(0, 120, 212);
constexpr COLORREF CardLightText = RGB(200, 205, 211);

/// Original app mark: navy disc, gold ring, gold centre dot. Caller owns the
/// returned bitmap. GDI+ must already be started.
Gdiplus::Bitmap* MakeAppMark(int size);

/// 32x32-style icon from the mark; brightness 1.0 = normal, lower = dimmed
/// "stealth" variant. Caller DestroyIcon()s.
HICON MakeAppIcon(int size, float brightness);

/// Card logo painters (original drawn marks). Draw within `bounds`.
void PaintRazerMark(Gdiplus::Graphics& g, const Gdiplus::Rect& bounds);
void PaintSteelSeriesMark(Gdiplus::Graphics& g, const Gdiplus::Rect& bounds);
void PaintLogitechMark(Gdiplus::Graphics& g, const Gdiplus::Rect& bounds);
void PaintCorsairMark(Gdiplus::Graphics& g, const Gdiplus::Rect& bounds);
void PaintOpenRgbMark(Gdiplus::Graphics& g, const Gdiplus::Rect& bounds);
void PaintWindowsMark(Gdiplus::Graphics& g, const Gdiplus::Rect& bounds);

inline Gdiplus::Color ToGdi(COLORREF c, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}
}  // namespace pc::brand
