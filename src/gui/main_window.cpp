#include "gui/main_window.h"

#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <format>
#include <iterator>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/util.h"
#include "gpu/vulkan_runtime.h"
#include "gui/resource.h"

namespace grd {
namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kShowMessage = WM_APP + 2;
constexpr int kColumnCount = 9;

std::wstring windowText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied =
        GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
    text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return text;
}

void setWindowText(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

void addComboItem(HWND combo, const std::wstring& text, LPARAM data) {
    const LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0,
                                       reinterpret_cast<LPARAM>(text.c_str()));
    if (index != CB_ERR && index != CB_ERRSPACE) {
        SendMessageW(combo, CB_SETITEMDATA, index, data);
    }
}

int comboIndexForData(HWND combo, LPARAM data) {
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        if (SendMessageW(combo, CB_GETITEMDATA, i, 0) == data) {
            return i;
        }
    }
    return CB_ERR;
}

LPARAM comboData(HWND combo) {
    const int index = ComboBox_GetCurSel(combo);
    if (index == CB_ERR) {
        return -1;
    }
    return ComboBox_GetItemData(combo, index);
}

bool checkboxChecked(HWND control) {
    return Button_GetCheck(control) == BST_CHECKED;
}

void setCheckbox(HWND control, bool checked) {
    Button_SetCheck(control, checked ? BST_CHECKED : BST_UNCHECKED);
}

std::wstring sizeText(uint64_t bytes) {
    return std::to_wstring(bytes / (1024ull * 1024ull));
}

}  // namespace

MainWindow::MainWindow(HINSTANCE instance) : instance_(instance) {}

MainWindow::~MainWindow() {
    removeTrayIcon();
    if (accel_ != nullptr) {
        DestroyAcceleratorTable(accel_);
    }
    if (font_ != nullptr && font_ != GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(font_);
    }
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
    }
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT message, WPARAM wParam,
                                     LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* self =
        reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        if (message == WM_NCCREATE) {
            self->hwnd_ = hwnd;
        }
        return self->handleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool MainWindow::create(bool startHidden, std::wstring& error) {
    error.clear();
    startHidden_ = startHidden;
    icon_ = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
    if (icon_ == nullptr) {
        icon_ = LoadIconW(nullptr, IDI_APPLICATION);
    }

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &MainWindow::WndProc;
    windowClass.hInstance = instance_;
    windowClass.hIcon = icon_;
    windowClass.hIconSm = icon_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kClassName;

    if (RegisterClassExW(&windowClass) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = L"Unable to register the main window class: " + lastError();
        return false;
    }

    hwnd_ = CreateWindowExW(0, kClassName, L"GpuRamDriveVk",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            1040, 720, nullptr, nullptr, instance_, this);
    if (hwnd_ == nullptr) {
        error = L"Unable to create the main window: " + lastError();
        return false;
    }

    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    createMenu();
    createControls();

    std::wstring loadError;
    if (!manager_.load(loadError)) {
        showError(L"Unable to load the configuration:\n" + loadError);
    }

    populateStaticControls();

    RECT rect = {};
    GetWindowRect(hwnd_, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd_, nullptr, (screenWidth - width) / 2,
                 (screenHeight - height) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    RECT client = {};
    GetClientRect(hwnd_, &client);
    layoutControls(client.right, client.bottom);

    if (startHidden_) {
        manager_.mountAutoStart();
        refreshList();
        bool anyMounted = false;
        for (const auto& status : manager_.statuses()) {
            anyMounted = anyMounted || status.mounted;
        }
        if (anyMounted) {
            addTrayIcon();
            ShowWindow(hwnd_, SW_HIDE);
        } else {
            addTrayIcon();
            ShowWindow(hwnd_, SW_SHOW);
            SetForegroundWindow(hwnd_);
        }
    } else {
        addTrayIcon();
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
    }
    return true;
}

int MainWindow::runMessageLoop() {
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (accel_ != nullptr && hwnd_ != nullptr &&
            TranslateAcceleratorW(hwnd_, accel_, &message) != 0) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void MainWindow::createMenu() {
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU tools = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

    AppendMenuW(file, MF_STRING, IDM_FILE_NEW, L"&New drive\tCtrl+N");
    AppendMenuW(file, MF_STRING, IDM_FILE_REMOVE, L"&Remove drive");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_FILE_MOUNT, L"&Mount");
    AppendMenuW(file, MF_STRING, IDM_FILE_UNMOUNT, L"&Unmount");
    AppendMenuW(file, MF_STRING, IDM_FILE_SAVE_IMAGE, L"&Save image\tCtrl+S");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_FILE_EXIT, L"E&xit");

    AppendMenuW(view, MF_STRING, IDM_VIEW_REFRESH, L"&Refresh\tF5");
    AppendMenuW(view, MF_STRING, IDM_VIEW_GPUS, L"Refresh &GPUs");

    AppendMenuW(tools, MF_STRING, IDM_TOOLS_AUTOSTART, L"&Autostart task");
    AppendMenuW(tools, MF_STRING, IDM_TOOLS_CONFIG, L"Open config &folder");
    AppendMenuW(tools, MF_STRING, IDM_TOOLS_LOG, L"Open &log");

    AppendMenuW(help, MF_STRING, IDM_HELP_ABOUT, L"&About");

    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), L"&Tools");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    SetMenu(hwnd_, menu);

    ACCEL accelerators[] = {
        {FVIRTKEY | FCONTROL, 'N', IDM_FILE_NEW},
        {FVIRTKEY | FCONTROL, 'S', IDM_FILE_SAVE_IMAGE},
        {FVIRTKEY, VK_F5, IDM_VIEW_REFRESH},
    };
    accel_ = CreateAcceleratorTableW(
        accelerators, static_cast<int>(std::size(accelerators)));
}

