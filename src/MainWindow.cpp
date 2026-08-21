#include "pch.h"
#include "MainWindow.h"

#include "Branding.h"
#include "Config.h"
#include "DiscordPresence.h"
#include "LightingBackend.h"
#include "Log.h"
#include "TrayApp.h"

#include <cmath>
#include <cwchar>
#include <cwctype>
#include <lmcons.h>

// Port of MainForm.cs: the settings/status window in raw Win32 + GDI+.
//
// Structure
//   main window ─┬─ header band (painted)      84 px, navy
//                ├─ tab strip   (painted)      4 x 96x30, under the header
//                ├─ page windows (4 children)  one per tab, shown/hidden
//                │    ├─ Status   : labels + "Preview guest mode (10 s)"
//                │    ├─ Lighting : scrollable stack of six brand cards
//                │    ├─ Discord  : preview panel + three edits + toggles
//                │    └─ Advanced : account edit, toggles, buttons, version
//                └─ bottom bar  (painted)      52 px, feedback + Save
//
// Every metric below is the 96-DPI value from MainForm.cs; S() scales it to
// the window's DPI (Per-Monitor V2, re-laid out on WM_DPICHANGED).

#ifndef PC_VERSION_STRING
#define PC_VERSION_STRING "3.0.0"
#endif
#define PC_WIDEN2(x) L##x
#define PC_WIDEN(x) PC_WIDEN2(x)

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace pc {
namespace {

// ---------------------------------------------------------------------------
// RAII helpers
// ---------------------------------------------------------------------------

template <typename T>
class GdiObject {
public:
    GdiObject() = default;
    explicit GdiObject(T handle) : handle_(handle) {}
    GdiObject(const GdiObject&) = delete;
    GdiObject& operator=(const GdiObject&) = delete;
    GdiObject(GdiObject&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    GdiObject& operator=(GdiObject&& other) noexcept {
        if (this != &other) {
            reset(other.handle_);
            other.handle_ = nullptr;
        }
        return *this;
    }
    ~GdiObject() { reset(); }
    void reset(T handle = nullptr) {
        if (handle_) DeleteObject(handle_);
        handle_ = handle;
    }
    T get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    T handle_ = nullptr;
};

/// Selects a GDI object into a DC for the scope.
class ScopedSelect {
public:
    ScopedSelect(HDC dc, HGDIOBJ obj) : dc_(dc), old_(SelectObject(dc, obj)) {}
    ~ScopedSelect() { SelectObject(dc_, old_); }
    ScopedSelect(const ScopedSelect&) = delete;
    ScopedSelect& operator=(const ScopedSelect&) = delete;

private:
    HDC dc_;
    HGDIOBJ old_;
};

/// GetDC/ReleaseDC pair (nullptr window = screen DC).
class WindowDc {
public:
    explicit WindowDc(HWND hwnd) : hwnd_(hwnd), dc_(GetDC(hwnd)) {}
    ~WindowDc() {
        if (dc_) ReleaseDC(hwnd_, dc_);
    }
    WindowDc(const WindowDc&) = delete;
    WindowDc& operator=(const WindowDc&) = delete;
    HDC get() const { return dc_; }

private:
    HWND hwnd_;
    HDC dc_;
};

// ---------------------------------------------------------------------------
// Constants (design metrics are 96-DPI pixels taken from MainForm.cs)
// ---------------------------------------------------------------------------

constexpr wchar_t kMainClass[] = L"PlaycastCompanion.MainWindow";
constexpr wchar_t kPageClass[] = L"PlaycastCompanion.Page";
constexpr wchar_t kWindowTitle[] = L"Playcast Companion";

enum Tab : int { kTabStatus = 0, kTabLighting, kTabDiscord, kTabAdvanced, kTabCount };
constexpr const wchar_t* kTabNames[kTabCount] = {L"Status", L"Lighting", L"Discord", L"Advanced"};
constexpr const wchar_t* kTabFileNames[kTabCount] = {L"status", L"lighting", L"discord", L"advanced"};

constexpr int kClientW = 584;       // Form.ClientSize
constexpr int kClientH = 568;
constexpr int kHeaderH = 84;        // header panel (Dock = Top)
constexpr int kBottomH = 52;        // bottom bar (Dock = Bottom)
constexpr int kTabHostPadL = 12;    // tabs host Padding(12, 10, 12, 4)
constexpr int kTabHostPadT = 10;
constexpr int kTabHostPadR = 12;
constexpr int kTabHostPadB = 4;
constexpr int kTabW = 96;           // DarkTabControl.ItemSize
constexpr int kTabH = 30;
constexpr int kTabUnderline = 2;    // gold accent under the active tab
constexpr int kPagePadX = 14;       // MakePage Padding(14, 12, 14, 12)
constexpr int kPagePadY = 12;

// header
constexpr int kHeaderMarkX = 20, kHeaderMarkY = 14, kHeaderMarkSize = 56;
constexpr int kHeaderTitleX = 88, kHeaderTitleY = 16;
constexpr int kHeaderSubtitleX = 90, kHeaderSubtitleY = 48;
constexpr COLORREF kHeaderSubtitleColor = RGB(196, 208, 222);

// bottom bar
constexpr int kFeedbackX = 16, kFeedbackY = 16;
constexpr int kSaveW = 124, kSaveH = 30, kSaveRight = 138, kSaveY = 10;
constexpr COLORREF kSaveTextColor = RGB(26, 26, 26);

// generic control metrics
constexpr int kEditH = 25;          // WinForms TextBox (FixedSingle) height at 9.75pt
constexpr int kCheckBoxSize = 14;   // drawn check box
constexpr int kCheckGap = 6;        // box -> label
constexpr int kCheckPadH = 4;       // extra height around the label
constexpr int kButtonH = 30;        // flat auto-size buttons
constexpr int kButtonPadW = 28;     // Padding(8, 3, 8, 3) + flat borders
constexpr COLORREF kButtonHover = RGB(45, 45, 45);
constexpr COLORREF kWhite = RGB(255, 255, 255);

// lighting cards: FlowLayoutPanel padding (6,6,0,6) + card Margin(4,4,4,8)
constexpr int kCardX = 10;
constexpr int kCardY0 = 10;
constexpr int kCardW = 506, kCardH = 130;
constexpr int kCardGap = 12;        // margin bottom 8 + next margin top 4
constexpr int kCardsTail = 14;      // last margin bottom 8 + page padding 6
constexpr int kLogoX = 16, kLogoY = 26, kLogoSize = 60;
constexpr int kWordX = 92, kWordY = 14;
constexpr int kLogiGX = 184;
constexpr int kTagX = 93, kTagY = 44, kTagMaxW = 400;
constexpr int kCardDotX = 92, kCardDotY = 68;
constexpr int kCardStatusX = 110, kCardStatusY = 69;
constexpr int kCardCheckX = 92, kCardCheckY = 92;
constexpr int kRazerCaptionX = 316, kRazerCaptionY = 94;
constexpr int kRazerSpinX = 438, kRazerSpinY = 90, kRazerSpinW = 52;
constexpr int kOpenRgbCaptionX = 300, kOpenRgbCaptionY = 94;
constexpr int kOpenRgbEditX = 392, kOpenRgbEditY = 90, kOpenRgbEditW = 110;
constexpr int kScrollLine = 20;
constexpr COLORREF kNumericBack = RGB(32, 34, 37);
constexpr COLORREF kOpenRgbWordColor = RGB(224, 224, 224);
constexpr COLORREF kDynamicText = RGB(70, 70, 70);
constexpr COLORREF kFixedSingleBorder = RGB(100, 100, 100);  // SystemColors.WindowFrame

// discord preview panel
constexpr int kPreviewW = 380, kPreviewH = 80;
constexpr COLORREF kPreviewBack = RGB(43, 45, 49);
constexpr int kPreviewMarkX = 14, kPreviewMarkY = 16, kPreviewMarkSize = 48;
constexpr int kPreviewTextX = 74, kPreviewPlayingY = 14, kPreviewLineY = 36, kPreviewElapsedY = 56;
constexpr int kPreviewLineW = 292, kPreviewLineH = 18;
constexpr COLORREF kPreviewLineColor = RGB(219, 222, 225);
constexpr COLORREF kPreviewElapsedColor = RGB(148, 155, 164);
constexpr int kDiscordEditW = 320;
constexpr int kUserEditW = 200;

enum ControlId : int {
    kIdPageBase = 100,
    kIdPreview = 201,
    kIdSave,
    kIdOpenLog,
    kIdOpenFolder,
    kIdChkRazer = 301,
    kIdChkSteel,
    kIdChkLogi,
    kIdChkCorsair,
    kIdChkOpenRgb,
    kIdChkDynamic,
    kIdChkDiscord,
    kIdChkPassthrough,
    kIdChkDisconnected,
    kIdChkRegistry,
    kIdEditTick = 401,
    kIdUpDownTick,
    kIdEditProfile,
    kIdEditLine1,
    kIdEditLine2,
    kIdEditTemplate,
    kIdEditUser,
    kIdLabelBase = 1000,
};

constexpr UINT_PTR kTimerRefresh = 1;
constexpr UINT_PTR kTimerPreview = 2;
constexpr UINT kRefreshIntervalMs = 1000;
constexpr UINT kPreviewMs = 10000;

enum class FontId : size_t {
    Base,         // Segoe UI 9.75 (form font)
    Semibold,     // Segoe UI Semibold 9.75 (status values)
    Summary,      // Segoe UI Semibold 11.5
    Title,        // Segoe UI Semibold 15
    Subtitle,     // Segoe UI 9.25
    Small,        // Segoe UI 8.5 (card captions, "12:34 elapsed")
    Tagline,      // Roboto 8.5 when installed, else Segoe UI 8.5
    CardStatus,   // Segoe UI Semibold 9
    RazerWord,    // Segoe UI Semibold 14
    SteelWord,    // Segoe UI Black 12.5
    LogiWord,     // Segoe UI 14.5
    LogiG,        // Segoe UI 14.5 bold
    CorsairWord,  // Bahnschrift 14 bold when installed, else Segoe UI Semibold 14
    OpenRgbWord,  // Consolas 14 bold
    DynamicWord,  // Segoe UI Semibold 13
    Playing,      // Segoe UI Semibold 9.5
    PrevLine,     // Segoe UI 9
    Count
};

// ---------------------------------------------------------------------------
// Small free helpers
// ---------------------------------------------------------------------------

int CALLBACK FontEnumCallback(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lParam) {
    *reinterpret_cast<bool*>(lParam) = true;
    return 0;
}

bool FontFaceInstalled(const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face);
    bool found = false;
    WindowDc dc(nullptr);
    if (dc.get()) EnumFontFamiliesExW(dc.get(), &lf, FontEnumCallback, reinterpret_cast<LPARAM>(&found), 0);
    return found;
}

HFONT CreateUiFont(const wchar_t* face, float pointSize, int weight, int dpi) {
    LOGFONTW lf{};
    lf.lfHeight = -static_cast<LONG>(std::lround(pointSize * static_cast<float>(dpi) / 72.0f));
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcscpy_s(lf.lfFaceName, face);
    return CreateFontIndirectW(&lf);
}

std::wstring GetText(HWND hwnd) {
    if (!hwnd) return {};
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) return {};
    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
    const int got = GetWindowTextW(hwnd, buffer.data(), length + 1);
    return std::wstring(buffer.data(), static_cast<size_t>(got > 0 ? got : 0));
}

std::wstring Trim(std::wstring_view s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && std::iswspace(static_cast<wint_t>(s[begin]))) ++begin;
    while (end > begin && std::iswspace(static_cast<wint_t>(s[end - 1]))) --end;
    return std::wstring(s.substr(begin, end - begin));
}

