#include "pch.h"
#include "Branding.h"

#include <memory>

// Port of Branding.cs (the generated app mark / icon) and the six drawn brand
// marks from MainForm.cs. No vendor artwork is embedded: every mark is an
// original drawing in the vendor's colours.
//
// The C# painters used fixed pixel offsets inside a 60x60 panel. Here every
// offset is multiplied by k = bounds.Width / 60 so the marks scale with DPI
// (identical output at 96 DPI, where k == 1).

namespace pc::brand {
namespace {

constexpr float kMarkDesignSize = 60.0f;  // the C# logo panel was 60x60

float ScaleOf(const Gdiplus::Rect& bounds) {
    return bounds.Width > 0 ? static_cast<float>(bounds.Width) / kMarkDesignSize : 1.0f;
}

Gdiplus::RectF InflateF(const Gdiplus::Rect& r, float dx, float dy) {
    // Rectangle.Inflate(r, -8, -8): shrink by 8 px on every side.
    return Gdiplus::RectF(static_cast<float>(r.X) - dx, static_cast<float>(r.Y) - dy,
                          static_cast<float>(r.Width) + 2 * dx, static_cast<float>(r.Height) + 2 * dy);
}

}  // namespace

// ---------------------------------------------------------------------------
// App mark / icon (Branding.DrawFallbackMark + Branding.MakeSquidIcon)
// ---------------------------------------------------------------------------

Gdiplus::Bitmap* MakeAppMark(int size) {
    if (size < 1) size = 1;
    // Caller owns the bitmap (see Branding.h).
    auto* bmp = new Gdiplus::Bitmap(size, size, PixelFormat32bppARGB);
    Gdiplus::Graphics g(bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    const float s = static_cast<float>(size);
    const float pad = s * 0.06f;
    Gdiplus::SolidBrush navy(ToGdi(Navy));
    g.FillEllipse(&navy, pad, pad, s - 2 * pad, s - 2 * pad);
    Gdiplus::Pen ring(ToGdi(Gold), s * 0.09f);
    g.DrawEllipse(&ring, s * 0.26f, s * 0.26f, s * 0.48f, s * 0.48f);
    Gdiplus::SolidBrush dot(ToGdi(Gold));
    g.FillEllipse(&dot, s * 0.42f, s * 0.42f, s * 0.16f, s * 0.16f);
    return bmp;
}

HICON MakeAppIcon(int size, float brightness) {
    if (size < 1) size = 32;
    // Render the mark at a comfortable resolution and scale it down with a
    // high-quality filter, like the C# version (256 px mark -> 32 px icon).
    std::unique_ptr<Gdiplus::Bitmap> mark(MakeAppMark(256));
    Gdiplus::Bitmap icon(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&icon);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        const INT srcW = static_cast<INT>(mark->GetWidth());
        const INT srcH = static_cast<INT>(mark->GetHeight());
        if (brightness >= 1.0f) {
            g.DrawImage(mark.get(), Gdiplus::Rect(0, 0, size, size), 0, 0, srcW, srcH, Gdiplus::UnitPixel);
        } else {
            // Scale R, G and B by `brightness`; alpha untouched ("stealth" icon).
            const float b = brightness < 0.0f ? 0.0f : brightness;
            Gdiplus::ColorMatrix matrix = {{{b, 0, 0, 0, 0},
                                            {0, b, 0, 0, 0},
                                            {0, 0, b, 0, 0},
                                            {0, 0, 0, 1, 0},
                                            {0, 0, 0, 0, 1}}};
            Gdiplus::ImageAttributes attrs;
            attrs.SetColorMatrix(&matrix, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
            g.DrawImage(mark.get(), Gdiplus::Rect(0, 0, size, size), 0, 0, srcW, srcH, Gdiplus::UnitPixel, &attrs);
        }
    }
    HICON handle = nullptr;
    if (icon.GetHICON(&handle) != Gdiplus::Ok) return nullptr;
    return handle;
}

// ---------------------------------------------------------------------------
// Card marks (MainForm.Paint*Mark)
// ---------------------------------------------------------------------------

void PaintRazerMark(Gdiplus::Graphics& g, const Gdiplus::Rect& bounds) {
    // Three green blades fanning up from a common base.
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float k = ScaleOf(bounds);
    Gdiplus::Pen pen(ToGdi(RazerGreen), 5.0f * k);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapTriangle);
    const float cx = static_cast<float>(bounds.X) + static_cast<float>(bounds.Width) / 2.0f;
    const float top = static_cast<float>(bounds.Y);
    const float bottom = static_cast<float>(bounds.GetBottom()) - 8.0f * k;
    g.DrawBezier(&pen, cx, bottom, cx - 14 * k, bottom - 18 * k, cx - 16 * k, bottom - 34 * k, cx - 22 * k, top + 8 * k);
    g.DrawBezier(&pen, cx, bottom, cx, bottom - 20 * k, cx, bottom - 38 * k, cx, top + 4 * k);
    g.DrawBezier(&pen, cx, bottom, cx + 14 * k, bottom - 18 * k, cx + 16 * k, bottom - 34 * k, cx + 22 * k, top + 8 * k);
}

void PaintSteelSeriesMark(Gdiplus::Graphics& g, const Gdiplus::Rect& bounds) {
    // Orange ring with a white diagonal blade.
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float k = ScaleOf(bounds);
    const Gdiplus::RectF rect = InflateF(bounds, -8 * k, -8 * k);
    Gdiplus::Pen pen(ToGdi(SteelOrange), 6.0f * k);
    g.DrawEllipse(&pen, rect);
    Gdiplus::Pen blade(Gdiplus::Color(255, 255, 255, 255), 5.0f * k);
    blade.SetStartCap(Gdiplus::LineCapRound);
    blade.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&blade, rect.X + 10 * k, rect.GetBottom() - 10 * k, rect.GetRight() - 10 * k, rect.Y + 10 * k);
}