void MainWindow::createControls() {
    HDC screen = GetDC(nullptr);
    const int dpi = screen != nullptr ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen != nullptr) {
        ReleaseDC(nullptr, screen);
    }
    font_ = CreateFontW(
        -MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (font_ == nullptr) {
        font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }

    list_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_DRIVE_LIST), instance_,
        nullptr);
    ListView_SetExtendedListViewStyle(
        list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);

    const wchar_t* titles[kColumnCount] = {
        L"Drive", L"State", L"Size (MiB)", L"GPU", L"FS", L"Label",
        L"TEMP",  L"Image", L"Autostart"};
    const int widths[kColumnCount] = {55, 80, 85, 55, 70, 130, 55, 330, 80};
    for (int i = 0; i < kColumnCount; ++i) {
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.cx = widths[i];
        column.pszText = const_cast<wchar_t*>(titles[i]);
        column.iSubItem = i;
        ListView_InsertColumn(list_, i, &column);
    }

    group_ = CreateWindowExW(
        0, L"BUTTON", L"Drive settings",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(IDC_GROUP), instance_, nullptr);

    auto makeLabel = [this](const wchar_t* text, int x, int y, int width) {
        return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                               x, y, width, 20, group_, nullptr, instance_,
                               nullptr);
    };

    makeLabel(L"Drive:", 12, 27, 45);
    makeLabel(L"GPU:", 150, 27, 35);
    makeLabel(L"Size:", 525, 27, 35);
    makeLabel(L"MiB", 648, 27, 30);
    makeLabel(L"Filesystem:", 12, 59, 68);
    makeLabel(L"Type:", 200, 59, 35);
    makeLabel(L"Label:", 360, 59, 38);
    makeLabel(L"Image:", 12, 91, 42);

    letter_ = CreateWindowExW(
        0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
        60, 24, 70, 240, group_, reinterpret_cast<HMENU>(IDC_LETTER), instance_,
        nullptr);
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        if (letter == L'C') {
            continue;
        }
        addComboItem(letter_, displayLetter(letter), static_cast<LPARAM>(letter));
    }
    ComboBox_SetCurSel(letter_, 0);

    gpu_ = CreateWindowExW(
        0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
        188, 24, 320, 240, group_, reinterpret_cast<HMENU>(IDC_GPU), instance_,
        nullptr);

    size_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"1024",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 562,
        24, 80, 24, group_, reinterpret_cast<HMENU>(IDC_SIZE_MB), instance_,
        nullptr);

    fs_ = CreateWindowExW(
        0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
        80, 56, 110, 200, group_, reinterpret_cast<HMENU>(IDC_FS), instance_,
        nullptr);
    addComboItem(fs_, L"NTFS", static_cast<LPARAM>(FileSystem::Ntfs));
    addComboItem(fs_, L"exFAT", static_cast<LPARAM>(FileSystem::ExFat));
    addComboItem(fs_, L"FAT32", static_cast<LPARAM>(FileSystem::Fat32));
    ComboBox_SetCurSel(fs_, 0);

    type_ = CreateWindowExW(
        0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
        240, 56, 110, 200, group_, reinterpret_cast<HMENU>(IDC_TYPE), instance_,
        nullptr);
    addComboItem(type_, L"Hard disk", static_cast<LPARAM>(DriveKind::HardDisk));
    addComboItem(type_, L"Floppy", static_cast<LPARAM>(DriveKind::Floppy));
    ComboBox_SetCurSel(type_, 0);

    label_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"GpuDisk",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 402, 56, 360, 24,
        group_, reinterpret_cast<HMENU>(IDC_LABEL), instance_, nullptr);

    image_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 60, 88, 360, 24,
        group_, reinterpret_cast<HMENU>(IDC_IMAGE), instance_, nullptr);

    browse_ = CreateWindowExW(
        0, L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        760, 88, 88, 26, group_, reinterpret_cast<HMENU>(IDC_BROWSE), instance_,
        nullptr);

    removable_ = CreateWindowExW(
        0, L"BUTTON", L"Removable",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 60, 120, 100, 22,
        group_, reinterpret_cast<HMENU>(IDC_REMOVABLE), instance_, nullptr);
    readOnly_ = CreateWindowExW(
        0, L"BUTTON", L"Read-only",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 170, 120, 90, 22,
        group_, reinterpret_cast<HMENU>(IDC_READONLY), instance_, nullptr);
    temp_ = CreateWindowExW(
        0, L"BUTTON", L"Use TEMP",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 270, 120, 90, 22,
        group_, reinterpret_cast<HMENU>(IDC_TEMP), instance_, nullptr);
    autostart_ = CreateWindowExW(
        0, L"BUTTON", L"Mount at logon",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 370, 120, 140, 22,
        group_, reinterpret_cast<HMENU>(IDC_AUTOSTART), instance_, nullptr);

    add_ = CreateWindowExW(0, L"BUTTON", L"Add / Update",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP, 12, 154, 100, 28,
                           group_, reinterpret_cast<HMENU>(IDC_ADD), instance_,
                           nullptr);
    remove_ = CreateWindowExW(0, L"BUTTON", L"Remove",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP, 120, 154, 100,
                              28, group_, reinterpret_cast<HMENU>(IDC_REMOVE),
                              instance_, nullptr);
    mount_ = CreateWindowExW(0, L"BUTTON", L"Mount",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP, 228, 154, 100,
                             28, group_, reinterpret_cast<HMENU>(IDC_MOUNT),
                             instance_, nullptr);
    unmount_ = CreateWindowExW(
        0, L"BUTTON", L"Unmount", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 336, 154,
        100, 28, group_, reinterpret_cast<HMENU>(IDC_UNMOUNT), instance_,
        nullptr);
    save_ = CreateWindowExW(0, L"BUTTON", L"Save image",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 444, 154, 100,
                            28, group_, reinterpret_cast<HMENU>(IDC_SAVE),
                            instance_, nullptr);
    format_ = CreateWindowExW(0, L"BUTTON", L"Format",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP, 552, 154, 100,
                              28, group_, reinterpret_cast<HMENU>(IDC_FORMAT),
                              instance_, nullptr);
    refresh_ = CreateWindowExW(0, L"BUTTON", L"Refresh",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP, 660, 154, 100,
                               28, group_, reinterpret_cast<HMENU>(IDC_REFRESH),
                               instance_, nullptr);
    new_ = CreateWindowExW(0, L"BUTTON", L"New",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP, 768, 154, 100, 28,
                           group_, reinterpret_cast<HMENU>(IDC_NEW), instance_,
                           nullptr);

    status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                              WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                              hwnd_, reinterpret_cast<HMENU>(IDC_STATUS), instance_,
                              nullptr);

    SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    EnumChildWindows(
        group_,
        [](HWND child, LPARAM parameter) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(parameter), TRUE);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(font_));

    setWindowText(size_, L"1024");
}