bool ContainsNoCase(std::wstring_view hay, std::wstring_view needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::towlower(static_cast<wint_t>(hay[i + j])) != std::towlower(static_cast<wint_t>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

bool EqualsNoCase(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

/// MainForm.IsWarnStatus
bool IsWarnStatus(std::wstring_view s) {
    return ContainsNoCase(s, L"retry") || ContainsNoCase(s, L"Unreachable") || ContainsNoCase(s, L"not found") ||
           ContainsNoCase(s, L"error");
}

std::wstring CurrentUserName() {
    wchar_t buffer[UNLEN + 1] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (GetUserNameW(buffer, &size) && size > 0) return std::wstring(buffer);
    return {};
}

std::wstring ExeDirectory() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) return {};
        if (n < buffer.size() - 1) break;
        if (buffer.size() > 65536) return {};
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer.data()).parent_path().wstring();
}

/// MainForm.ReadAutostartState: HKLM Run value => every account; HKCU => this
/// account; neither => off; access problems => unknown.
std::pair<COLORREF, std::wstring> ReadAutostartState() {
    constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr const wchar_t* kValue = L"PlaycastCompanion";

    wchar_t buffer[1024] = {};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    const LSTATUS machine = RegGetValueW(HKEY_LOCAL_MACHINE, kRunKey, kValue,
                                         RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, buffer, &size);
    if (machine == ERROR_SUCCESS) {
        buffer[std::size(buffer) - 1] = L'\0';
        if (buffer[0] != L'\0') return {brand::Green, L"On for every account"};
    } else if (machine == ERROR_MORE_DATA) {
        return {brand::Green, L"On for every account"};  // a (long) non-empty string
    } else if (machine != ERROR_FILE_NOT_FOUND && machine != ERROR_UNSUPPORTED_TYPE) {
        return {brand::Slate, L"Unknown"};
    }

    const LSTATUS user = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValue, RRF_RT_ANY, nullptr, nullptr, nullptr);
    if (user == ERROR_SUCCESS || user == ERROR_MORE_DATA) return {brand::Green, L"On for this account only"};
    if (user == ERROR_FILE_NOT_FOUND) return {brand::Amber, L"Off — run the installer to turn it on"};
    return {brand::Slate, L"Unknown"};
}

bool GetEncoderClsid(const wchar_t* mimeType, CLSID* out) {
    UINT count = 0;
    UINT bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0) return false;
    std::vector<BYTE> storage(bytes);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < count; ++i) {
        if (codecs[i].MimeType && wcscmp(codecs[i].MimeType, mimeType) == 0) {
            *out = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

void PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            return;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct MainWindow::Impl {
    explicit Impl(TrayApp& application) : app(application) {}
    ~Impl() {
        if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // --- per-control theming records -----------------------------------
    struct LabelInfo {
        FontId font = FontId::Base;
        COLORREF fore = brand::UiText;
        COLORREF back = brand::UiBack;
        std::wstring text;
        int maxWidth = 0;        // design px; > 0 = word-wrap width
        bool autoSize = true;
        SIZE fixedSize{};        // when !autoSize (design px)
    };
    struct EditInfo {
        COLORREF fore = brand::UiText;
        COLORREF back = brand::UiSurface;
    };
    struct CheckInfo {
        bool checked = false;
        COLORREF fore = brand::UiText;
        COLORREF back = brand::UiBack;
    };
    struct ButtonInfo {
        COLORREF back = brand::UiSurface;
        COLORREF fore = brand::UiText;
        COLORREF border = brand::UiBorder;
        bool gold = false;
        bool hot = false;
    };
    struct Card {
        ILightingBackend* backend = nullptr;
        COLORREF back = brand::UiBack;
        bool border = false;
        void (*painter)(Gdiplus::Graphics&, const Gdiplus::Rect&) = nullptr;
        HWND word = nullptr;     // wordmark
        HWND word2 = nullptr;    // Logitech "G"
        HWND tag = nullptr;      // tagline
        HWND dot = nullptr;      // "●"
        HWND status = nullptr;   // "Off" / "Ready" / StatusText
        HWND check = nullptr;    // "Include in guest blackout"
        HWND caption = nullptr;  // Razer / OpenRGB option caption
    };

    // --- state --------------------------------------------------------
    TrayApp& app;
    HINSTANCE hinst = nullptr;
    HWND hwnd = nullptr;
    std::array<HWND, kTabCount> pages{};
    int activeTab = kTabStatus;
    int dpi = 96;
    int nextLabelId = kIdLabelBase;
    bool previewing = false;
    int lightingScroll = 0;      // current vertical scroll offset (px)
    int lightingContentH = 0;    // full stack height (px)
    int wheelAccum = 0;
    RECT previewPanel{};         // Discord preview panel, page client coords

    std::unique_ptr<Gdiplus::Bitmap> mark;  // generated app mark (256 px)
    std::array<GdiObject<HFONT>, static_cast<size_t>(FontId::Count)> fonts;
    std::unordered_map<COLORREF, GdiObject<HBRUSH>> brushes;
    std::unordered_map<HWND, LabelInfo> labels;
    std::unordered_map<HWND, EditInfo> edits;
    std::unordered_map<HWND, CheckInfo> checks;
    std::unordered_map<HWND, ButtonInfo> buttons;

    // Status tab
    HWND lblSummary = nullptr;
    std::array<HWND, 4> statusDots{};
    std::array<HWND, 4> statusCaptions{};
    std::array<HWND, 4> statusValues{};
    HWND btnPreview = nullptr;

    // Lighting tab
    std::array<Card, 6> cards{};
    HWND editTick = nullptr;
    HWND updownTick = nullptr;
    HWND editOpenRgbProfile = nullptr;

    // Discord tab
    HWND chkDiscord = nullptr;
    HWND lblPlaying = nullptr;
    HWND lblPrevLine1 = nullptr;
    HWND lblElapsed = nullptr;
    std::array<HWND, 3> discordCaptions{};
    HWND editLine1 = nullptr;
    HWND editLine2 = nullptr;
    HWND editHostTemplate = nullptr;
    HWND chkPassthrough = nullptr;
    HWND lblAppIdState = nullptr;

    // Advanced tab
    HWND lblUserCaption = nullptr;
    HWND editUser = nullptr;
    HWND lblUserBlurb = nullptr;
    HWND chkDisconnected = nullptr;
    HWND chkRegistry = nullptr;
    HWND btnOpenLog = nullptr;
    HWND btnOpenFolder = nullptr;
    HWND lblVersion = nullptr;

    // bottom bar
    HWND lblFeedback = nullptr;
    HWND btnSave = nullptr;

    // --- scaling --------------------------------------------------------
    int S(int v) const { return MulDiv(v, dpi, 96); }

    // --- lifecycle --------------------------------------------------------
    void Create();
    void RegisterClasses();
    void BuildFonts();
    void ApplyFonts();
    HFONT Font(FontId id) const { return fonts[static_cast<size_t>(id)].get(); }
    HBRUSH Brush(COLORREF color);
    SIZE MeasureText(FontId font, std::wstring_view text, int maxWidthPx = 0) const;
    int LineHeight() const { return MeasureText(FontId::Base, L"Ag").cy; }

    // --- control factories ---------------------------------------------
    HWND MakeLabel(HWND parent, std::wstring_view text, FontId font, COLORREF fore, COLORREF back, int maxWidth = 0,
                   DWORD extraStyle = 0);
    HWND MakeFixedLabel(HWND parent, std::wstring_view text, FontId font, COLORREF fore, COLORREF back, SIZE size,
                        DWORD extraStyle);
    HWND MakeEdit(HWND parent, int id, COLORREF fore, COLORREF back, bool numeric);
    HWND MakeCheck(HWND parent, int id, std::wstring_view text, COLORREF fore, COLORREF back);
    HWND MakeButton(HWND parent, int id, std::wstring_view text, bool gold);
    void Subclass(HWND control);

    // --- label/control state -----------------------------------------------
    void PlaceLabel(HWND label, int x, int y);
    void Place(HWND control, int x, int y, int w, int h);
    void PlaceEdit(HWND edit, int x, int y, int w);
    void PlaceCheck(HWND check, int x, int y);
    int PlaceButton(HWND button, int x, int y);  // returns width
    SIZE CheckSize(HWND check) const;
    void SetLabelText(HWND label, std::wstring_view text);
    void SetLabelColor(HWND label, COLORREF fore);
    void SetRow(size_t row, COLORREF color, std::wstring_view text);
    bool IsChecked(HWND check) const;
    void SetChecked(HWND check, bool value);

    // --- pages ---------------------------------------------------------
    void CreatePages();
    RECT TabRect(int index) const;
    RECT PageRect() const;
    void BuildStatusPage();
    void BuildLightingPage();
    void AddCardBody(Card& card, HWND page, int checkId, std::wstring_view tagline, COLORREF fore);
    void BuildDiscordPage();
    void BuildAdvancedPage();
    void BuildBottomBar();
    void LayoutAll();
    void LayoutStatusPage();
    void LayoutLightingPage();
    void LayoutDiscordPage();
    void LayoutAdvancedPage();
    void LayoutBottomBar();
    void SelectTab(int index);

    // --- painting ------------------------------------------------------
    void PaintMain(HDC hdc, const RECT& client);
    void PaintPage(int index, HDC hdc, const RECT& client);
    void PaintEditFrames(HWND page, HDC hdc);
    void DrawMark(HDC hdc, int x, int y, int size);
    void PaintText(HDC hdc, std::wstring_view text, FontId font, COLORREF color, RECT rc, UINT format);
    void DrawCheckItem(const DRAWITEMSTRUCT& dis, const CheckInfo& info);
    void DrawButtonItem(const DRAWITEMSTRUCT& dis, const ButtonInfo& info);

    // --- scrolling (Lighting) ------------------------------------------
    void UpdateLightingScrollInfo();
    void ScrollLightingTo(int position);
    void OnLightingVScroll(int code);
    void OnLightingWheel(int delta);

    // --- behaviour -----------------------------------------------------
    void LoadFromConfig();
    void RefreshStatus();
    void UpdatePreviewLine();
    void StartPreview();
    void EndPreview();
    void SaveSettings();
    void OpenInstallFolder();
    void OnDpiChanged(int newDpi, const RECT* suggested);
    bool CaptureWindowPng(const std::wstring& file);
    void SaveSnapshots(const std::wstring& dir);

    // --- message handling ----------------------------------------------
    LRESULT OnCommand(WPARAM wParam, LPARAM lParam, bool& handled);
    LRESULT OnCtlColor(UINT msg, HDC hdc, HWND control, bool& handled);
    bool OnDrawItem(const DRAWITEMSTRUCT& dis);
    void OnButtonHover(HWND button, bool hot);
    LRESULT MainProc(UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT PageProc(HWND page, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK PageWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ControlSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                UINT_PTR subclassId, DWORD_PTR refData);
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void MainWindow::Impl::RegisterClasses() {
    hinst = app.Instance() ? app.Instance() : GetModuleHandleW(nullptr);
    WNDCLASSEXW probe{};
    probe.cbSize = sizeof(probe);
    if (!GetClassInfoExW(hinst, kMainClass, &probe)) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = MainWndProc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;  // everything is painted in WM_PAINT
        wc.lpszClassName = kMainClass;
        RegisterClassExW(&wc);
    }
    if (!GetClassInfoExW(hinst, kPageClass, &probe)) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = 0;
        wc.lpfnWndProc = PageWndProc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kPageClass;
        RegisterClassExW(&wc);
    }
}

void MainWindow::Impl::Create() {
    RegisterClasses();
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS};
    InitCommonControlsEx(&icc);
    if (!mark) mark.reset(brand::MakeAppMark(256));

    constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    constexpr DWORD kExStyle = 0;

    // Create on the monitor under the cursor (Form.StartPosition.CenterScreen),
    // then size/centre with that monitor's DPI.
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT work{0, 0, 1280, 720};
    if (GetMonitorInfoW(monitor, &mi)) work = mi.rcWork;

    HWND created = CreateWindowExW(kExStyle, kMainClass, kWindowTitle, kStyle, work.left, work.top, kClientW,
                                   kClientH, nullptr, nullptr, hinst, this);
    if (!created) throw std::runtime_error("CreateWindowExW(main) failed");
    hwnd = created;

    dpi = static_cast<int>(GetDpiForWindow(hwnd));
    if (dpi <= 0) dpi = 96;
    RECT frame{0, 0, S(kClientW), S(kClientH)};
    AdjustWindowRectExForDpi(&frame, kStyle, FALSE, kExStyle, static_cast<UINT>(dpi));
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    const int x = work.left + ((work.right - work.left) - width) / 2;
    const int y = work.top + ((work.bottom - work.top) - height) / 2;
    SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);

    // Dark title bar (DWMWA_USE_IMMERSIVE_DARK_MODE = 20; 19 on pre-20H1 builds).
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));

    if (HICON icon = app.AppIcon()) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }

    BuildFonts();
    CreatePages();
    BuildStatusPage();
    BuildLightingPage();
    BuildDiscordPage();
    BuildAdvancedPage();
    BuildBottomBar();
    LayoutAll();
    LoadFromConfig();
    SetTimer(hwnd, kTimerRefresh, kRefreshIntervalMs, nullptr);
}

