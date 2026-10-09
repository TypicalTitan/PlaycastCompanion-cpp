#include "pch.h"
#include "MainWindowInternal.h"
#include <cmath>

namespace pc {
namespace {
constexpr COLORREF Colors[] = {ui::Gold, RGB(114, 177, 247), RGB(185, 145, 244), ui::Green, RGB(232, 147, 177), ui::Dim, ui::Warning};
struct PlotRange {
    int64_t First = 0, Last = 0; bool Known = false;
    void Add(int64_t time) { if (!Known) { First = Last = time; Known = true; } else { First = std::min(First, time); Last = std::max(Last, time); } }
    int X(int64_t time, RECT rectangle) const {
        const auto span = static_cast<double>(std::max<int64_t>(1, Last - First));
        return rectangle.left + static_cast<int>((time - First) / span * (rectangle.right - rectangle.left));
    }
};
void Line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int width = 1) {
    ui::GdiOwner<HPEN> pen(CreatePen(PS_SOLID, width, color)); ui::SelectObjectScope selected(dc, pen.Get());
    MoveToEx(dc, x1, y1, nullptr); LineTo(dc, x2, y2);
}
void Gaps(HDC dc, const LogAnalysisResult& result, const PlotRange& range, RECT rectangle) {
    for (auto index : result.Gaps) {
        if (index >= result.Records.size() || !result.Records[index].TimeMs) continue;
        const auto x = range.X(*result.Records[index].TimeMs, rectangle);
        if (x >= rectangle.left && x <= rectangle.right) Line(dc, x, rectangle.top, x, rectangle.bottom, RGB(225, 111, 111));
    }
}
void Timeline(HDC dc, RECT rectangle, HFONT font, const LogAnalysisResult& result, int scale) {
    const wchar_t* names[] = {L"Realtime", L"IPC", L"Scripts", L"Games", L"Diagnostics", L"Session", L"Other"};
    const wchar_t* categories[] = {L"realtime", L"ipc", L"scripts", L"games", L"diagnostics", L"session"};
    PlotRange range; size_t unknown = 0;
    for (const auto& record : result.Records) { if (record.TimeMs) range.Add(*record.TimeMs); else ++unknown; }
    RECT plot{rectangle.left + scale * 92 / 96, rectangle.top + scale * 28 / 96, rectangle.right - scale * 14 / 96, rectangle.bottom - scale * 22 / 96};
    const int laneHeight = std::max(1L, (plot.bottom - plot.top) / 7);
    ui::DrawText(dc, std::format(L"Category timeline · {} records · {} unknown timestamps · red = capture gaps", result.Records.size(), unknown),
        {rectangle.left + 8, rectangle.top + 4, rectangle.right - 8, plot.top - 2}, font, ui::Text);
    for (int lane = 0; lane < 7; ++lane) {
        const int y = plot.top + lane * laneHeight + laneHeight / 2;
        ui::DrawText(dc, names[lane], {rectangle.left + 8, y - laneHeight / 2, plot.left - 6, y + laneHeight / 2}, font, Colors[lane]);
        Line(dc, plot.left, y, plot.right, y, ui::Border);
    }
    if (!range.Known) { ui::DrawText(dc, L"No valid capture timestamps to plot", plot, font, ui::Dim, DT_CENTER | DT_VCENTER | DT_SINGLELINE); return; }
    for (const auto& record : result.Records) {
        if (!record.TimeMs) continue;
        int lane = 6; for (int i = 0; i < 6; ++i) if (record.Category == categories[i]) { lane = i; break; }
        const int x = range.X(*record.TimeMs, plot), y = plot.top + lane * laneHeight + laneHeight / 2;
        ui::Fill(dc, {x - 2, y - 3, x + 2, y + 3}, Colors[lane]);
    }
    Gaps(dc, result, range, plot);
    ui::DrawText(dc, std::format(L"Start  ·  {:.1f} seconds captured  ·  End", (range.Last - range.First) / 1000.0),
        {plot.left, plot.bottom + 2, plot.right, rectangle.bottom - 1}, font, ui::Dim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
void MetricPlot(HDC dc, RECT rectangle, HFONT font, const LogAnalysisResult& result, const PlotRange& range, bool cpu) {
    const auto value = [cpu](const CompanionMetricSample& sample) -> std::optional<double> {
        if (cpu) return sample.CpuPercent;
        return sample.WorkingSetBytes ? std::optional(*sample.WorkingSetBytes / (1024 * 1024)) : std::nullopt;
    };
    double maximum = 1; size_t observed = 0;
    for (const auto& sample : result.Metrics) if (auto number = value(sample)) { maximum = std::max(maximum, *number); ++observed; }
    RECT plot{rectangle.left + 8, rectangle.top + 30, rectangle.right - 10, rectangle.bottom - 20};
    ui::DrawText(dc, cpu ? L"Companion CPU (% machine capacity)" : L"Companion working set (MiB)",
        {rectangle.left + 8, rectangle.top + 3, rectangle.right - 8, rectangle.top + 27}, font, ui::Text);
    for (int grid = 0; grid < 3; ++grid) { const int y = plot.top + (plot.bottom - plot.top) * grid / 2; Line(dc, plot.left, y, plot.right, y, ui::Border); }
    if (!observed || !range.Known) { ui::DrawText(dc, L"Unknown / no comparable samples", plot, font, ui::Dim, DT_CENTER | DT_VCENTER | DT_SINGLELINE); return; }
    std::map<std::wstring, POINT> previous;
    std::map<std::wstring, int64_t> previousTime;
    for (const auto& sample : result.Metrics) {
        const auto number = value(sample);
        if (!number) { previous.erase(sample.ProcessIdentity); continue; }
        const POINT point{range.X(sample.TimeMs, plot), plot.bottom - static_cast<LONG>(*number / maximum * (plot.bottom - plot.top))};
        const auto old = previous.find(sample.ProcessIdentity);
        if (!sample.ProcessIdentity.empty() && old != previous.end() && sample.TimeMs > previousTime[sample.ProcessIdentity] && sample.TimeMs - previousTime[sample.ProcessIdentity] <= 120000)
            Line(dc, old->second.x, old->second.y, point.x, point.y, cpu ? Colors[1] : ui::Gold, 2);
        ui::Fill(dc, {point.x - 2, point.y - 2, point.x + 2, point.y + 2}, cpu ? Colors[1] : ui::Gold);
        previous[sample.ProcessIdentity] = point; previousTime[sample.ProcessIdentity] = sample.TimeMs;
    }
    Gaps(dc, result, range, plot);
    ui::DrawText(dc, std::format(L"0 → {:.1f} · {} observed samples", maximum, observed), {plot.left, plot.bottom + 2, plot.right, rectangle.bottom}, font, ui::Dim);
}
}
void MainWindow::Impl::PaintAnalysisGraph(HDC dc, RECT rectangle) {
    ui::Fill(dc, rectangle, ui::Surface);
    if (!analysisResult) { ui::DrawText(dc, L"Import a session capture to see its timeline and process samples.", rectangle, smallFont.Get(), ui::Dim, DT_CENTER | DT_VCENTER | DT_SINGLELINE); return; }
    if (analysisTab == ui::Timeline) { Timeline(dc, rectangle, smallFont.Get(), *analysisResult, static_cast<int>(dpi)); return; }
    PlotRange range; for (const auto& sample : analysisResult->Metrics) range.Add(sample.TimeMs);
    const LONG middle = (rectangle.left + rectangle.right) / 2;
    MetricPlot(dc, {rectangle.left, rectangle.top, middle - Scale(5), rectangle.bottom}, smallFont.Get(), *analysisResult, range, false);
    MetricPlot(dc, {middle + Scale(5), rectangle.top, rectangle.right, rectangle.bottom}, smallFont.Get(), *analysisResult, range, true);
}
} // namespace pc