void MainWindow::populateStaticControls() {
    populateGpuCombo();
    refreshList();
    const auto letters = manager_.configs().letters();
    if (!letters.empty()) {
        selected_ = letters.front();
        selectLetterInList(letters.front());
        updateSelection();
    } else {
        clearForm();
    }
    updateMenuState();
}

void MainWindow::populateGpuCombo() {
    SendMessageW(gpu_, CB_RESETCONTENT, 0, 0);
    std::wstring error;
    auto& runtime = VulkanRuntime::instance();
    if (!runtime.initialize(error)) {
        addComboItem(gpu_, L"Vulkan unavailable", -1);
        ComboBox_SetCurSel(gpu_, 0);
        setStatus(L"GPU detection failed: " + error);
        return;
    }

    const auto& devices = runtime.devices();
    for (const auto& device : devices) {
        const std::wstring text = std::format(
            L"[{}] {} ({:.1f} GiB)", device.index, device.name,
            static_cast<double>(device.deviceLocalBytes) /
                (1024.0 * 1024.0 * 1024.0));
        addComboItem(gpu_, text, static_cast<LPARAM>(device.index));
    }
    if (devices.empty()) {
        addComboItem(gpu_, L"No Vulkan devices", -1);
    }
    ComboBox_SetCurSel(gpu_, 0);
}