void MainWindow::Impl::BuildFonts() {
    auto set = [this](FontId id, const wchar_t* face, float pt, int weight) {
        fonts[static_cast<size_t>(id)].reset(CreateUiFont(face, pt, weight, dpi));
    };
    set(FontId::Base, L"Segoe UI", 9.75f, FW_NORMAL);
    set(FontId::Semibold, L"Segoe UI Semibold", 9.75f, FW_SEMIBOLD);
    set(FontId::Summary, L"Segoe UI Semibold", 11.5f, FW_SEMIBOLD);
    set(FontId::Title, L"Segoe UI Semibold", 15.0f, FW_SEMIBOLD);
    set(FontId::Subtitle, L"Segoe UI", 9.25f, FW_NORMAL);
    set(FontId::Small, L"Segoe UI", 8.5f, FW_NORMAL);
    // Roboto is Razer's own body font (installed system-wide by Synapse); the
    // C# form asked for it and fell back when absent.
    set(FontId::Tagline, FontFaceInstalled(L"Roboto") ? L"Roboto" : L"Segoe UI", 8.5f, FW_NORMAL);
    set(FontId::CardStatus, L"Segoe UI Semibold", 9.0f, FW_SEMIBOLD);
    set(FontId::RazerWord, L"Segoe UI Semibold", 14.0f, FW_SEMIBOLD);
    set(FontId::SteelWord, L"Segoe UI Black", 12.5f, FW_BLACK);
    set(FontId::LogiWord, L"Segoe UI", 14.5f, FW_NORMAL);
    set(FontId::LogiG, L"Segoe UI", 14.5f, FW_BOLD);
    if (FontFaceInstalled(L"Bahnschrift"))
        set(FontId::CorsairWord, L"Bahnschrift", 14.0f, FW_BOLD);
    else
        set(FontId::CorsairWord, L"Segoe UI Semibold", 14.0f, FW_SEMIBOLD);
    set(FontId::OpenRgbWord, L"Consolas", 14.0f, FW_BOLD);
    set(FontId::DynamicWord, L"Segoe UI Semibold", 13.0f, FW_SEMIBOLD);
    set(FontId::Playing, L"Segoe UI Semibold", 9.5f, FW_SEMIBOLD);
    set(FontId::PrevLine, L"Segoe UI", 9.0f, FW_NORMAL);
}

void MainWindow::Impl::ApplyFonts() {
    for (auto& [label, info] : labels)
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(Font(info.font)), TRUE);
    for (auto& [edit, info] : edits)
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(Font(FontId::Base)), TRUE);
    for (auto& [check, info] : checks)
        SendMessageW(check, WM_SETFONT, reinterpret_cast<WPARAM>(Font(FontId::Base)), TRUE);
    for (auto& [button, info] : buttons)
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(Font(FontId::Base)), TRUE);
}

HBRUSH MainWindow::Impl::Brush(COLORREF color) {
    auto it = brushes.find(color);
    if (it == brushes.end()) it = brushes.emplace(color, GdiObject<HBRUSH>(CreateSolidBrush(color))).first;
    return it->second.get();
}

SIZE MainWindow::Impl::MeasureText(FontId font, std::wstring_view text, int maxWidthPx) const {
    WindowDc dc(nullptr);
    if (!dc.get()) return SIZE{0, 0};
    ScopedSelect select(dc.get(), Font(font));
    RECT rc{0, 0, maxWidthPx > 0 ? maxWidthPx : 0, 0};
    UINT format = DT_CALCRECT | DT_NOPREFIX;
    format |= maxWidthPx > 0 ? DT_WORDBREAK : DT_SINGLELINE;
    const std::wstring copy(text.empty() ? L"Ag" : text);
    DrawTextW(dc.get(), copy.c_str(), static_cast<int>(copy.size()), &rc, format);
    SIZE size{rc.right - rc.left, rc.bottom - rc.top};
    if (text.empty()) size.cx = 0;
    return size;
}

// ---------------------------------------------------------------------------
// Control factories
// ---------------------------------------------------------------------------

HWND MainWindow::Impl::MakeLabel(HWND parent, std::wstring_view text, FontId font, COLORREF fore, COLORREF back,
                                 int maxWidth, DWORD extraStyle) {
    const std::wstring copy(text);
    HWND label = CreateWindowExW(0, L"STATIC", copy.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | extraStyle,
                                 0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(nextLabelId++)),
                                 hinst, nullptr);
    if (!label) throw std::runtime_error("CreateWindowExW(STATIC) failed");
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(Font(font)), TRUE);
    LabelInfo info;
    info.font = font;
    info.fore = fore;
    info.back = back;
    info.text = copy;
    info.maxWidth = maxWidth;
    info.autoSize = true;
    labels[label] = std::move(info);
    return label;
}

HWND MainWindow::Impl::MakeFixedLabel(HWND parent, std::wstring_view text, FontId font, COLORREF fore, COLORREF back,
                                      SIZE size, DWORD extraStyle) {
    HWND label = MakeLabel(parent, text, font, fore, back, 0, extraStyle);
    LabelInfo& info = labels[label];
    info.autoSize = false;
    info.fixedSize = size;
    return label;
}

HWND MainWindow::Impl::MakeEdit(HWND parent, int id, COLORREF fore, COLORREF back, bool numeric) {
    const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL | (numeric ? ES_NUMBER : 0);
    HWND edit = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hinst, nullptr);
    if (!edit) throw std::runtime_error("CreateWindowExW(EDIT) failed");
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(Font(FontId::Base)), TRUE);
    SendMessageW(edit, EM_SETLIMITTEXT, 512, 0);
    EditInfo info;
    info.fore = fore;
    info.back = back;
    edits[edit] = info;
    Subclass(edit);
    return edit;
}

HWND MainWindow::Impl::MakeCheck(HWND parent, int id, std::wstring_view text, COLORREF fore, COLORREF back) {
    const std::wstring copy(text);
    HWND check = CreateWindowExW(0, L"BUTTON", copy.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0,
                                 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hinst, nullptr);
    if (!check) throw std::runtime_error("CreateWindowExW(BUTTON check) failed");
    SendMessageW(check, WM_SETFONT, reinterpret_cast<WPARAM>(Font(FontId::Base)), TRUE);
    CheckInfo info;
    info.fore = fore;
    info.back = back;
    checks[check] = info;
    Subclass(check);
    return check;
}

HWND MainWindow::Impl::MakeButton(HWND parent, int id, std::wstring_view text, bool gold) {
    const std::wstring copy(text);
    HWND button = CreateWindowExW(0, L"BUTTON", copy.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                                  0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hinst,
                                  nullptr);
    if (!button) throw std::runtime_error("CreateWindowExW(BUTTON) failed");
    SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(Font(FontId::Base)), TRUE);
    ButtonInfo info;
    info.gold = gold;
    if (gold) {
        info.back = brand::Gold;
        info.fore = kSaveTextColor;
        info.border = brand::Gold;
    }
    buttons[button] = info;
    Subclass(button);
    return button;
}

void MainWindow::Impl::Subclass(HWND control) {
    SetWindowSubclass(control, ControlSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
}

// ---------------------------------------------------------------------------
// Placement / state helpers
// ---------------------------------------------------------------------------

void MainWindow::Impl::Place(HWND control, int x, int y, int w, int h) {
    SetWindowPos(control, nullptr, x, y, std::max(w, 1), std::max(h, 1), SWP_NOZORDER | SWP_NOACTIVATE);
}

void MainWindow::Impl::PlaceLabel(HWND label, int x, int y) {
    auto it = labels.find(label);
    if (it == labels.end()) return;
    const LabelInfo& info = it->second;
    SIZE size;
    if (info.autoSize) {
        size = MeasureText(info.font, info.text, info.maxWidth > 0 ? S(info.maxWidth) : 0);
        size.cx += 2;  // Label.AutoSize keeps a hair of slack so glyphs never clip
    } else {
        size = SIZE{S(info.fixedSize.cx), S(info.fixedSize.cy)};
    }
    Place(label, x, y, size.cx, size.cy);
}

void MainWindow::Impl::PlaceEdit(HWND edit, int x, int y, int w) {
    // The 1 px UiBorder frame is painted by the parent around (x, y, w, kEditH);
    // the EDIT itself sits one pixel inside.
    Place(edit, x + 1, y + 1, w - 2, S(kEditH) - 2);
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(3), S(3)));
}

