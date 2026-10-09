#pragma once
#include "MainWindow.h"
#include "Config.h"
#include "LogAnalysis.h"
#include "LogAnalysisSample.h"
#include <array>
#include <map>
#include <mutex>
#include <thread>

namespace pc::ui {
constexpr COLORREF Back = RGB(21, 25, 31), Surface = RGB(27, 32, 40), Nav = RGB(23, 28, 35);
constexpr COLORREF Border = RGB(48, 56, 69), Text = RGB(237, 241, 247), Dim = RGB(166, 178, 196);
constexpr COLORREF Gold = RGB(246, 197, 72), Green = RGB(131, 214, 169), Warning = RGB(239, 188, 112);
enum Page { Overview, Lighting, Discord, SessionLogs, AnalyzeLogs, Settings, PageCount };
inline constexpr const wchar_t* PageNames[] = {L"Overview", L"Lighting", L"Discord", L"Session logs", L"Analyze logs", L"Settings"};
enum Id {
    Save = 200, Discard, Preview, OpenLog, OpenInstall, OpenSessions, AdvancedLighting,
    Razer = 300, Steel, Logitech, Corsair, OpenRgb, Dynamic, DiscordEnabled, Passthrough,
    Disconnected, Registry, LogEnabled, RawSecrets, ExcludeVendor,
    Tick = 400, Profile, LineOne, LineTwo, HostTemplate, Username, SnapshotInterval,
    Retention, FileBudget, SessionBudget, OpenRgbHost, OpenRgbPort,
    ImportFiles = 500, ImportFolder, AnalysisView, AnalysisList, AnalysisDetails, AnalysisFilter, NextPage, PreviousPage, LoadSample,
    NavBase = 600
};
enum AnalysisTab { Timeline, Communications, Scripts, Games, Performance };
inline constexpr const wchar_t* AnalysisNames[] = {L"Timeline", L"Communications", L"Scripts", L"Games", L"Health"};
inline constexpr UINT ImportComplete = WM_APP + 80;
template<class T> class GdiOwner {
public:
    explicit GdiOwner(T value = nullptr) : value_(value) {}
    ~GdiOwner() { if (value_) DeleteObject(value_); }
    GdiOwner(const GdiOwner&) = delete;
    GdiOwner& operator=(const GdiOwner&) = delete;
    T Get() const { return value_; }
    void Reset(T value) { if (value_) DeleteObject(value_); value_ = value; }
private: T value_;
};
struct SelectObjectScope {
    HDC Dc; HGDIOBJ Previous;
    SelectObjectScope(HDC dc, HGDIOBJ value) : Dc(dc), Previous(SelectObject(dc, value)) {}
    ~SelectObjectScope() { SelectObject(Dc, Previous); }
};
std::wstring ReadText(HWND window);
std::wstring Trim(std::wstring_view text);
void Fill(HDC dc, RECT rectangle, COLORREF color);
void DrawText(HDC dc, const std::wstring& text, RECT rectangle, HFONT font, COLORREF color, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
} // namespace pc::ui

namespace pc {
struct MainWindow::Impl {
    struct PageContext { Impl* Owner = nullptr; int Index = 0; };
    struct Control { HWND Window = nullptr; int Page = 0; int X = 0, Y = 0, W = 0, H = 0; bool Check = false, Checked = false, Gold = false; };
    TrayApp& app;
    HWND hwnd = nullptr;
    UINT dpi = 96;
    std::array<HWND, ui::PageCount> pages{};
    std::array<PageContext, ui::PageCount> contexts{};
    std::map<int, Control> controls;
    std::array<int, ui::PageCount> offsets{}, contentHeights{};
    std::array<HWND, 6> lightingStatus{};
    HWND overviewStatus = nullptr, overviewLighting = nullptr, overviewDiscord = nullptr, overviewStartup = nullptr;
    HWND loggingStatus = nullptr, loggingPath = nullptr, discordPreview = nullptr, discordStatus = nullptr;
    HWND feedback = nullptr, importSummary = nullptr, importWarning = nullptr;
    ui::GdiOwner<HFONT> baseFont, titleFont, smallFont, monoFont;
    ui::GdiOwner<HBRUSH> backBrush, surfaceBrush;
    AppConfig draft;
    AppConfig savedDraft;
    std::string savedJson;
    bool loading = false, dirty = false, advanced = false, importing = false, previewing = false;
    int selectedPage = ui::Overview, analysisTab = ui::Timeline;
    size_t analysisOffset = 0;
    std::vector<size_t> analysisRows;
    std::shared_ptr<LogAnalysisResult> analysisResult;
    std::shared_ptr<LogAnalysisResult> pendingAnalysis;
    std::mutex importMutex;
    std::jthread importThread;
    explicit Impl(TrayApp& owner);
    ~Impl();
    int Scale(int value) const;
    void Create();
    void BuildFonts();
    void BuildPages();
    void BuildOverview();
    void BuildLighting();
    void BuildDiscord();
    void BuildSessionLogs();
    void BuildSettings();
    void BuildAnalysis();
    HWND Add(int id, int page, const wchar_t* kind, const std::wstring& text, int x, int y, int width, int height, DWORD style = 0);
    HWND Label(int page, const std::wstring& text, int x, int y, int width, int height = 24);
    HWND Button(int id, int page, const std::wstring& text, int x, int y, int width, bool gold = false);
    HWND Check(int id, int page, const std::wstring& text, int x, int y, int width);
    HWND Edit(int id, int page, int x, int y, int width, bool numeric = false);
    void Layout();
    void LayoutAnalysis();
    void SelectPage(int page);
    void ScrollPage(int page, int code, int position = 0);
    void LoadSettings(bool fromRuntime = true);
    void ReadSettings();
    void SetDirty(bool value);
    void SaveSettings();
    void RefreshStatus();
    void Command(int id, int notification);
    void PaintMain(HDC dc, RECT client);
    void PaintPage(int page, HDC dc, RECT client);
    void DrawButton(const DRAWITEMSTRUCT& item);
    void PaintAnalysisGraph(HDC dc, RECT rectangle);
    void Import(bool folder);
    void BeginImport(std::vector<std::filesystem::path> sources, std::unique_ptr<TemporaryLogSample> sample = {});
    void FinishImport();
    void RefreshAnalysis();
    void SelectAnalysisRow();
    std::wstring AnalysisRowText(size_t index) const;
    void SaveSnapshots(const std::wstring& directory);
    bool CapturePng(const std::wstring& path);
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM word, LPARAM parameter);
    static LRESULT CALLBACK PageProc(HWND window, UINT message, WPARAM word, LPARAM parameter);
    LRESULT Message(HWND window, UINT message, WPARAM word, LPARAM parameter, int page);
};
} // namespace pc