void MainWindow::layoutControls(int width, int height) {
    if (list_ == nullptr || group_ == nullptr || status_ == nullptr) {
        return;
    }

    SendMessageW(status_, WM_SIZE, 0, 0);
    RECT statusRect = {};
    GetWindowRect(status_, &statusRect);
    const int statusHeight = statusRect.bottom - statusRect.top;

    const int margin = 12;
    const int listHeight = std::max(190, std::min(320, height - 340));
    MoveWindow(list_, margin, margin, width - 2 * margin, listHeight, TRUE);

    const int formTop = margin + listHeight + 10;
    const int formHeight =
        std::max(220, height - formTop - statusHeight - 8);
    const int groupWidth = width - 2 * margin;
    MoveWindow(group_, margin, formTop, groupWidth, formHeight, TRUE);

    MoveWindow(label_, 402, 59, std::max(100, groupWidth - 414), 24, TRUE);
    MoveWindow(image_, 60, 88, std::max(100, groupWidth - 168), 24, TRUE);
    MoveWindow(browse_, groupWidth - 100, 88, 88, 26, TRUE);

    ListView_SetColumnWidth(list_, 7, std::max(120, width - 24 - 609));
}

void MainWindow::refreshList() {
    const std::optional<wchar_t> previous = selected_;
    selected_.reset();
    ListView_DeleteAllItems(list_);

    const auto statuses = manager_.statuses();
    int row = 0;
    int mountedCount = 0;
    for (const auto& status : statuses) {
        const std::wstring drive = displayLetter(status.config.letter);
        const std::wstring state = status.mounted ? L"Mounted" : L"Stopped";
        const std::wstring size =
            sizeText(status.mounted ? status.sizeBytes
                                    : status.config.sizeMiB * 1024ull * 1024ull);
        const std::wstring gpu = std::to_wstring(status.config.gpuIndex);
        const std::wstring filesystem = fileSystemName(status.config.filesystem);
        const std::wstring label = status.config.label;
        const std::wstring temp = status.config.useTempFolder ? L"Yes" : L"No";
        const std::wstring image =
            status.config.imagePath.empty() ? L"-" : status.config.imagePath;
        const std::wstring autoStart =
            status.config.autoStart ? L"Yes" : L"No";
        if (status.mounted) {
            ++mountedCount;
        }

        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.pszText = const_cast<wchar_t*>(drive.c_str());
        item.lParam = static_cast<LPARAM>(status.config.letter);
        const int inserted = ListView_InsertItem(list_, &item);
        if (inserted < 0) {
            continue;
        }

        const wchar_t* values[kColumnCount] = {
            drive.c_str(), state.c_str(),    size.c_str(),   gpu.c_str(),
            filesystem.c_str(), label.c_str(), temp.c_str(), image.c_str(),
            autoStart.c_str()};
        for (int column = 1; column < kColumnCount; ++column) {
            ListView_SetItemText(list_, inserted, column,
                                 const_cast<wchar_t*>(values[column]));
        }
        ++row;
    }

    if (previous) {
        selectLetterInList(*previous);
    }

    if (const auto letter = selectedLetter()) {
        selected_ = letter;
        if (const auto config = manager_.configs().find(*letter)) {
            loadForm(*config);
        }
    } else {
        clearForm();
    }

    updateButtons();
    setStatus(std::format(L"{} configured, {} mounted.", statuses.size(),
                          mountedCount));
}

void MainWindow::selectLetterInList(wchar_t letter) {
    const int count = ListView_GetItemCount(list_);
    for (int i = 0; i < count; ++i) {
        LVITEMW item = {};
        item.mask = LVIF_PARAM;
        item.iItem = i;
        if (ListView_GetItem(list_, &item) != FALSE &&
            item.lParam == static_cast<LPARAM>(letter)) {
            ListView_SetItemState(list_, i,
                                  LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list_, i, FALSE);
            return;
        }
    }
}

void MainWindow::updateSelection() {
    const auto letter = selectedLetter();
    if (!letter) {
        return;
    }
    const auto config = manager_.configs().find(*letter);
    if (!config) {
        return;
    }
    selected_ = letter;
    loadForm(*config);
    updateButtons();
}