SIZE MainWindow::Impl::CheckSize(HWND check) const {
    const SIZE text = MeasureText(FontId::Base, GetText(check));
    return SIZE{S(kCheckBoxSize) + S(kCheckGap) + text.cx + S(kCheckPadH),
                std::max(static_cast<int>(text.cy), S(kCheckBoxSize)) + S(kCheckPadH)};
}

void MainWindow::Impl::PlaceCheck(HWND check, int x, int y) {
    const SIZE size = CheckSize(check);
    Place(check, x, y, size.cx, size.cy);
}

int MainWindow::Impl::PlaceButton(HWND button, int x, int y) {
    const SIZE text = MeasureText(FontId::Base, GetText(button));
    const int w = text.cx + S(kButtonPadW);
    Place(button, x, y, w, S(kButtonH));
    return w;
}

void MainWindow::Impl::SetLabelText(HWND label, std::wstring_view text) {
    auto it = labels.find(label);
    if (it == labels.end()) return;
    LabelInfo& info = it->second;
    if (info.text == text) return;
    info.text.assign(text);
    SetWindowTextW(label, info.text.c_str());
    if (info.autoSize) {
        SIZE size = MeasureText(info.font, info.text, info.maxWidth > 0 ? S(info.maxWidth) : 0);
        size.cx += 2;
        SetWindowPos(label, nullptr, 0, 0, std::max<LONG>(size.cx, 1), std::max<LONG>(size.cy, 1),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    InvalidateRect(label, nullptr, TRUE);
}

void MainWindow::Impl::SetLabelColor(HWND label, COLORREF fore) {
    auto it = labels.find(label);
    if (it == labels.end() || it->second.fore == fore) return;
    it->second.fore = fore;
    InvalidateRect(label, nullptr, TRUE);
}

/// MainForm.SetRow: recolour the dot and set the value text of a status row.
void MainWindow::Impl::SetRow(size_t row, COLORREF color, std::wstring_view text) {
    if (row >= statusDots.size()) return;
    SetLabelColor(statusDots[row], color);
    SetLabelText(statusValues[row], text);
}

bool MainWindow::Impl::IsChecked(HWND check) const {
    auto it = checks.find(check);
    return it != checks.end() && it->second.checked;
}

void MainWindow::Impl::SetChecked(HWND check, bool value) {
    auto it = checks.find(check);
    if (it == checks.end() || it->second.checked == value) return;
    it->second.checked = value;
    InvalidateRect(check, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

RECT MainWindow::Impl::TabRect(int index) const {
    const int x = S(kTabHostPadL) + index * S(kTabW);
    const int y = S(kHeaderH) + S(kTabHostPadT);
    return RECT{x, y, x + S(kTabW), y + S(kTabH)};
}

RECT MainWindow::Impl::PageRect() const {
    const int left = S(kTabHostPadL);
    const int top = S(kHeaderH) + S(kTabHostPadT) + S(kTabH);
    const int right = S(kClientW) - S(kTabHostPadR);
    const int bottom = S(kClientH) - S(kBottomH) - S(kTabHostPadB);
    return RECT{left, top, right, bottom};
}

void MainWindow::Impl::CreatePages() {
    const RECT page = PageRect();
    for (int i = 0; i < kTabCount; ++i) {
        DWORD style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        if (i == kTabLighting) style |= WS_VSCROLL;
        if (i == activeTab) style |= WS_VISIBLE;
        HWND created = CreateWindowExW(WS_EX_CONTROLPARENT, kPageClass, kTabNames[i], style, page.left, page.top,
                                       page.right - page.left, page.bottom - page.top, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPageBase + i)), hinst, this);
        if (!created) throw std::runtime_error("CreateWindowExW(page) failed");
        pages[static_cast<size_t>(i)] = created;
    }
    // Dark scrollbar for the card stack (no-op on builds without the theme).
    SetWindowTheme(pages[kTabLighting], L"DarkMode_Explorer", nullptr);
}

void MainWindow::Impl::SelectTab(int index) {
    if (index < 0 || index >= kTabCount || index == activeTab) return;
    ShowWindow(pages[static_cast<size_t>(activeTab)], SW_HIDE);
    activeTab = index;
    ShowWindow(pages[static_cast<size_t>(activeTab)], SW_SHOW);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Status tab
// ---------------------------------------------------------------------------

void MainWindow::Impl::BuildStatusPage() {
    HWND page = pages[kTabStatus];
    lblSummary = MakeLabel(page, L"", FontId::Summary, brand::Green, brand::UiBack);
    constexpr const wchar_t* kCaptions[4] = {L"Guest mode", L"Lighting", L"Discord", L"Starts with Windows"};
    for (size_t i = 0; i < 4; ++i) {
        statusDots[i] = MakeLabel(page, L"●", FontId::Base, brand::Slate, brand::UiBack);
        statusCaptions[i] = MakeLabel(page, kCaptions[i], FontId::Base, brand::UiTextDim, brand::UiBack);
        statusValues[i] = MakeLabel(page, L"—", FontId::Semibold, brand::UiText, brand::UiBack);
    }
    btnPreview = MakeButton(page, kIdPreview, L"Preview guest mode (10 s)", false);
}

void MainWindow::Impl::LayoutStatusPage() {
    // TableLayoutPanel: summary (margin 3,4,3,12) / 3-column grid / preview
    // button (margin 3,18,3,3). Grid cells: dot (3,6,0,6), caption (6,6,14,6),
    // value (3,6,3,6).
    const int padX = S(kPagePadX);
    const int padY = S(kPagePadY);
    const int lineH = LineHeight();
    const int summaryH = MeasureText(FontId::Summary, L"Ag").cy;

    PlaceLabel(lblSummary, padX + S(3), padY + S(4));
    const int gridY = padY + S(4) + summaryH + S(12);

    const int dotW = MeasureText(FontId::Base, L"●").cx + 2;
    int captionW = 0;
    for (HWND caption : statusCaptions)
        captionW = std::max(captionW, static_cast<int>(MeasureText(FontId::Base, labels[caption].text).cx) + 2);
    const int col0 = S(3) + dotW;              // dot column incl. margins
    const int col1 = S(6) + captionW + S(14);  // caption column incl. margins
    const int rowH = lineH + S(12);
    for (size_t r = 0; r < 4; ++r) {
        const int y = gridY + static_cast<int>(r) * rowH + S(6);
        PlaceLabel(statusDots[r], padX + S(3), y);
        PlaceLabel(statusCaptions[r], padX + col0 + S(6), y);
        PlaceLabel(statusValues[r], padX + col0 + col1 + S(3), y);
    }
    PlaceButton(btnPreview, padX + S(3), gridY + 4 * rowH + S(18));
}

// ---------------------------------------------------------------------------
// Lighting tab (brand cards)
// ---------------------------------------------------------------------------

void MainWindow::Impl::AddCardBody(Card& card, HWND page, int checkId, std::wstring_view tagline, COLORREF fore) {
    card.tag = MakeLabel(page, tagline, FontId::Tagline, fore, card.back, kTagMaxW);
    card.dot = MakeLabel(page, L"●", FontId::Base, brand::Slate, card.back);
    card.status = MakeLabel(page, L"—", FontId::CardStatus, fore, card.back);
    card.check = MakeCheck(page, checkId, L"Include in guest blackout", fore, card.back);
}

void MainWindow::Impl::BuildLightingPage() {
    HWND page = pages[kTabLighting];
    const auto& backends = app.Lighting();
    auto backendAt = [&backends](size_t i) -> ILightingBackend* {
        return i < backends.size() ? backends[i].get() : nullptr;
    };

    // order matches TrayApp: Chroma, SteelSeries, Logitech, Corsair, OpenRGB, DynamicLighting
    {   // Razer-styled card: black surface, Chroma-green accent.
        Card& c = cards[0];
        c.backend = backendAt(0);
        c.back = brand::RazerBlack;
        c.painter = brand::PaintRazerMark;
        c.word = MakeLabel(page, L"Razer", FontId::RazerWord, brand::RazerGreen, c.back);
        AddCardBody(c, page, kIdChkRazer, L"Chroma SDK — keyboards, mice, headsets, pads & ChromaLink",
                    brand::RazerText);
        c.caption = MakeLabel(page, L"re-assert every (s)", FontId::Small, brand::RazerTextDim, c.back);
        editTick = MakeEdit(page, kIdEditTick, kWhite, brand::RazerSurface, true);
        updownTick = CreateWindowExW(0, UPDOWN_CLASSW, L"",
                                     WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS |
                                         UDS_NOTHOUSANDS,
                                     0, 0, 0, 0, page, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdUpDownTick)),
                                     hinst, nullptr);
        if (!updownTick) throw std::runtime_error("CreateWindowExW(updown) failed");
        SendMessageW(updownTick, UDM_SETRANGE32, 1, 10);
        SendMessageW(updownTick, UDM_SETBUDDY, reinterpret_cast<WPARAM>(editTick), 0);
    }
    {   // SteelSeries
        Card& c = cards[1];
        c.backend = backendAt(1);
        c.back = brand::SteelDark;
        c.painter = brand::PaintSteelSeriesMark;
        c.word = MakeLabel(page, L"SteelSeries", FontId::SteelWord, brand::SteelOrange, c.back);
        AddCardBody(c, page, kIdChkSteel, L"GameSense — needs SteelSeries GG running", brand::CardLightText);
    }
    {   // Logitech G
        Card& c = cards[2];
        c.backend = backendAt(2);
        c.back = brand::LogiDark;
        c.painter = brand::PaintLogitechMark;
        c.word = MakeLabel(page, L"Logitech", FontId::LogiWord, kWhite, c.back);
        c.word2 = MakeLabel(page, L"G", FontId::LogiG, brand::LogiBlue, c.back);
        AddCardBody(c, page, kIdChkLogi, L"LED SDK — via G HUB, or drop the wrapper DLL next to the app",
                    brand::CardLightText);
    }
    {   // Corsair
        Card& c = cards[3];
        c.backend = backendAt(3);
        c.back = brand::CorsairBlack;
        c.painter = brand::PaintCorsairMark;
        c.word = MakeLabel(page, L"Corsair", FontId::CorsairWord, brand::CorsairYellow, c.back);
        AddCardBody(c, page, kIdChkCorsair, L"iCUE — needs CUESDK.x64_2017.dll next to the app (from the CUE SDK)",
                    brand::CardLightText);
    }
    {   // OpenRGB
        Card& c = cards[4];
        c.backend = backendAt(4);
        c.back = brand::OpenRgbDark;
        c.painter = brand::PaintOpenRgbMark;
        c.word = MakeLabel(page, L"OpenRGB", FontId::OpenRgbWord, kOpenRgbWordColor, c.back);
        AddCardBody(c, page, kIdChkOpenRgb, L"One server for motherboards, RAM, GPUs & more", brand::CardLightText);
        c.caption = MakeLabel(page, L"blackout profile", FontId::Small, brand::CardLightText, c.back);
        editOpenRgbProfile = MakeEdit(page, kIdEditProfile, kWhite, kNumericBack, false);
    }
    {   // Windows Dynamic Lighting: the one light card, with a 1 px border.
        Card& c = cards[5];
        c.backend = backendAt(5);
        c.back = kWhite;
        c.border = true;
        c.painter = brand::PaintWindowsMark;
        c.word = MakeLabel(page, L"Dynamic Lighting", FontId::DynamicWord, brand::WindowsBlue, c.back);
        AddCardBody(c, page, kIdChkDynamic, L"Built into Windows 11 — covers devices no other engine owns",
                    kDynamicText);
    }
}

void MainWindow::Impl::LayoutLightingPage() {
    // Children are positioned at scroll offset 0; ScrollLightingTo moves them.
    const int pitch = S(kCardH) + S(kCardGap);
    for (size_t i = 0; i < cards.size(); ++i) {
        const Card& c = cards[i];
        const int cx = S(kCardX);
        const int cy = S(kCardY0) + static_cast<int>(i) * pitch - lightingScroll;
        PlaceLabel(c.word, cx + S(kWordX), cy + S(kWordY));
        if (c.word2) PlaceLabel(c.word2, cx + S(kLogiGX), cy + S(kWordY));
        PlaceLabel(c.tag, cx + S(kTagX), cy + S(kTagY));
        PlaceLabel(c.dot, cx + S(kCardDotX), cy + S(kCardDotY));
        PlaceLabel(c.status, cx + S(kCardStatusX), cy + S(kCardStatusY));
        PlaceCheck(c.check, cx + S(kCardCheckX), cy + S(kCardCheckY));
        if (i == 0) {
            PlaceLabel(c.caption, cx + S(kRazerCaptionX), cy + S(kRazerCaptionY));
            PlaceEdit(editTick, cx + S(kRazerSpinX), cy + S(kRazerSpinY), S(kRazerSpinW));
            // re-attach so UDS_ALIGNRIGHT re-aligns the spinner to the moved buddy
            SendMessageW(updownTick, UDM_SETBUDDY, reinterpret_cast<WPARAM>(editTick), 0);
        } else if (i == 4) {
            PlaceLabel(c.caption, cx + S(kOpenRgbCaptionX), cy + S(kOpenRgbCaptionY));
            PlaceEdit(editOpenRgbProfile, cx + S(kOpenRgbEditX), cy + S(kOpenRgbEditY), S(kOpenRgbEditW));
        }
    }
    lightingContentH = S(kCardY0) + static_cast<int>(cards.size() - 1) * pitch + S(kCardH) + S(kCardsTail);
    UpdateLightingScrollInfo();
}

void MainWindow::Impl::UpdateLightingScrollInfo() {
    HWND page = pages[kTabLighting];
    RECT rc{};
    GetClientRect(page, &rc);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(lightingContentH - 1, 0);
    si.nPage = static_cast<UINT>(std::max<LONG>(rc.bottom - rc.top, 0));
    si.nPos = lightingScroll;
    SetScrollInfo(page, SB_VERT, &si, TRUE);
}

void MainWindow::Impl::ScrollLightingTo(int position) {
    HWND page = pages[kTabLighting];
    RECT rc{};
    GetClientRect(page, &rc);
    const int maxPos = std::max(lightingContentH - static_cast<int>(rc.bottom - rc.top), 0);
    position = std::clamp(position, 0, maxPos);
    if (position == lightingScroll) return;
    const int delta = lightingScroll - position;  // negative = content moves up
    lightingScroll = position;
    ScrollWindowEx(page, 0, delta, nullptr, nullptr, nullptr, nullptr,
                   SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    SetScrollPos(page, SB_VERT, lightingScroll, TRUE);
    UpdateWindow(page);
}

void MainWindow::Impl::OnLightingVScroll(int code) {
    HWND page = pages[kTabLighting];
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    GetScrollInfo(page, SB_VERT, &si);
    int pos = lightingScroll;
    const int pageSize = static_cast<int>(si.nPage);
    switch (code) {
        case SB_LINEUP: pos -= S(kScrollLine); break;
        case SB_LINEDOWN: pos += S(kScrollLine); break;
        case SB_PAGEUP: pos -= pageSize; break;
        case SB_PAGEDOWN: pos += pageSize; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = lightingContentH; break;
        default: return;
    }
    ScrollLightingTo(pos);
}

void MainWindow::Impl::OnLightingWheel(int delta) {
    wheelAccum += delta;
    const int notches = wheelAccum / WHEEL_DELTA;
    if (notches == 0) return;
    wheelAccum -= notches * WHEEL_DELTA;
    ScrollLightingTo(lightingScroll - notches * 3 * S(kScrollLine));
}

// ---------------------------------------------------------------------------
// Discord tab
// ---------------------------------------------------------------------------

void MainWindow::Impl::BuildDiscordPage() {
    HWND page = pages[kTabDiscord];
    chkDiscord = MakeCheck(page, kIdChkDiscord, L"Show a Discord status while a guest is hosting", brand::UiText,
                           brand::UiBack);
    // preview panel (painted) + its labels
    lblPlaying = MakeLabel(page, L"Playing Playcast", FontId::Playing, kWhite, kPreviewBack);
    lblPrevLine1 = MakeFixedLabel(page, L"…", FontId::PrevLine, kPreviewLineColor, kPreviewBack,
                                  SIZE{kPreviewLineW, kPreviewLineH}, SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS);
    lblElapsed = MakeLabel(page, L"12:34 elapsed", FontId::Small, kPreviewElapsedColor, kPreviewBack);

    constexpr const wchar_t* kCaptions[3] = {L"Status line", L"Second line (optional)", L"Hosting template"};
    for (size_t i = 0; i < 3; ++i)
        discordCaptions[i] = MakeLabel(page, kCaptions[i], FontId::Base, brand::UiTextDim, brand::UiBack);
    editLine1 = MakeEdit(page, kIdEditLine1, brand::UiText, brand::UiSurface, false);
    editLine2 = MakeEdit(page, kIdEditLine2, brand::UiText, brand::UiSurface, false);
    editHostTemplate = MakeEdit(page, kIdEditTemplate, brand::UiText, brand::UiSurface, false);
    chkPassthrough = MakeCheck(page, kIdChkPassthrough,
                               L"Pass the guest's current game through (“Hosting {game} in Nonsole Mode”)",
                               brand::UiText, brand::UiBack);
    lblAppIdState = MakeLabel(page, L"", FontId::Base, brand::UiTextDim, brand::UiBack);
}

void MainWindow::Impl::LayoutDiscordPage() {
    // TableLayoutPanel: checkbox (3,3,3,8) / preview panel (3,4,3,0) /
    // rows table (0,10,0,0) / passthrough (3,10,3,3) / link state (3,10,3,3).
    const int padX = S(kPagePadX);
    const int padY = S(kPagePadY);
    const int lineH = LineHeight();

    PlaceCheck(chkDiscord, padX + S(3), padY + S(3));
    const int chkH = CheckSize(chkDiscord).cy;

    const int panelY = padY + S(3) + chkH + S(8) + S(4);
    previewPanel = RECT{padX + S(3), panelY, padX + S(3) + S(kPreviewW), panelY + S(kPreviewH)};
    PlaceLabel(lblPlaying, previewPanel.left + S(kPreviewTextX), previewPanel.top + S(kPreviewPlayingY));
    PlaceLabel(lblPrevLine1, previewPanel.left + S(kPreviewTextX), previewPanel.top + S(kPreviewLineY));
    PlaceLabel(lblElapsed, previewPanel.left + S(kPreviewTextX), previewPanel.top + S(kPreviewElapsedY));

    const int rowsY = previewPanel.bottom + S(10);
    int captionW = 0;
    for (HWND caption : discordCaptions) captionW = std::max(captionW, static_cast<int>(MeasureText(FontId::Base, labels[caption].text).cx) + 2);
    const int editX = padX + S(6) + captionW + S(14) + S(3);
    const int rowH = std::max(lineH + S(12), S(kEditH) + S(6));
    const HWND rowEdits[3] = {editLine1, editLine2, editHostTemplate};
    for (size_t i = 0; i < 3; ++i) {
        const int y = rowsY + static_cast<int>(i) * rowH;
        PlaceLabel(discordCaptions[i], padX + S(6), y + S(6));
        PlaceEdit(rowEdits[i], editX, y + S(3), S(kDiscordEditW));
    }

    const int passY = rowsY + 3 * rowH + S(10);
    PlaceCheck(chkPassthrough, padX + S(3), passY);
    const int stateY = passY + CheckSize(chkPassthrough).cy + S(3) + S(10);
    PlaceLabel(lblAppIdState, padX + S(3), stateY);
}

// ---------------------------------------------------------------------------
// Advanced tab
// ---------------------------------------------------------------------------

void MainWindow::Impl::BuildAdvancedPage() {
    HWND page = pages[kTabAdvanced];
    lblUserCaption = MakeLabel(page, L"Guest account name", FontId::Base, brand::UiTextDim, brand::UiBack);
    editUser = MakeEdit(page, kIdEditUser, brand::UiText, brand::UiSurface, false);
    lblUserBlurb = MakeLabel(page, L"The Windows account Playcast signs guests into (default: NonsoleMode).",
                             FontId::Base, brand::UiTextDim, brand::UiBack);
    chkDisconnected = MakeCheck(page, kIdChkDisconnected,
                                L"Stay stealthy while the guest is disconnected but still signed in", brand::UiText,
                                brand::UiBack);
    chkRegistry = MakeCheck(page, kIdChkRegistry, L"Also watch the Playcast GuestMode registry key (extra signal)",
                            brand::UiText, brand::UiBack);
    btnOpenLog = MakeButton(page, kIdOpenLog, L"Open log", false);
    btnOpenFolder = MakeButton(page, kIdOpenFolder, L"Open install folder", false);
    lblVersion = MakeLabel(page, L"Playcast Companion v" PC_WIDEN(PC_VERSION_STRING), FontId::Base, brand::UiTextDim,
                           brand::UiBack);
}

void MainWindow::Impl::LayoutAdvancedPage() {
    // TableLayoutPanel: account row / blurb (3,2,3,2) / disconnected (3,10,3,3)
    // / registry (3,4,3,3) / buttons flow (0,16,0,0) / version (3,18,3,3).
    const int padX = S(kPagePadX);
    const int padY = S(kPagePadY);
    const int lineH = LineHeight();

    int y = padY;
    const int captionW = MeasureText(FontId::Base, labels[lblUserCaption].text).cx + 2;
    PlaceLabel(lblUserCaption, padX + S(6), y + S(6));
    PlaceEdit(editUser, padX + S(6) + captionW + S(14) + S(3), y + S(3), S(kUserEditW));
    y += std::max(lineH + S(12), S(kEditH) + S(6));

    PlaceLabel(lblUserBlurb, padX + S(3), y + S(2));
    y += S(2) + lineH + S(2);

    PlaceCheck(chkDisconnected, padX + S(3), y + S(10));
    y += S(10) + CheckSize(chkDisconnected).cy + S(3);

    PlaceCheck(chkRegistry, padX + S(3), y + S(4));
    y += S(4) + CheckSize(chkRegistry).cy + S(3);

    y += S(16);
    const int logW = PlaceButton(btnOpenLog, padX + S(3), y);
    PlaceButton(btnOpenFolder, padX + S(3) + logW + S(6) + S(3), y);
    y += S(kButtonH);

    PlaceLabel(lblVersion, padX + S(3), y + S(18));
}

// ---------------------------------------------------------------------------
// Bottom bar
// ---------------------------------------------------------------------------

void MainWindow::Impl::BuildBottomBar() {
    lblFeedback = MakeLabel(hwnd, L"", FontId::Base, brand::UiTextDim, brand::UiBack);
    btnSave = MakeButton(hwnd, kIdSave, L"Save changes", true);
}

void MainWindow::Impl::LayoutBottomBar() {
    const int barY = S(kClientH) - S(kBottomH);
    PlaceLabel(lblFeedback, S(kFeedbackX), barY + S(kFeedbackY));
    Place(btnSave, S(kClientW) - S(kSaveRight), barY + S(kSaveY), S(kSaveW), S(kSaveH));
}

void MainWindow::Impl::LayoutAll() {
    const RECT page = PageRect();
    for (HWND p : pages) Place(p, page.left, page.top, page.right - page.left, page.bottom - page.top);
    LayoutStatusPage();
    LayoutLightingPage();
    LayoutDiscordPage();
    LayoutAdvancedPage();
    LayoutBottomBar();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void MainWindow::Impl::PaintText(HDC hdc, std::wstring_view text, FontId font, COLORREF color, RECT rc, UINT format) {
    ScopedSelect select(hdc, Font(font));
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, text.data(), static_cast<int>(text.size()), &rc, format | DT_NOPREFIX);
}

void MainWindow::Impl::DrawMark(HDC hdc, int x, int y, int size) {
    if (!mark) return;
    Gdiplus::Graphics g(hdc);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    g.DrawImage(mark.get(), Gdiplus::Rect(x, y, size, size));
}

void MainWindow::Impl::PaintMain(HDC hdc, const RECT& client) {
    FillRect(hdc, &client, Brush(brand::UiBack));

    // header band
    RECT header{0, 0, client.right, S(kHeaderH)};
    FillRect(hdc, &header, Brush(brand::Navy));
    DrawMark(hdc, S(kHeaderMarkX), S(kHeaderMarkY), S(kHeaderMarkSize));
    PaintText(hdc, L"Playcast Companion", FontId::Title, kWhite,
             RECT{S(kHeaderTitleX), S(kHeaderTitleY), client.right, S(kHeaderH)}, DT_LEFT | DT_TOP | DT_SINGLELINE);
    PaintText(hdc, L"Lights out and status up while guests play on your PC.", FontId::Subtitle, kHeaderSubtitleColor,
             RECT{S(kHeaderSubtitleX), S(kHeaderSubtitleY), client.right, S(kHeaderH)},
             DT_LEFT | DT_TOP | DT_SINGLELINE);

    // tab strip (DarkTabControl.OnPaint)
    for (int i = 0; i < kTabCount; ++i) {
        const RECT rc = TabRect(i);
        const bool selected = i == activeTab;
        if (selected) {
            FillRect(hdc, &rc, Brush(brand::UiSurface));
            RECT accent{rc.left, rc.bottom - S(kTabUnderline), rc.right, rc.bottom};
            FillRect(hdc, &accent, Brush(brand::Gold));
        }
        PaintText(hdc, kTabNames[i], FontId::Base, selected ? kWhite : brand::UiTextDim, rc,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    RECT border = PageRect();
    InflateRect(&border, 1, 1);
    FrameRect(hdc, &border, Brush(brand::UiBorder));

    // bottom bar is plain UiBack (already filled); the edit frames live in pages
}

void MainWindow::Impl::PaintEditFrames(HWND page, HDC hdc) {
    // A 1 px UiBorder frame around every EDIT on this page (and around the
    // Razer spinner's EDIT + up-down pair as one box).
    auto frameAround = [&](const RECT& windowRect) {
        RECT rc = windowRect;
        MapWindowPoints(nullptr, page, reinterpret_cast<POINT*>(&rc), 2);
        InflateRect(&rc, 1, 1);
        FrameRect(hdc, &rc, Brush(brand::UiBorder));
    };
    for (const auto& [edit, info] : edits) {
        if (GetParent(edit) != page) continue;
        RECT rc{};
        GetWindowRect(edit, &rc);
        if (edit == editTick && updownTick) {
            RECT ud{};
            GetWindowRect(updownTick, &ud);
            RECT both{};
            UnionRect(&both, &rc, &ud);
            rc = both;
        }
        frameAround(rc);
    }
}

void MainWindow::Impl::PaintPage(int index, HDC hdc, const RECT& client) {
    FillRect(hdc, &client, Brush(brand::UiBack));
    if (index == kTabLighting) {
        const int pitch = S(kCardH) + S(kCardGap);
        for (size_t i = 0; i < cards.size(); ++i) {
            const Card& c = cards[i];
            const int cx = S(kCardX);
            const int cy = S(kCardY0) + static_cast<int>(i) * pitch - lightingScroll;
            RECT rc{cx, cy, cx + S(kCardW), cy + S(kCardH)};
            if (rc.bottom < client.top || rc.top > client.bottom) continue;
            FillRect(hdc, &rc, Brush(c.back));
            if (c.border) FrameRect(hdc, &rc, Brush(kFixedSingleBorder));
            if (c.painter) {
                Gdiplus::Graphics g(hdc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                c.painter(g, Gdiplus::Rect(cx + S(kLogoX), cy + S(kLogoY), S(kLogoSize), S(kLogoSize)));
            }
        }
    } else if (index == kTabDiscord) {
        FillRect(hdc, &previewPanel, Brush(kPreviewBack));
        DrawMark(hdc, previewPanel.left + S(kPreviewMarkX), previewPanel.top + S(kPreviewMarkY), S(kPreviewMarkSize));
    }
    PaintEditFrames(pages[static_cast<size_t>(index)], hdc);
}

void MainWindow::Impl::DrawCheckItem(const DRAWITEMSTRUCT& dis, const CheckInfo& info) {
    HDC hdc = dis.hDC;
    const RECT& rc = dis.rcItem;
    FillRect(hdc, &rc, Brush(info.back));

    const int box = S(kCheckBoxSize);
    const int top = (rc.top + rc.bottom - box) / 2;
    RECT boxRect{rc.left, top, rc.left + box, top + box};
    if (info.checked) {
        FillRect(hdc, &boxRect, Brush(brand::Gold));
        // dark check mark
        GdiObject<HPEN> pen(CreatePen(PS_SOLID, std::max(S(2), 2), kSaveTextColor));
        ScopedSelect select(hdc, pen.get());
        const int x = boxRect.left;
        const int y = boxRect.top;
        const POINT pts[3] = {{x + MulDiv(box, 3, 14), y + MulDiv(box, 7, 14)},
                              {x + MulDiv(box, 6, 14), y + MulDiv(box, 10, 14)},
                              {x + MulDiv(box, 11, 14), y + MulDiv(box, 4, 14)}};
        Polyline(hdc, pts, 3);
    }
    FrameRect(hdc, &boxRect, Brush(brand::UiBorder));

    RECT textRect{rc.left + box + S(kCheckGap), rc.top, rc.right, rc.bottom};
    const bool disabled = (dis.itemState & ODS_DISABLED) != 0;
    PaintText(hdc, GetText(dis.hwndItem), FontId::Base, disabled ? brand::UiTextDim : info.fore, textRect,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if ((dis.itemState & ODS_FOCUS) && !(dis.itemState & ODS_NOFOCUSRECT)) {
        RECT focus = textRect;
        focus.right = std::min<LONG>(focus.right, textRect.left + MeasureText(FontId::Base, GetText(dis.hwndItem)).cx + 2);
        DrawFocusRect(hdc, &focus);
    }
}

void MainWindow::Impl::DrawButtonItem(const DRAWITEMSTRUCT& dis, const ButtonInfo& info) {
    HDC hdc = dis.hDC;
    const RECT& rc = dis.rcItem;
    const bool pressed = (dis.itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis.itemState & ODS_DISABLED) != 0;
    COLORREF back = info.back;
    if (!info.gold && (info.hot || pressed)) back = kButtonHover;  // FlatAppearance.MouseOverBackColor
    if (info.gold && pressed) back = RGB(220, 175, 60);
    FillRect(hdc, &rc, Brush(back));
    FrameRect(hdc, &rc, Brush(info.border));
    COLORREF fore = info.fore;
    if (disabled) fore = info.gold ? RGB(90, 80, 50) : brand::UiTextDim;
    PaintText(hdc, GetText(dis.hwndItem), FontId::Base, fore, rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if ((dis.itemState & ODS_FOCUS) && !(dis.itemState & ODS_NOFOCUSRECT)) {
        RECT focus = rc;
        InflateRect(&focus, -S(3), -S(3));
        DrawFocusRect(hdc, &focus);
    }
}

// ---------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------

/// MainForm.Load: populate every control from the live config.
void MainWindow::Impl::LoadFromConfig() {
    const AppConfig& c = app.Config();
    SetWindowTextW(editUser, c.TargetUsername.c_str());
    const int tick = std::clamp(c.TickSeconds, 1, 10);
    SendMessageW(updownTick, UDM_SETPOS32, 0, static_cast<LPARAM>(tick));
    SetWindowTextW(editTick, std::to_wstring(tick).c_str());
    SetChecked(chkDisconnected, c.IncludeDisconnectedSessions);
    SetChecked(chkRegistry, c.RegistryWatch.Enabled);
    SetChecked(cards[0].check, c.RazerEnabled);
    SetChecked(cards[1].check, c.SteelSeries.Enabled);
    SetChecked(cards[2].check, c.Logitech.Enabled);
    SetChecked(cards[3].check, c.Corsair.Enabled);
    SetChecked(cards[4].check, c.OpenRgb.Enabled);
    SetChecked(cards[5].check, c.DynamicLighting.Enabled);
    SetWindowTextW(editOpenRgbProfile, c.OpenRgb.BlackoutProfile.c_str());
    SetChecked(chkDiscord, c.Discord.Enabled);
    SetWindowTextW(editLine1, c.Discord.Details.c_str());
    SetWindowTextW(editLine2, c.Discord.State.c_str());
    SetChecked(chkPassthrough, c.Discord.GamePassthroughEnabled);
    SetWindowTextW(editHostTemplate, c.Discord.HostingTemplate.c_str());
    const DiscordPresence* discord = app.Discord();
    SetLabelText(lblAppIdState, discord && discord->IsConfigured()
                                    ? L"Linked to your Discord app ✓   (the ID lives in config.json)"
                                    : L"Not linked yet — add your Application ID to config.json (see README)");
    UpdatePreviewLine();
    RefreshStatus();
}

void MainWindow::Impl::UpdatePreviewLine() {
    const std::wstring line = Trim(GetText(editLine1));
    SetLabelText(lblPrevLine1, line.empty() ? std::wstring(L"…") : line);
}

/// MainForm.RefreshStatus (1 s timer).
void MainWindow::Impl::RefreshStatus() {
    const bool guest = app.GuestActive();
    SetLabelText(lblSummary, guest ? L"A guest is hosting — stealth mode is on." : L"All quiet — your PC is yours.");
    SetLabelColor(lblSummary, guest ? brand::Amber : brand::Green);

    SetRow(0, guest ? brand::Amber : brand::Green,
           guest ? std::format(L"Signed in ({})", app.Config().TargetUsername) : std::wstring(L"No guest signed in"));

    const auto& lighting = app.Lighting();
    std::vector<ILightingBackend*> enabled;
    for (const auto& backend : lighting)
        if (backend && backend->Enabled()) enabled.push_back(backend.get());
    if (enabled.empty()) {
        SetRow(1, brand::Slate, L"No lighting systems enabled");
    } else if (!guest) {
        SetRow(1, brand::Green,
               std::format(L"Normal — vendor software in control ({} system{} armed)", enabled.size(),
                           enabled.size() == 1 ? L"" : L"s"));
    } else {
        size_t settled = 0;
        for (ILightingBackend* b : enabled)
            if (b->IsHolding() && !IsWarnStatus(b->StatusText())) ++settled;
        const bool allDark = settled == enabled.size();
        SetRow(1, allDark ? brand::Slate : brand::Amber,
               allDark ? std::format(L"Off — all {} system{} dark", settled, settled == 1 ? L"" : L"s")
                       : std::format(L"{} of {} systems dark — see the Lighting tab", settled, enabled.size()));
    }

    const DiscordPresence* discord = app.Discord();
    if (!discord) {
        SetRow(2, brand::Slate, L"—");
    } else {
        const std::wstring status = discord->StatusText();
        const COLORREF color = status.starts_with(L"Live")           ? brand::Green
                               : status.find(L"retrying") != std::wstring::npos ? brand::Amber
                                                                                : brand::Slate;
        SetRow(2, color, status);
    }

    const auto [autoColor, autoText] = ReadAutostartState();
    SetRow(3, autoColor, autoText);

    for (const Card& card : cards) {
        if (!card.backend) continue;
        if (!card.backend->Enabled()) {
            SetLabelColor(card.dot, brand::Slate);
            SetLabelText(card.status, L"Off");
        } else if (card.backend->IsHolding()) {
            const std::wstring status = card.backend->StatusText();
            SetLabelColor(card.dot, IsWarnStatus(status) ? brand::Amber : brand::Slate);
            SetLabelText(card.status, status);
        } else {
            SetLabelColor(card.dot, brand::Green);
            SetLabelText(card.status, L"Ready");
        }
    }
}

void MainWindow::Impl::StartPreview() {
    if (previewing) return;
    previewing = true;
    EnableWindow(btnPreview, FALSE);
    SetWindowTextW(btnPreview, L"Previewing…");
    InvalidateRect(btnPreview, nullptr, TRUE);
    SetTimer(hwnd, kTimerPreview, kPreviewMs, nullptr);
    try {
        app.TestGuestMode();
    } catch (const std::exception& ex) {
        LogInfo(std::string("UI exception: ") + ex.what());
    }
}

void MainWindow::Impl::EndPreview() {
    if (!previewing) return;
    previewing = false;
    if (auto it = buttons.find(btnPreview); it != buttons.end()) it->second.hot = false;
    KillTimer(hwnd, kTimerPreview);
    SetWindowTextW(btnPreview, L"Preview guest mode (10 s)");
    EnableWindow(btnPreview, TRUE);
    InvalidateRect(btnPreview, nullptr, TRUE);
}

/// MainForm.SaveSettings
void MainWindow::Impl::SaveSettings() {
    const std::wstring user = Trim(GetText(editUser));
    if (user.empty()) {
        MessageBoxW(hwnd, L"The guest account name can't be empty.", kWindowTitle, MB_OK | MB_ICONWARNING);
        return;
    }
    if (EqualsNoCase(user, CurrentUserName())) {
        const std::wstring warning = std::format(
            L"\"{}\" is the account you're signed in as right now — your lights would stay off whenever you're logged in.\n\nSave anyway?",
            user);
        if (MessageBoxW(hwnd, warning.c_str(), kWindowTitle, MB_YESNO | MB_ICONWARNING) != IDYES) return;
    }

    int tick = 5;
    try {
        const std::wstring text = Trim(GetText(editTick));
        if (!text.empty()) tick = std::stoi(text);
    } catch (const std::exception&) {
        tick = 5;
    }
    tick = std::clamp(tick, 1, 10);

    const std::wstring profile = Trim(GetText(editOpenRgbProfile));
    const std::wstring hostTemplate = Trim(GetText(editHostTemplate));

    AppConfig& cfg = app.MutableConfig();
    cfg.TargetUsername = user;
    cfg.TickSeconds = tick;
    cfg.IncludeDisconnectedSessions = IsChecked(chkDisconnected);
    cfg.RegistryWatch.Enabled = IsChecked(chkRegistry);
    cfg.RazerEnabled = IsChecked(cards[0].check);
    cfg.SteelSeries.Enabled = IsChecked(cards[1].check);
    cfg.Logitech.Enabled = IsChecked(cards[2].check);
    cfg.Corsair.Enabled = IsChecked(cards[3].check);
    cfg.OpenRgb.Enabled = IsChecked(cards[4].check);
    cfg.OpenRgb.BlackoutProfile = profile.empty() ? std::wstring(L"Blackout") : profile;
    cfg.DynamicLighting.Enabled = IsChecked(cards[5].check);
    cfg.Discord.Enabled = IsChecked(chkDiscord);
    cfg.Discord.Details = Trim(GetText(editLine1));
    cfg.Discord.State = Trim(GetText(editLine2));
    cfg.Discord.GamePassthroughEnabled = IsChecked(chkPassthrough);
    cfg.Discord.HostingTemplate = hostTemplate.empty() ? std::wstring(L"Hosting {game} in Nonsole Mode") : hostTemplate;
    // Discord.ApplicationId is intentionally untouched: config-file only.

    std::wstring error;
    if (!app.SaveConfig(error)) {
        SetLabelText(lblFeedback, L"Couldn't save: " + error);
        LogInfo(L"settings save failed: " + error);
        return;
    }

    LogInfo(L"settings saved from UI");
    SetLabelText(lblFeedback, L"Saved! Lighting toggles apply right away.");
    if (MessageBoxW(hwnd,
                    L"Saved! Lighting and Discord toggles apply right away.\n\nRestart Playcast Companion too, so account/registry changes take effect?",
                    kWindowTitle, MB_YESNO | MB_ICONQUESTION) == IDYES) {
        app.RestartApp();
    }
}

void MainWindow::Impl::OpenInstallFolder() {
    const std::wstring dir = ExeDirectory();
    if (dir.empty()) return;
    ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);  // failures are ignored, as in C#
}

void MainWindow::Impl::OnDpiChanged(int newDpi, const RECT* suggested) {
    if (newDpi <= 0 || newDpi == dpi) return;
    dpi = newDpi;
    BuildFonts();
    ApplyFonts();
    // children are re-laid out from scroll offset 0
    lightingScroll = 0;
    wheelAccum = 0;

    RECT frame{0, 0, S(kClientW), S(kClientH)};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    AdjustWindowRectExForDpi(&frame, style, FALSE, exStyle, static_cast<UINT>(dpi));
    int x = 0;
    int y = 0;
    if (suggested) {
        x = suggested->left;
        y = suggested->top;
    } else {
        RECT current{};
        GetWindowRect(hwnd, &current);
        x = current.left;
        y = current.top;
    }
    SetWindowPos(hwnd, nullptr, x, y, frame.right - frame.left, frame.bottom - frame.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    LayoutAll();
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

bool MainWindow::Impl::CaptureWindowPng(const std::wstring& file) {
    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return false;
    const int width = wr.right - wr.left;
    const int height = wr.bottom - wr.top;
    if (width <= 0 || height <= 0) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    WindowDc screen(nullptr);
    void* bits = nullptr;
    GdiObject<HBITMAP> dib(CreateDIBSection(screen.get(), &bi, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!dib || !bits) return false;
    HDC memDc = CreateCompatibleDC(screen.get());
    if (!memDc) return false;
    bool ok = false;
    {
        ScopedSelect select(memDc, dib.get());
        ok = PrintWindow(hwnd, memDc, PW_RENDERFULLCONTENT) != FALSE;
        if (!ok) ok = PrintWindow(hwnd, memDc, 0) != FALSE;
        GdiFlush();
        if (ok) {
            // Ignore the alpha channel: GDI drawing leaves it at 0.
            Gdiplus::Bitmap bitmap(width, height, width * 4, PixelFormat32bppRGB, static_cast<BYTE*>(bits));
            CLSID png{};
            ok = GetEncoderClsid(L"image/png", &png) && bitmap.Save(file.c_str(), &png, nullptr) == Gdiplus::Ok;
        }
    }
    DeleteDC(memDc);
    return ok;
}

/// MainForm.SaveSnapshots (--snapshot dev harness).
void MainWindow::Impl::SaveSnapshots(const std::wstring& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path base(dir);

    auto settle = [this]() {
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        PumpMessages();
        DwmFlush();
    };

    for (int i = 0; i < kTabCount; ++i) {
        SelectTab(i);
        settle();
        RefreshStatus();
        settle();
        const std::wstring name = std::format(L"tab-{}-{}.png", i, kTabFileNames[i]);
        if (!CaptureWindowPng((base / name).wstring())) LogInfo(L"snapshot failed: " + name);

        if (i == kTabLighting) {
            // scrollable pages get a second, bottom-scrolled capture
            ScrollLightingTo(lightingContentH);
            settle();
            const std::wstring scrolled = std::format(L"tab-{}-{}-scrolled.png", i, kTabFileNames[i]);
            if (!CaptureWindowPng((base / scrolled).wstring())) LogInfo(L"snapshot failed: " + scrolled);
            ScrollLightingTo(0);
            settle();
        }
    }
}

// ---------------------------------------------------------------------------
// Message handling
// ---------------------------------------------------------------------------

LRESULT MainWindow::Impl::OnCommand(WPARAM wParam, LPARAM lParam, bool& handled) {
    const int id = LOWORD(wParam);
    const int code = HIWORD(wParam);
    HWND control = reinterpret_cast<HWND>(lParam);
    handled = true;
    if (control && code == BN_CLICKED) {
        if (auto it = checks.find(control); it != checks.end()) {
            it->second.checked = !it->second.checked;
            InvalidateRect(control, nullptr, TRUE);
            return 0;
        }
        switch (id) {
            case kIdPreview: StartPreview(); return 0;
            case kIdSave: SaveSettings(); return 0;
            case kIdOpenLog: app.OpenLog(); return 0;
            case kIdOpenFolder: OpenInstallFolder(); return 0;
            default: break;
        }
    }
    if (control && code == EN_CHANGE && id == kIdEditLine1) {
        UpdatePreviewLine();
        return 0;
    }
    handled = false;
    return 0;
}

LRESULT MainWindow::Impl::OnCtlColor(UINT msg, HDC hdc, HWND control, bool& handled) {
    handled = true;
    if (msg == WM_CTLCOLORSTATIC) {
        if (auto it = labels.find(control); it != labels.end()) {
            SetTextColor(hdc, it->second.fore);
            SetBkMode(hdc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(Brush(it->second.back));
        }
    } else if (msg == WM_CTLCOLOREDIT) {
        if (auto it = edits.find(control); it != edits.end()) {
            SetTextColor(hdc, it->second.fore);
            SetBkColor(hdc, it->second.back);
            SetBkMode(hdc, OPAQUE);
            return reinterpret_cast<LRESULT>(Brush(it->second.back));
        }
    } else if (msg == WM_CTLCOLORBTN) {
        if (auto it = checks.find(control); it != checks.end()) return reinterpret_cast<LRESULT>(Brush(it->second.back));
        if (auto it = buttons.find(control); it != buttons.end()) return reinterpret_cast<LRESULT>(Brush(it->second.back));
    }
    handled = false;
    return 0;
}

bool MainWindow::Impl::OnDrawItem(const DRAWITEMSTRUCT& dis) {
    if (dis.CtlType != ODT_BUTTON) return false;
    if (auto it = checks.find(dis.hwndItem); it != checks.end()) {
        DrawCheckItem(dis, it->second);
        return true;
    }
    if (auto it = buttons.find(dis.hwndItem); it != buttons.end()) {
        DrawButtonItem(dis, it->second);
        return true;
    }
    return false;
}

void MainWindow::Impl::OnButtonHover(HWND button, bool hot) {
    auto it = buttons.find(button);
    if (it == buttons.end() || it->second.hot == hot) return;
    it->second.hot = hot;
    if (hot) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = button;
        TrackMouseEvent(&tme);
    }
    InvalidateRect(button, nullptr, TRUE);
}

LRESULT MainWindow::Impl::MainProc(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            const int w = client.right - client.left;
            const int h = client.bottom - client.top;
            if (hdc && w > 0 && h > 0) {
                HDC mem = CreateCompatibleDC(hdc);
                GdiObject<HBITMAP> bmp(CreateCompatibleBitmap(hdc, w, h));
                if (mem && bmp) {
                    ScopedSelect select(mem, bmp.get());
                    PaintMain(mem, client);
                    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
                } else {
                    PaintMain(hdc, client);
                }
                if (mem) DeleteDC(mem);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT: {
            RECT client{};
            GetClientRect(hwnd, &client);
            PaintMain(reinterpret_cast<HDC>(wParam), client);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_LBUTTONDOWN: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            for (int i = 0; i < kTabCount; ++i) {
                const RECT rc = TabRect(i);
                if (PtInRect(&rc, pt)) {
                    SelectTab(i);
                    SetFocus(hwnd);
                    break;
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            bool handled = false;
            const LRESULT r = OnCommand(wParam, lParam, handled);
            if (handled) return r;
            break;
        }
        case WM_DRAWITEM:
            if (OnDrawItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) return TRUE;
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN: {
            bool handled = false;
            const LRESULT r = OnCtlColor(msg, reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam), handled);
            if (handled) return r;
            break;
        }
        case WM_TIMER:
            if (wParam == kTimerRefresh) {
                RefreshStatus();
                return 0;
            }
            if (wParam == kTimerPreview) {
                EndPreview();
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            if (activeTab == kTabLighting) {
                OnLightingWheel(GET_WHEEL_DELTA_WPARAM(wParam));
                return 0;
            }
            break;
        case WM_DPICHANGED:
            OnDpiChanged(static_cast<int>(HIWORD(wParam)), reinterpret_cast<const RECT*>(lParam));
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTimerRefresh);
            KillTimer(hwnd, kTimerPreview);
            return 0;
        case WM_NCDESTROY:
            // Every child is gone by now; forget all handles.
            labels.clear();
            edits.clear();
            checks.clear();
            buttons.clear();
            pages.fill(nullptr);
            hwnd = nullptr;
            previewing = false;
            lightingScroll = 0;
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::Impl::PageProc(HWND page, UINT msg, WPARAM wParam, LPARAM lParam) {
    int index = -1;
    for (int i = 0; i < kTabCount; ++i)
        if (pages[static_cast<size_t>(i)] == page) index = i;
    if (index < 0) return DefWindowProcW(page, msg, wParam, lParam);

    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(page, &ps);
            RECT client{};
            GetClientRect(page, &client);
            const int w = client.right - client.left;
            const int h = client.bottom - client.top;
            if (hdc && w > 0 && h > 0) {
                HDC mem = CreateCompatibleDC(hdc);
                GdiObject<HBITMAP> bmp(CreateCompatibleBitmap(hdc, w, h));
                if (mem && bmp) {
                    ScopedSelect select(mem, bmp.get());
                    PaintPage(index, mem, client);
                    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
                } else {
                    PaintPage(index, hdc, client);
                }
                if (mem) DeleteDC(mem);
            }
            EndPaint(page, &ps);
            return 0;
        }
        case WM_PRINTCLIENT: {
            RECT client{};
            GetClientRect(page, &client);
            PaintPage(index, reinterpret_cast<HDC>(wParam), client);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_LBUTTONDOWN:
            SetFocus(page);
            return 0;
        case WM_VSCROLL:
            if (index == kTabLighting && lParam == 0) {  // NULL = the page scrollbar; the up-down passes its HWND
                OnLightingVScroll(LOWORD(wParam));
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            if (index == kTabLighting) {
                OnLightingWheel(GET_WHEEL_DELTA_WPARAM(wParam));
                return 0;
            }
            break;
        case WM_COMMAND: {
            bool handled = false;
            const LRESULT r = OnCommand(wParam, lParam, handled);
            if (handled) return r;
            break;
        }
        case WM_DRAWITEM:
            if (OnDrawItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) return TRUE;
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN: {
            bool handled = false;
            const LRESULT r = OnCtlColor(msg, reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam), handled);
            if (handled) return r;
            break;
        }
        default: break;
    }
    return DefWindowProcW(page, msg, wParam, lParam);
}

LRESULT CALLBACK MainWindow::Impl::MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        auto* self = static_cast<Impl*>(cs->lpCreateParams);
        if (self) self->hwnd = hwnd;
    }
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    try {
        return self->MainProc(msg, wParam, lParam);
    } catch (const std::exception& ex) {
        LogInfo(std::string("UI exception: ") + ex.what());
    } catch (...) {
        LogInfo(L"UI exception: unknown");
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK MainWindow::Impl::PageWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    try {
        return self->PageProc(hwnd, msg, wParam, lParam);
    } catch (const std::exception& ex) {
        LogInfo(std::string("UI exception: ") + ex.what());
    } catch (...) {
        LogInfo(L"UI exception: unknown");
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/// Common subclass for EDIT/BUTTON children: Tab navigation (there is no
/// IsDialogMessage in the app's message loop), hover tracking for the flat
/// buttons, and wheel forwarding so the card stack scrolls under any control.
LRESULT CALLBACK MainWindow::Impl::ControlSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                       UINT_PTR subclassId, DWORD_PTR refData) {
    auto* self = reinterpret_cast<Impl*>(refData);
    switch (msg) {
        case WM_KEYDOWN:
            if (wParam == VK_TAB && self && self->hwnd) {
                const BOOL previous = GetKeyState(VK_SHIFT) < 0 ? TRUE : FALSE;
                HWND next = GetNextDlgTabItem(self->hwnd, hwnd, previous);
                if (next && next != hwnd) {
                    SetFocus(next);
                    wchar_t cls[16] = {};
                    if (GetClassNameW(next, cls, static_cast<int>(std::size(cls))) && _wcsicmp(cls, L"EDIT") == 0)
                        SendMessageW(next, EM_SETSEL, 0, static_cast<LPARAM>(-1));
                }
                return 0;
            }
            break;
        case WM_CHAR:
            if (wParam == VK_TAB) return 0;
            break;
        case WM_MOUSEMOVE:
            if (self) self->OnButtonHover(hwnd, true);
            break;
        case WM_MOUSELEAVE:
            if (self) self->OnButtonHover(hwnd, false);
            break;
        case WM_MOUSEWHEEL:
            if (HWND parent = GetParent(hwnd)) return SendMessageW(parent, msg, wParam, lParam);
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, ControlSubclassProc, subclassId);
            break;
        default: break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// MainWindow (public surface)
// ---------------------------------------------------------------------------

MainWindow::MainWindow(TrayApp& app) : impl_(std::make_unique<Impl>(app)) {}

MainWindow::~MainWindow() = default;

void MainWindow::Show() {
    if (!IsAlive()) {
        try {
            impl_->Create();
        } catch (const std::exception& ex) {
            LogInfo(std::string("settings window failed: ") + ex.what());
            if (impl_->hwnd && IsWindow(impl_->hwnd)) DestroyWindow(impl_->hwnd);
            impl_->hwnd = nullptr;
            return;
        }
    }
    ShowWindow(impl_->hwnd, SW_SHOWNORMAL);
    UpdateWindow(impl_->hwnd);
    Activate();
}

void MainWindow::Activate() {
    if (!IsAlive()) return;
    if (IsIconic(impl_->hwnd)) ShowWindow(impl_->hwnd, SW_RESTORE);
    SetForegroundWindow(impl_->hwnd);
    BringWindowToTop(impl_->hwnd);
    SetActiveWindow(impl_->hwnd);
}

bool MainWindow::IsAlive() const {
    return impl_->hwnd != nullptr && IsWindow(impl_->hwnd);
}

HWND MainWindow::Handle() const {
    return impl_->hwnd;
}

void MainWindow::SaveSnapshots(const std::wstring& dir) {
    if (!IsAlive()) {
        Show();
        if (!IsAlive()) return;
    }
    try {
        impl_->SaveSnapshots(dir);
    } catch (const std::exception& ex) {
        LogInfo(std::string("snapshot failed: ") + ex.what());
    }
}

}  // namespace pc
