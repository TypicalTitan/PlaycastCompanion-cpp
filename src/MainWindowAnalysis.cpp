#include "pch.h"
#include "MainWindowInternal.h"
#include "TrayApp.h"
#include "Log.h"
#include <shobjidl.h>

namespace pc {
namespace {
std::wstring ValueText(std::optional<double> value, const wchar_t* suffix = L"") {
    return value ? std::format(L"{:.2f}{}", *value, suffix) : L"Unknown / not observed";
}
std::wstring CountText(std::optional<size_t> value) { return value ? std::to_wstring(*value) : L"Unknown"; }
}
void MainWindow::Impl::BuildAnalysis() {
    Button(ui::ImportFiles, ui::AnalyzeLogs, L"Import files", 0, 88, 138, true);
    Button(ui::ImportFolder, ui::AnalyzeLogs, L"Import folder", 148, 88, 138);
    Button(ui::LoadSample, ui::AnalyzeLogs, L"Load sample", 296, 88, 138);
    auto view = Add(ui::AnalysisView, ui::AnalyzeLogs, L"COMBOBOX", L"", 310, 88, 200, 250, CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL);
    for (auto name : ui::AnalysisNames) SendMessageW(view, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    SendMessageW(view, CB_SETCURSEL, 0, 0);
    importSummary = Label(ui::AnalyzeLogs, L"No capture imported. Choose local files, a session folder or the synthetic sample.", 0, 130, -1, 32);
    importWarning = Label(ui::AnalyzeLogs, L"Local analysis only. Scripts in logs are displayed and never executed.", 0, 164, -1, 36);
    auto filter = Add(ui::AnalysisFilter, ui::AnalyzeLogs, L"COMBOBOX", L"", 0, 364, 184, 250, CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL);
    for (auto name : {L"All categories", L"realtime", L"ipc", L"scripts", L"games", L"diagnostics", L"session", L"Capture gaps"})
        SendMessageW(filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    SendMessageW(filter, CB_SETCURSEL, 0, 0);
    Button(ui::PreviousPage, ui::AnalyzeLogs, L"Previous 500", 196, 364, 128);
    Button(ui::NextPage, ui::AnalyzeLogs, L"Next 500", 334, 364, 128);
    Add(ui::AnalysisList, ui::AnalyzeLogs, L"LISTBOX", L"", 0, 410, -1, 110,
        WS_BORDER | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT);
    auto details = Add(ui::AnalysisDetails, ui::AnalyzeLogs, L"EDIT", L"Select a record to inspect its source and captured fields.", 0, 530, -1, 130,
        WS_BORDER | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOHSCROLL | ES_AUTOVSCROLL);
    SendMessageW(details, WM_SETFONT, reinterpret_cast<WPARAM>(monoFont.Get()), FALSE);
    SendMessageW(details, EM_SETLIMITTEXT, 24 * 1024 * 1024, 0);
    contentHeights[ui::AnalyzeLogs] = 660;
}
void MainWindow::Impl::LayoutAnalysis() {
    if (!pages[ui::AnalyzeLogs] || !controls.contains(ui::AnalysisList)) return;
    RECT client{}; GetClientRect(pages[ui::AnalyzeLogs], &client);
    const int width = MulDiv(client.right, 96, static_cast<int>(dpi));
    const int height = MulDiv(client.bottom, 96, static_cast<int>(dpi));
    const bool graph = analysisTab == ui::Timeline || analysisTab == ui::Performance;
    const int canvasHeight = std::max(height, 660);
    const int toolbar = graph ? 384 : 204, listTop = toolbar + 44;
    const int listHeight = std::max(90, (canvasHeight - listTop - 12) / 3);
    auto move = [&](int id, int x, int y, int w, int h) {
        MoveWindow(controls.at(id).Window, Scale(x), Scale(y - offsets[ui::AnalyzeLogs]), Scale(std::max(30, w)), Scale(std::max(24, h)), TRUE);
    };
    move(ui::AnalysisView, width - 210, 130, 200, 250);
    MoveWindow(importSummary, 0, Scale(130 - offsets[ui::AnalyzeLogs]), Scale(std::max(100, width - 228)), Scale(32), TRUE);
    move(ui::AnalysisFilter, 0, toolbar, 184, 250);
    move(ui::PreviousPage, 196, toolbar, 128, 34); move(ui::NextPage, 334, toolbar, 128, 34);
    move(ui::AnalysisList, 0, listTop, width - 10, listHeight);
    move(ui::AnalysisDetails, 0, listTop + listHeight + 10, width - 10, canvasHeight - listTop - listHeight - 14);
    const bool filterable = analysisTab == ui::Timeline || analysisTab == ui::Communications;
    EnableWindow(controls.at(ui::AnalysisFilter).Window, filterable);
    contentHeights[ui::AnalyzeLogs] = canvasHeight;
    SCROLLINFO scroll{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS, 0, canvasHeight - 1,
        static_cast<UINT>(height), offsets[ui::AnalyzeLogs], 0}; SetScrollInfo(pages[ui::AnalyzeLogs], SB_VERT, &scroll, TRUE);
}
void MainWindow::Impl::Import(bool folder) {
    if (importing) return;
    winrt::com_ptr<IFileOpenDialog> dialog;
    winrt::check_hresult(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())));
    DWORD options = 0; winrt::check_hresult(dialog->GetOptions(&options));
    winrt::check_hresult(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_ALLOWMULTISELECT)));
    if (!folder) {
        const COMDLG_FILTERSPEC filters[] = {{L"Session capture files", L"*.jsonl;*.ndjson;*.log"}, {L"All files", L"*.*"}};
        winrt::check_hresult(dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters));
    }
    const auto shown = dialog->Show(hwnd);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    winrt::check_hresult(shown);
    winrt::com_ptr<IShellItemArray> items; winrt::check_hresult(dialog->GetResults(items.put()));
    DWORD count = 0; winrt::check_hresult(items->GetCount(&count));
    std::vector<std::filesystem::path> paths;
    for (DWORD index = 0; index < count; ++index) {
        winrt::com_ptr<IShellItem> item; winrt::check_hresult(items->GetItemAt(index, item.put()));
        PWSTR path = nullptr; winrt::check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &path));
        struct FreePath { PWSTR Path; ~FreePath() { CoTaskMemFree(Path); } } release{path};
        paths.emplace_back(path);
    }
    BeginImport(std::move(paths));
}
void MainWindow::Impl::BeginImport(std::vector<std::filesystem::path> sources, std::unique_ptr<TemporaryLogSample> sample) {
    if (importing) return;
    if (importThread.joinable()) importThread.join();
    importing = true; EnableWindow(controls.at(ui::ImportFiles).Window, FALSE); EnableWindow(controls.at(ui::ImportFolder).Window, FALSE);
    EnableWindow(controls.at(ui::LoadSample).Window, FALSE);
    SetWindowTextW(importSummary, L"Importing local capture files...");
    importThread = std::jthread([this, paths = std::move(sources), temporary = std::move(sample)](std::stop_token stop) mutable {
        auto result = std::make_shared<LogAnalysisResult>(); bool apartment = false;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded); apartment = true;
            *result = LogAnalysisImporter{}.Import(paths, stop);
        } catch (...) { result->Warnings.push_back(L"Import failed. The source may be unreadable or malformed."); }
        if (apartment) winrt::uninit_apartment();
        if (temporary) { result->Warnings.insert(result->Warnings.begin(), L"Synthetic sample; games, processes and events are examples."); temporary.reset(); }
        if (stop.stop_requested()) return;
        { std::lock_guard lock(importMutex); pendingAnalysis = std::move(result); }
        PostMessageW(hwnd, ui::ImportComplete, 0, 0);
    });
}
void MainWindow::Impl::FinishImport() {
    { std::lock_guard lock(importMutex); analysisResult = std::move(pendingAnalysis); }
    importing = false; EnableWindow(controls.at(ui::ImportFiles).Window, TRUE); EnableWindow(controls.at(ui::ImportFolder).Window, TRUE);
    EnableWindow(controls.at(ui::LoadSample).Window, TRUE);
    if (!analysisResult) return;
    const auto& result = *analysisResult;
    SetWindowTextW(importSummary, std::format(L"{} files · {} records · {:.1f} MiB · {} capture-gap events", result.FilesRead,
        result.Records.size(), static_cast<double>(result.BytesRead) / (1024 * 1024), result.Gaps.size()).c_str());
    std::wstring warning = result.Warnings.empty() ? L"No import warnings. Unknown values remain unknown." : result.Warnings.front();
    if (result.Warnings.size() > 1) warning += std::format(L" (+{} more; see record details)", result.Warnings.size() - 1);
    if (result.SecretRecords > 0) warning += std::format(L" · {} records marked raw secrets", result.SecretRecords);
    SetWindowTextW(importWarning, warning.c_str()); analysisOffset = 0; RefreshAnalysis(); LayoutAnalysis();
    InvalidateRect(pages[ui::AnalyzeLogs], nullptr, TRUE);
}
void MainWindow::Impl::RefreshAnalysis() {
    analysisRows.clear(); const auto list = controls.at(ui::AnalysisList).Window;
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    std::vector<size_t> matching;
    if (analysisResult) {
        const auto& result = *analysisResult;
        if (analysisTab == ui::Scripts) for (size_t i = 0; i < result.Scripts.size(); ++i) matching.push_back(i);
        else if (analysisTab == ui::Performance) for (size_t i = 0; i < result.Metrics.size(); ++i) matching.push_back(i);
        else if (analysisTab == ui::Games) { if (result.Games.Observed) matching.push_back(0); }
        else {
            const auto selected = SendMessageW(controls.at(ui::AnalysisFilter).Window, CB_GETCURSEL, 0, 0);
            constexpr const wchar_t* categories[] = {L"", L"realtime", L"ipc", L"scripts", L"games", L"diagnostics", L"session"};
            for (size_t i = 0; i < result.Records.size(); ++i) {
                const auto& record = result.Records[i];
                if (analysisTab == ui::Communications && record.Category != L"realtime" && record.Category != L"ipc") continue;
                if (selected == 7 && !std::binary_search(result.Gaps.begin(), result.Gaps.end(), i)) continue;
                if (selected > 0 && selected < 7 && record.Category != categories[selected]) continue;
                matching.push_back(i);
            }
        }
    }
    if (analysisOffset >= matching.size()) analysisOffset = matching.empty() ? 0 : (matching.size() - 1) / 500 * 500;
    const auto end = std::min(matching.size(), analysisOffset + 500);
    for (size_t i = analysisOffset; i < end; ++i) {
        analysisRows.push_back(matching[i]); const auto text = AnalysisRowText(matching[i]);
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendMessageW(list, LB_SETHORIZONTALEXTENT, Scale(1200), 0);
    EnableWindow(controls.at(ui::PreviousPage).Window, analysisOffset > 0);
    EnableWindow(controls.at(ui::NextPage).Window, end < matching.size());
    if (!analysisRows.empty()) SendMessageW(list, LB_SETCURSEL, 0, 0);
    SelectAnalysisRow();
}
std::wstring MainWindow::Impl::AnalysisRowText(size_t index) const {
    const auto& result = *analysisResult;
    if (analysisTab == ui::Scripts) { const auto& script = result.Scripts[index]; return script.Name + L" · " + script.Status + L" · " + script.Execution; }
    if (analysisTab == ui::Games) return L"Latest passive inventory · installed entries " + CountText(result.Games.InstalledCount) + L" · running game processes " + CountText(result.Games.RunningCount);
    if (analysisTab == ui::Performance) {
        const auto& sample = result.Metrics[index];
        return sample.Timestamp + L" · memory " + ValueText(sample.WorkingSetBytes ? std::optional(*sample.WorkingSetBytes / (1024 * 1024)) : std::nullopt, L" MiB")
            + L" · CPU " + ValueText(sample.CpuPercent, L" %");
    }
    const auto& record = result.Records[index];
    return record.Timestamp + L" · " + record.Category + L" · " + record.Direction + L" " + record.Type + L" · " + record.Source.filename().wstring() + L":" + std::to_wstring(record.Line);
}
void MainWindow::Impl::SelectAnalysisRow() {
    std::wstring details = L"No matching records. Import a capture or choose another view/category.";
    const auto selected = SendMessageW(controls.at(ui::AnalysisList).Window, LB_GETCURSEL, 0, 0);
    if (analysisResult && selected >= 0 && static_cast<size_t>(selected) < analysisRows.size()) {
        const auto index = analysisRows[static_cast<size_t>(selected)]; const auto& result = *analysisResult;
        if (analysisTab == ui::Scripts) details = LogAnalysisProjection::ScriptDetail(result.Scripts[index], result);
        else if (analysisTab == ui::Games) {
            const auto& games = result.Games;
            details = L"Latest passive inventory\r\nCaptured: " + games.Timestamp + L"\r\nSource: " + games.Source
                + L"\r\nInstalled entries: " + CountText(games.InstalledCount) + L"\r\nRunning game processes: " + CountText(games.RunningCount)
                + L"\r\nRunning status: " + games.RunningStatus + L"\r\n\r\nInstalled games\r\n" + games.InstalledJson
                + L"\r\n\r\nRunning games\r\n" + games.RunningJson + L"\r\n\r\nInventory sources\r\n" + games.SourcesJson
                + L"\r\n\r\nLimitations\r\n" + games.LimitationsJson;
        } else if (analysisTab == ui::Performance) {
            const auto& sample = result.Metrics[index];
            details = L"Companion process sample\r\n" + sample.Timestamp + L"\r\nProcess: " + sample.ProcessIdentity
                + L"\r\nSource: " + sample.Source.wstring() + L":" + std::to_wstring(sample.Line)
                + L"\r\nWorking set: " + ValueText(sample.WorkingSetBytes, L" bytes") + L"\r\nPrivate bytes: " + ValueText(sample.PrivateBytes, L" bytes")
                + L"\r\nTotal CPU time: " + ValueText(sample.CpuSeconds, L" seconds") + L"\r\nCPU share of machine capacity: " + ValueText(sample.CpuPercent, L" %")
                + L"\r\nCPU percentage is derived only from consecutive samples of the same process, with a known logical processor count. A missing first sample is unknown.";
        } else details = LogAnalysisProjection::RecordDetail(result.Records[index]);
    }
    if (analysisResult && !analysisResult->Warnings.empty()) {
        details += L"\r\n\r\nImport warnings\r\n";
        for (const auto& warning : analysisResult->Warnings) details += warning + L"\r\n";
    }
    std::wstring windowsLines; windowsLines.reserve(details.size());
    for (size_t index = 0; index < details.size(); ++index) {
        if (details[index] == L'\n' && (index == 0 || details[index - 1] != L'\r')) windowsLines += L'\r';
        windowsLines += details[index];
    }
    SetWindowTextW(controls.at(ui::AnalysisDetails).Window, windowsLines.c_str());
}
} // namespace pc