void MainWindow::updateButtons() {
    const auto letter = selectedLetter();
    Drive* drive = letter ? manager_.find(*letter) : nullptr;
    const bool mounted = drive != nullptr && drive->mounted();
    const bool writable = drive != nullptr && !drive->config().readOnly;

    EnableWindow(mount_, letter.has_value() && !mounted);
    EnableWindow(unmount_, mounted);
    EnableWindow(save_, mounted && writable &&
                            !drive->config().imagePath.empty());
    EnableWindow(format_, mounted && writable);
    EnableWindow(remove_, letter.has_value());
    EnableWindow(refresh_, TRUE);
    EnableWindow(add_, TRUE);
    EnableWindow(new_, TRUE);
}

void MainWindow::updateMenuState() {
    HMENU menu = GetMenu(hwnd_);
    if (menu == nullptr) {
        return;
    }
    HMENU tools = GetSubMenu(menu, 2);
    if (tools != nullptr) {
        const bool enabled = manager_.autoStartEnabled();
        CheckMenuItem(tools, IDM_TOOLS_AUTOSTART,
                      MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED));
    }
}

void MainWindow::loadForm(const DriveConfig& config) {
    int index = comboIndexForData(letter_, static_cast<LPARAM>(config.letter));
    if (index == CB_ERR) {
        index = 0;
    }
    ComboBox_SetCurSel(letter_, index);

    index = comboIndexForData(gpu_, static_cast<LPARAM>(config.gpuIndex));
    if (index == CB_ERR) {
        addComboItem(gpu_, std::format(L"[{}] (not detected)",
                                       config.gpuIndex),
                     static_cast<LPARAM>(config.gpuIndex));
        index = comboIndexForData(gpu_, static_cast<LPARAM>(config.gpuIndex));
    }
    ComboBox_SetCurSel(gpu_, index == CB_ERR ? 0 : index);

    setWindowText(size_, std::to_wstring(config.sizeMiB));
    index = comboIndexForData(fs_, static_cast<LPARAM>(config.filesystem));
    ComboBox_SetCurSel(fs_, index == CB_ERR ? 0 : index);
    index = comboIndexForData(type_, static_cast<LPARAM>(config.kind));
    ComboBox_SetCurSel(type_, index == CB_ERR ? 0 : index);
    setWindowText(label_, config.label);
    setWindowText(image_, config.imagePath);

    setCheckbox(removable_, config.removable);
    setCheckbox(readOnly_, config.readOnly);
    setCheckbox(temp_, config.useTempFolder);
    setCheckbox(autostart_, config.autoStart);
    selected_ = config.letter;
}

void MainWindow::clearForm() {
    selected_.reset();
    if (ComboBox_GetCount(letter_) > 0) {
        ComboBox_SetCurSel(letter_, 0);
    }
    if (ComboBox_GetCount(gpu_) > 0) {
        ComboBox_SetCurSel(gpu_, 0);
    }
    setWindowText(size_, L"1024");
    ComboBox_SetCurSel(fs_, 0);
    ComboBox_SetCurSel(type_, 0);
    setWindowText(label_, L"GpuDisk");
    setWindowText(image_, L"");
    setCheckbox(removable_, false);
    setCheckbox(readOnly_, false);
    setCheckbox(temp_, false);
    setCheckbox(autostart_, false);
    ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    updateButtons();
}

std::optional<DriveConfig> MainWindow::readForm(std::wstring& error) const {
    error.clear();
    DriveConfig config;

    const LPARAM letter = comboData(letter_);
    if (letter < 0 || !isDriveLetter(static_cast<wchar_t>(letter))) {
        error = L"Choose a drive letter.";
        return std::nullopt;
    }
    config.letter = static_cast<wchar_t>(letter);

    const LPARAM gpu = comboData(gpu_);
    if (gpu < 0 || gpu > UINT32_MAX) {
        error = L"Choose a Vulkan device.";
        return std::nullopt;
    }
    config.gpuIndex = static_cast<uint32_t>(gpu);

    const auto parsed = parseUInt64(windowText(size_));
    if (!parsed || *parsed == 0) {
        error = L"Enter a drive size in MiB.";
        return std::nullopt;
    }
    config.sizeMiB = *parsed;

    const LPARAM filesystem = comboData(fs_);
    if (filesystem < 0 || filesystem > static_cast<LPARAM>(FileSystem::Ntfs)) {
        error = L"Choose a filesystem.";
        return std::nullopt;
    }
    config.filesystem = static_cast<FileSystem>(filesystem);

    const LPARAM kind = comboData(type_);
    if (kind < 0 || kind > static_cast<LPARAM>(DriveKind::Floppy)) {
        error = L"Choose a device type.";
        return std::nullopt;
    }
    config.kind = static_cast<DriveKind>(kind);

    config.label = trim(windowText(label_));
    config.imagePath = trim(windowText(image_));
    config.removable = checkboxChecked(removable_);
    config.readOnly = checkboxChecked(readOnly_);
    config.useTempFolder = checkboxChecked(temp_);
    config.autoStart = checkboxChecked(autostart_);

    if (config.readOnly && config.useTempFolder) {
        error = L"A read-only drive cannot provide TEMP.";
        return std::nullopt;
    }
    if (config.readOnly && config.imagePath.empty()) {
        error = L"A read-only drive requires an image.";
        return std::nullopt;
    }
    return config;
}

