#pragma once
#include <memory>
#include <string>
#include <filesystem>
#include <vector>
#include <windows.h>

namespace pc {
class TrayApp;
/// Native responsive sidebar, draft settings, live status and passive local log analysis.
class MainWindow {
public:
    explicit MainWindow(TrayApp& app);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    void Show();
    void Activate();
    bool IsAlive() const;
    HWND Handle() const;
    void SaveSnapshots(const std::wstring& dir);
    void ImportLogs(const std::vector<std::filesystem::path>& sources);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace pc
