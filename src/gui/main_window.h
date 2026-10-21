#pragma once

#include <windows.h>
#include <shellapi.h>

#include <optional>
#include <string>

#include "core/drive_manager.h"

namespace grd {

class MainWindow {
public:
    static constexpr wchar_t kClassName[] = L"GpuRamDriveVkMainWindow";

    explicit MainWindow(HINSTANCE instance);
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool create(bool startHidden, std::wstring& error);
    int runMessageLoop();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam,
                                    LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void createControls();
    void createMenu();
    void populateStaticControls();
    void populateGpuCombo();
    void layoutControls(int width, int height);
    void refreshList();
    void selectLetterInList(wchar_t letter);
    void updateSelection();
    void updateButtons();
    void updateMenuState();

    void loadForm(const DriveConfig& config);
    void clearForm();
    std::optional<DriveConfig> readForm(std::wstring& error) const;
    std::optional<wchar_t> selectedLetter() const;

    void onAddOrUpdate();
    void onRemove();
    void onMount();
    void onUnmount();
    void onSaveImage();
    void onFormat();
    void onNew();
    void onBrowse();
    void onAutostartTask();
    void onOpenConfig();
    void onAbout();

    void addTrayIcon();
    void removeTrayIcon();
    void showTrayMenu();
    void showFromTray();
    void hideToTray();
    bool shutdown();
    void setStatus(const std::wstring& text);
    void showError(const std::wstring& error);
    bool confirm(const std::wstring& text, const std::wstring& title);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND list_ = nullptr;
    HWND group_ = nullptr;
    HWND letter_ = nullptr;
    HWND gpu_ = nullptr;
    HWND size_ = nullptr;
    HWND fs_ = nullptr;
    HWND type_ = nullptr;
    HWND label_ = nullptr;
    HWND image_ = nullptr;
    HWND browse_ = nullptr;
    HWND removable_ = nullptr;
    HWND readOnly_ = nullptr;
    HWND temp_ = nullptr;
    HWND autostart_ = nullptr;
    HWND add_ = nullptr;
    HWND remove_ = nullptr;
    HWND mount_ = nullptr;
    HWND unmount_ = nullptr;
    HWND save_ = nullptr;
    HWND format_ = nullptr;
    HWND refresh_ = nullptr;
    HWND new_ = nullptr;
    HWND status_ = nullptr;

    HICON icon_ = nullptr;
    HFONT font_ = nullptr;
    HACCEL accel_ = nullptr;
    NOTIFYICONDATAW tray_ = {};
    UINT taskbarCreatedMessage_ = 0;
    bool trayAdded_ = false;
    bool exiting_ = false;
    bool startHidden_ = false;

    std::optional<wchar_t> selected_;
    DriveManager manager_;
};

}  // namespace grd