std::optional<wchar_t> MainWindow::selectedLetter() const {
    const int item = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (item < 0) {
        return std::nullopt;
    }
    LVITEMW entry = {};
    entry.mask = LVIF_PARAM;
    entry.iItem = item;
    if (ListView_GetItem(list_, &entry) == FALSE) {
        return std::nullopt;
    }
    return static_cast<wchar_t>(entry.lParam);
}

void MainWindow::onAddOrUpdate() {
    std::wstring error;
    const auto config = readForm(error);
    if (!config) {
        showError(error);
        return;
    }
    if (selected_ && *selected_ != config->letter) {
        showError(L"Unmount and remove the old drive before changing its letter.");
        return;
    }
    if (Drive* drive = manager_.find(config->letter);
        drive != nullptr && drive->mounted()) {
        const DriveConfig& current = drive->config();
        if (config->gpuIndex != current.gpuIndex ||
            config->sizeMiB != current.sizeMiB ||
            config->kind != current.kind ||
            config->imagePath != current.imagePath ||
            config->removable != current.removable ||
            config->readOnly != current.readOnly ||
            config->useTempFolder != current.useTempFolder) {
            showError(L"Unmount the drive before changing its size, GPU, "
                      L"device type, image, removable flag, read-only flag, "
                      L"or TEMP setting.");
            return;
        }
    }
    if (!manager_.addOrUpdate(*config, error)) {
        showError(error);
        return;
    }
    selected_ = config->letter;
    refreshList();
    selectLetterInList(config->letter);
    setStatus(std::format(L"Saved drive {}.", displayLetter(config->letter)));
}

void MainWindow::onRemove() {
    const auto letter = selectedLetter();
    if (!letter) {
        return;
    }
    if (!confirm(std::format(L"Remove the configuration for {}?", displayLetter(*letter)),
                 L"Remove drive")) {
        return;
    }

    std::wstring error;
    if (manager_.mounted(*letter)) {
        if (!manager_.unmount(*letter, false, error)) {
            if (!confirm(error + L"\n\nForce unmount and discard unsaved changes?",
                         L"Remove drive")) {
                return;
            }
            if (!manager_.unmount(*letter, true, error)) {
                showError(error);
                return;
            }
        }
    }

    if (!manager_.remove(*letter, error)) {
        showError(error);
        return;
    }
    selected_.reset();
    refreshList();
    setStatus(L"Drive configuration removed.");
}

void MainWindow::onMount() {
    const auto letter = selectedLetter();
    if (!letter) {
        return;
    }
    std::wstring error;
    if (!manager_.mount(*letter, error)) {
        showError(error);
        return;
    }
    selected_ = letter;
    refreshList();
    selectLetterInList(*letter);
    setStatus(std::format(L"Mounted {}:.", displayLetter(*letter)));
}

void MainWindow::onUnmount() {
    const auto letter = selectedLetter();
    if (!letter) {
        return;
    }
    std::wstring error;
    if (!manager_.unmount(*letter, false, error)) {
        if (!confirm(error + L"\n\nForce unmount and discard unsaved changes?",
                     L"Unmount")) {
            return;
        }
        if (!manager_.unmount(*letter, true, error)) {
            showError(error);
            return;
        }
    }
    refreshList();
    setStatus(std::format(L"Unmounted {}:.", displayLetter(*letter)));
}

void MainWindow::onSaveImage() {
    const auto letter = selectedLetter();
    if (!letter) {
        return;
    }
    Drive* drive = manager_.find(*letter);
    if (drive == nullptr) {
        showError(L"The drive is not mounted.");
        return;
    }
    std::wstring error;
    if (!drive->saveNow(error)) {
        showError(error);
        return;
    }
    setStatus(L"Image saved.");
}