void PaintLogitechMark(Gdiplus::Graphics& g, const Gdiplus::Rect& bounds) {
    // Blue ring around a bold white "G".
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    const float k = ScaleOf(bounds);
    const Gdiplus::RectF rect = InflateF(bounds, -6 * k, -6 * k);
    Gdiplus::Pen pen(ToGdi(LogiBlue), 4.5f * k);
    g.DrawEllipse(&pen, rect);
    // 22 pt at 96 DPI == 22 * 96 / 72 px; sized in pixels so the DC's DPI is
    // not applied a second time on top of k.
    Gdiplus::Font font(L"Segoe UI", 22.0f * (96.0f / 72.0f) * k, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::RectF box;
    g.MeasureString(L"G", 1, &font, Gdiplus::PointF(0.0f, 0.0f), &box);
    Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
    g.DrawString(L"G", 1, &font,
                 Gdiplus::PointF(rect.X + (rect.Width - box.Width) / 2 + 1 * k,
                                 rect.Y + (rect.Height - box.Height) / 2 + 1 * k),
                 &white);
}

void PaintCorsairMark(Gdiplus::Graphics& g, const Gdiplus::Rect& bounds) {
    // Three yellow slanted triangles (sails).
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float k = ScaleOf(bounds);
    Gdiplus::SolidBrush brush(ToGdi(CorsairYellow));
    const float left = static_cast<float>(bounds.X);
    const float right = static_cast<float>(bounds.GetRight());
    for (int i = 0; i < 3; ++i) {
        const float fi = static_cast<float>(i);
        const float y = static_cast<float>(bounds.Y) + (10 + i * 15) * k;
        const Gdiplus::PointF points[3] = {
            Gdiplus::PointF(left + (8 + fi * 5) * k, y + 12 * k),
            Gdiplus::PointF(right - 12 * k, y),
            Gdiplus::PointF(left + (20 + fi * 5) * k, y + 16 * k),
        };
        g.FillPolygon(&brush, points, 3);
    }
}

void PaintOpenRgbMark(Gdiplus::Graphics& g, const Gdiplus::Rect& bounds) {
    // Three overlapping translucent R / G / B discs.
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float k = ScaleOf(bounds);
    Gdiplus::SolidBrush red(Gdiplus::Color(210, 235, 60, 60));
    Gdiplus::SolidBrush green(Gdiplus::Color(210, 60, 220, 90));
    Gdiplus::SolidBrush blue(Gdiplus::Color(210, 70, 110, 245));
    const float d = 30.0f * k;
    const float left = static_cast<float>(bounds.X);
    const float top = static_cast<float>(bounds.Y);
    g.FillEllipse(&red, left + 15 * k, top + 4 * k, d, d);
    g.FillEllipse(&green, left + 4 * k, top + 24 * k, d, d);
    g.FillEllipse(&blue, left + 26 * k, top + 24 * k, d, d);
}

void PaintWindowsMark(Gdiplus::Graphics& g, const Gdiplus::Rect& bounds) {
    // Four blue squares in a 2x2 grid, centred.
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float k = ScaleOf(bounds);
    Gdiplus::SolidBrush brush(ToGdi(WindowsBlue));
    const float s = 26.0f * k;
    const float gap = 4.0f * k;
    const float x = static_cast<float>(bounds.X) + (static_cast<float>(bounds.Width) - (2 * s + gap)) / 2;
    const float y = static_cast<float>(bounds.Y) + (static_cast<float>(bounds.Height) - (2 * s + gap)) / 2;
    g.FillRectangle(&brush, x, y, s, s);
    g.FillRectangle(&brush, x + s + gap, y, s, s);
    g.FillRectangle(&brush, x, y + s + gap, s, s);
    g.FillRectangle(&brush, x + s + gap, y + s + gap, s, s);
}

}  // namespace pc::brand