void MainWindow::onFormat() {
    const auto letter = selectedLetter();
    if (!letter) {
        return;
    }
    Drive* drive = manager_.find(*letter);
    if (drive == nullptr) {
        showError(L"The drive is not mounted.");
        return;
    }

    std::wstring error;
    const auto config = readForm(error);
    if (!config) {
        showError(error);
        return;
    }
    if (config->letter != *letter) {
        showError(L"The selected drive and the form letter do not match.");
        return;
    }
    if (!confirm(std::format(
                     L"Format {}: as {}? Every file on the drive will be lost.",
                     displayLetter(*letter), fileSystemName(config->filesystem)),
                 L"Format drive")) {
        return;
    }
    if (!drive->formatNow(config->filesystem, config->label, error)) {
        showError(error);
        return;
    }
    DriveConfig updated = drive->config();
    updated.filesystem = config->filesystem;
    updated.label = config->label;
    if (!manager_.addOrUpdate(updated, error)) {
        showError(error);
    }
    refreshList();
    setStatus(L"Drive formatted.");
}

void MainWindow::onNew() {
    clearForm();
    SetFocus(letter_);
    setStatus(L"Enter settings for a new drive.");
}

void MainWindow::onBrowse() {
    std::vector<wchar_t> path(32768, L'\0');
    const std::wstring current = windowText(image_);
    if (!current.empty()) {
        wcsncpy(path.data(), current.c_str(), path.size() - 1);
    }

    const wchar_t filter[] =
        L"Disk images (*.img)\0*.img\0All files (*.*)\0*.*\0\0";
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrTitle = L"Choose image file";
    dialog.lpstrDefExt = L"img";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

    if (GetSaveFileNameW(&dialog) != FALSE) {
        setWindowText(image_, path.data());
    }
}

void MainWindow::onAutostartTask() {
    const bool enabled = manager_.autoStartEnabled();
    std::wstring error;
    if (!manager_.setAutoStart(!enabled, error)) {
        showError(error);
        return;
    }
    updateMenuState();
    setStatus(enabled ? L"Autostart task disabled."
                      : L"Autostart task enabled.");
}

void MainWindow::onOpenConfig() {
    ensureDirectory(configDir());
    ShellExecuteW(nullptr, L"open", configDir().c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
}

void MainWindow::onAbout() {
    MessageBoxW(hwnd_,
                L"GpuRamDriveVk\n\nA Vulkan-backed RAM disk for Windows.\n"
                L"Uses WinSpd as the volume driver.\n\n"
                L"WinSpd - Windows Storage Proxy Driver, "
                L"Copyright (C) Bill Zissimopoulos\n"
                L"https://github.com/winfsp/winspd",
                L"About GpuRamDriveVk", MB_OK | MB_ICONINFORMATION);
}

void MainWindow::addTrayIcon() {
    if (trayAdded_) {
        return;
    }
    tray_ = {};
    tray_.cbSize = sizeof(tray_);
    tray_.hWnd = hwnd_;
    tray_.uID = 1;
    tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray_.uCallbackMessage = kTrayMessage;
    tray_.hIcon = icon_;
    lstrcpynW(tray_.szTip, L"GpuRamDriveVk", ARRAYSIZE(tray_.szTip));
    if (Shell_NotifyIconW(NIM_ADD, &tray_) != FALSE) {
        trayAdded_ = true;
    }
}

void MainWindow::removeTrayIcon() {
    if (trayAdded_) {
        Shell_NotifyIconW(NIM_DELETE, &tray_);
        trayAdded_ = false;
    }
}

void MainWindow::showTrayMenu() {
    SetForegroundWindow(hwnd_);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOW, L"Show GpuRamDriveVk");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_MOUNT, L"Mount autostart drives");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_UNMOUNT, L"Unmount all drives");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Exit");

    POINT point = {};
    GetCursorPos(&point);
    const UINT command = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd_,
        nullptr);
    DestroyMenu(menu);
    if (command != 0) {
        SendMessageW(hwnd_, WM_COMMAND, command, 0);
    }
    PostMessageW(hwnd_, WM_NULL, 0, 0);
}

void MainWindow::showFromTray() {
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
    refreshList();
}

void MainWindow::hideToTray() {
    addTrayIcon();
    if (!trayAdded_) {
        ShowWindow(hwnd_, SW_MINIMIZE);
        setStatus(L"Unable to create a tray icon; the window was minimized.");
        return;
    }
    ShowWindow(hwnd_, SW_HIDE);
}

bool MainWindow::shutdown() {
    if (exiting_) {
        return true;
    }

    std::wstring error;
    if (!manager_.unmountAll(false, error)) {
        if (!confirm(error + L"\n\nForce unmount and discard unsaved changes?",
                     L"Exit")) {
            return false;
        }
        if (!manager_.unmountAll(true, error)) {
            showError(error);
        }
    }

    exiting_ = true;
    removeTrayIcon();
    DestroyWindow(hwnd_);
    return true;
}

void MainWindow::setStatus(const std::wstring& text) {
    if (status_ != nullptr) {
        SendMessageW(status_, SB_SETTEXTW, 0,
                     reinterpret_cast<LPARAM>(text.c_str()));
    }
}

void MainWindow::showError(const std::wstring& error) {
    setStatus(error);
    MessageBoxW(hwnd_, error.c_str(), L"GpuRamDriveVk",
                MB_OK | MB_ICONERROR);
}

bool MainWindow::confirm(const std::wstring& text, const std::wstring& title) {
    return MessageBoxW(hwnd_, text.c_str(), title.c_str(),
                       MB_YESNO | MB_ICONQUESTION) == IDYES;
}

LRESULT MainWindow::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            switch (id) {
                case IDM_FILE_NEW:
                case IDC_NEW:
                    onNew();
                    return 0;
                case IDM_FILE_REMOVE:
                case IDC_REMOVE:
                    onRemove();
                    return 0;
                case IDM_FILE_MOUNT:
                case IDC_MOUNT:
                    onMount();
                    return 0;
                case IDM_FILE_UNMOUNT:
                case IDC_UNMOUNT:
                    onUnmount();
                    return 0;
                case IDM_FILE_SAVE_IMAGE:
                case IDC_SAVE:
                    onSaveImage();
                    return 0;
                case IDC_FORMAT:
                    onFormat();
                    return 0;
                case IDM_FILE_EXIT:
                    shutdown();
                    return 0;
                case IDM_VIEW_REFRESH:
                case IDC_REFRESH:
                    refreshList();
                    return 0;
                case IDM_VIEW_GPUS:
                    populateGpuCombo();
                    if (selected_) {
                        if (const auto config = manager_.configs().find(*selected_)) {
                            loadForm(*config);
                        }
                    }
                    setStatus(L"GPU list refreshed.");
                    return 0;
                case IDM_TOOLS_AUTOSTART:
                    onAutostartTask();
                    return 0;
                case IDM_TOOLS_CONFIG:
                    onOpenConfig();
                    return 0;
                case IDM_TOOLS_LOG:
                    ShellExecuteW(nullptr, L"open",
                                  joinPath(configDir(), L"gpuvramdrive.log").c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                case IDM_HELP_ABOUT:
                    onAbout();
                    return 0;
                case IDC_ADD:
                    onAddOrUpdate();
                    return 0;
                case IDC_BROWSE:
                    onBrowse();
                    return 0;
                case IDM_TRAY_SHOW:
                    showFromTray();
                    return 0;
                case IDM_TRAY_MOUNT:
                    manager_.mountAutoStart();
                    refreshList();
                    return 0;
                case IDM_TRAY_UNMOUNT: {
                    std::wstring error;
                    if (!manager_.unmountAll(false, error)) {
                        if (confirm(error + L"\n\nForce unmount?", L"Unmount")) {
                            manager_.unmountAll(true, error);
                        }
                    }
                    refreshList();
                    return 0;
                }
                case IDM_TRAY_EXIT:
                    shutdown();
                    return 0;
                default:
                    break;
            }
            break;
        }

        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->idFrom == IDC_DRIVE_LIST &&
                header->code == LVN_ITEMCHANGED) {
                updateSelection();
            }
            break;
        }

        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                layoutControls(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = 980;
            info->ptMinTrackSize.y = 620;
            return 0;
        }

        case WM_INITMENU:
            updateMenuState();
            break;

        case WM_CLOSE:
            if (!exiting_) {
                hideToTray();
                return 0;
            }
            break;

        case WM_DESTROY:
            removeTrayIcon();
            if (accel_ != nullptr) {
                DestroyAcceleratorTable(accel_);
                accel_ = nullptr;
            }
            PostQuitMessage(0);
            return 0;

        case WM_NCDESTROY: {
            HWND window = hwnd_;
            hwnd_ = nullptr;
            return DefWindowProcW(window, message, wParam, lParam);
        }

        case WM_QUERYENDSESSION: {
            std::wstring error;
            if (!manager_.unmountAll(false, error)) {
                manager_.unmountAll(true, error);
            }
            return TRUE;
        }

        case WM_ENDSESSION:
            if (wParam != 0) {
                exiting_ = true;
            }
            return 0;

        case kTrayMessage:
            if (lParam == WM_LBUTTONDBLCLK) {
                showFromTray();
            } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                showTrayMenu();
            }
            return 0;

        case kShowMessage:
            showFromTray();
            return 0;

        default:
            if (message == taskbarCreatedMessage_ &&
                taskbarCreatedMessage_ != 0) {
                trayAdded_ = false;
                addTrayIcon();
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

}  // namespace grd
