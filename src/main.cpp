#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <UIAutomationClient.h>

#include <array>
#include <string>

#pragma comment(lib, "gdi32.lib")

namespace {

constexpr wchar_t kWindowClassName[] = L"TrayKeyboardWindowClass";
constexpr wchar_t kWindowTitle[] = L"TrayKeyboard";
constexpr wchar_t kStartupMenuText[] = L"\u5F00\u673A\u81EA\u542F\u52A8";
constexpr wchar_t kStartupUpdateErrorMessage[] = L"\u65E0\u6CD5\u66F4\u65B0\u5F00\u673A\u81EA\u542F\u52A8\u8BBE\u7F6E\u3002";
constexpr wchar_t kStartupRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kStartupRunValueName[] = L"TrayKeyboard";
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kMenuStartupCommand = 2001;
constexpr UINT kMenuExitCommand = 2000;
constexpr UINT kIconSize = 16;

struct InputTarget {
    HWND topLevelWindow = nullptr;
    HWND focusWindow = nullptr;
};

enum class ActionIconKind {
    Enter,
    Backspace,
    Delete,
};

struct TrayAction {
    UINT id;
    const wchar_t* tooltip;
    UINT virtualKey;
    ActionIconKind iconKind;
    COLORREF accentColor;
};

constexpr std::array<TrayAction, 3> kTrayActions{{
    {1001, L"TrayKeyboard: Enter", VK_RETURN, ActionIconKind::Enter, RGB(14, 165, 233)},
    {1002, L"TrayKeyboard: Backspace", VK_BACK, ActionIconKind::Backspace, RGB(245, 158, 11)},
    {1003, L"TrayKeyboard: Delete", VK_DELETE, ActionIconKind::Delete, RGB(239, 68, 68)},
}};

UINT gTaskbarCreatedMessage = 0;
HWINEVENTHOOK gForegroundHook = nullptr;
HWINEVENTHOOK gFocusHook = nullptr;
std::array<HICON, kTrayActions.size()> gTrayIcons{};
InputTarget gLastInputTarget;
UINT gLastHandledTrayId = 0;
DWORD gLastHandledTick = 0;
IUIAutomation* gAutomation = nullptr;
IUIAutomationElement* gFocusedAutomationElement = nullptr;
bool gLaunchAtStartupEnabled = false;

std::wstring GetStartupCommandValue() {
    wchar_t executablePath[MAX_PATH]{};
    const DWORD pathLength = GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
    if (pathLength == 0 || pathLength >= std::size(executablePath)) {
        return {};
    }

    std::wstring commandValue = L"\"";
    commandValue += executablePath;
    commandValue += L"\"";
    return commandValue;
}

bool IsLaunchAtStartupEnabled() {
    wchar_t valueData[1024]{};
    DWORD valueSize = sizeof(valueData);
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        kStartupRunKeyPath,
        kStartupRunValueName,
        RRF_RT_REG_SZ,
        nullptr,
        valueData,
        &valueSize);

    return status == ERROR_SUCCESS && valueData[0] != L'\0';
}

bool SetLaunchAtStartupEnabled(bool enabled) {
    if (enabled) {
        const std::wstring commandValue = GetStartupCommandValue();
        if (commandValue.empty()) {
            return false;
        }

        HKEY runKey = nullptr;
        const LSTATUS createStatus = RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kStartupRunKeyPath,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            nullptr,
            &runKey,
            nullptr);
        if (createStatus != ERROR_SUCCESS) {
            return false;
        }

        const LSTATUS setStatus = RegSetValueExW(
            runKey,
            kStartupRunValueName,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(commandValue.c_str()),
            static_cast<DWORD>((commandValue.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(runKey);
        return setStatus == ERROR_SUCCESS;
    }

    HKEY runKey = nullptr;
    const LSTATUS openStatus = RegOpenKeyExW(HKEY_CURRENT_USER, kStartupRunKeyPath, 0, KEY_SET_VALUE, &runKey);
    if (openStatus == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (openStatus != ERROR_SUCCESS) {
        return false;
    }

    const LSTATUS deleteStatus = RegDeleteValueW(runKey, kStartupRunValueName);
    RegCloseKey(runKey);
    return deleteStatus == ERROR_SUCCESS || deleteStatus == ERROR_FILE_NOT_FOUND;
}

void RefreshLaunchAtStartupState() {
    gLaunchAtStartupEnabled = IsLaunchAtStartupEnabled();
}

bool ShouldSuppressDuplicateTrayAction(UINT trayId) {
    constexpr DWORD kDuplicateWindowMs = 250;
    const DWORD currentTick = GetTickCount();
    if (gLastHandledTrayId == trayId && currentTick - gLastHandledTick <= kDuplicateWindowMs) {
        return true;
    }

    gLastHandledTrayId = trayId;
    gLastHandledTick = currentTick;
    return false;
}

template <typename T>
void ReleaseAndNull(T*& pointer) {
    if (pointer != nullptr) {
        pointer->Release();
        pointer = nullptr;
    }
}

bool IsValidTargetWindow(HWND windowHandle) {
    return windowHandle != nullptr && IsWindow(windowHandle) != FALSE;
}

bool IsTrayOwnedWindow(HWND windowHandle) {
    if (!IsValidTargetWindow(windowHandle)) {
        return false;
    }

    HWND rootWindow = GetAncestor(windowHandle, GA_ROOT);
    if (rootWindow == nullptr) {
        rootWindow = windowHandle;
    }

    wchar_t className[64]{};
    GetClassNameW(rootWindow, className, static_cast<int>(std::size(className)));
    return lstrcmpW(className, kWindowClassName) == 0;
}

bool IsShellTrayWindow(HWND windowHandle) {
    if (!IsValidTargetWindow(windowHandle)) {
        return false;
    }

    HWND rootWindow = GetAncestor(windowHandle, GA_ROOT);
    if (rootWindow == nullptr) {
        rootWindow = windowHandle;
    }

    wchar_t className[64]{};
    GetClassNameW(rootWindow, className, static_cast<int>(std::size(className)));
    return lstrcmpW(className, L"Shell_TrayWnd") == 0 ||
           lstrcmpW(className, L"NotifyIconOverflowWindow") == 0;
}

bool IsChromiumWindow(HWND windowHandle) {
    if (!IsValidTargetWindow(windowHandle)) {
        return false;
    }

    HWND rootWindow = GetAncestor(windowHandle, GA_ROOT);
    if (rootWindow == nullptr) {
        rootWindow = windowHandle;
    }

    wchar_t className[64]{};
    GetClassNameW(rootWindow, className, static_cast<int>(std::size(className)));
    return lstrcmpW(className, L"Chrome_WidgetWin_1") == 0;
}

void CaptureAutomationFocus() {
    ReleaseAndNull(gFocusedAutomationElement);

    if (gAutomation == nullptr) {
        return;
    }

    IUIAutomationElement* focusedElement = nullptr;
    if (SUCCEEDED(gAutomation->GetFocusedElement(&focusedElement)) && focusedElement != nullptr) {
        gFocusedAutomationElement = focusedElement;
    }
}

void UpdateInputTarget(HWND windowHandle) {
    if (!IsValidTargetWindow(windowHandle) || IsTrayOwnedWindow(windowHandle) || IsShellTrayWindow(windowHandle)) {
        return;
    }

    HWND foregroundWindow = GetAncestor(windowHandle, GA_ROOT);
    if (!IsValidTargetWindow(foregroundWindow) || IsTrayOwnedWindow(foregroundWindow) || IsShellTrayWindow(foregroundWindow)) {
        return;
    }

    DWORD targetThreadId = GetWindowThreadProcessId(foregroundWindow, nullptr);
    GUITHREADINFO threadInfo{};
    threadInfo.cbSize = sizeof(threadInfo);

    HWND focusWindow = foregroundWindow;
    if (targetThreadId != 0 && GetGUIThreadInfo(targetThreadId, &threadInfo) != FALSE &&
        IsValidTargetWindow(threadInfo.hwndFocus)) {
        focusWindow = threadInfo.hwndFocus;
    }

    gLastInputTarget.topLevelWindow = foregroundWindow;
    gLastInputTarget.focusWindow = focusWindow;

    if (IsChromiumWindow(foregroundWindow)) {
        CaptureAutomationFocus();
    } else {
        ReleaseAndNull(gFocusedAutomationElement);
    }
}

void CALLBACK HandleWinEvent(
    HWINEVENTHOOK,
    DWORD event,
    HWND windowHandle,
    LONG,
    LONG,
    DWORD,
    DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND || event == EVENT_OBJECT_FOCUS) {
        UpdateInputTarget(windowHandle);
    }
}

void InstallWindowTrackingHooks() {
    gForegroundHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND,
        nullptr,
        HandleWinEvent,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    gFocusHook = SetWinEventHook(
        EVENT_OBJECT_FOCUS,
        EVENT_OBJECT_FOCUS,
        nullptr,
        HandleWinEvent,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    UpdateInputTarget(GetForegroundWindow());
}

void RemoveWindowTrackingHooks() {
    if (gForegroundHook != nullptr) {
        UnhookWinEvent(gForegroundHook);
        gForegroundHook = nullptr;
    }

    if (gFocusHook != nullptr) {
        UnhookWinEvent(gFocusHook);
        gFocusHook = nullptr;
    }
}

void RestoreInputTarget(const InputTarget& inputTarget) {
    if (!IsValidTargetWindow(inputTarget.topLevelWindow)) {
        return;
    }

    if (IsIconic(inputTarget.topLevelWindow) != FALSE) {
        ShowWindow(inputTarget.topLevelWindow, SW_RESTORE);
    }

    DWORD currentThreadId = GetCurrentThreadId();
    HWND foregroundWindow = GetForegroundWindow();
    DWORD foregroundThreadId = GetWindowThreadProcessId(foregroundWindow, nullptr);
    DWORD targetThreadId = GetWindowThreadProcessId(inputTarget.topLevelWindow, nullptr);
    const bool attachInput = targetThreadId != 0 && targetThreadId != currentThreadId;
    const bool attachForeground = foregroundThreadId != 0 && foregroundThreadId != currentThreadId && foregroundThreadId != targetThreadId;

    if (attachForeground) {
        AttachThreadInput(currentThreadId, foregroundThreadId, TRUE);
    }

    if (attachInput) {
        AttachThreadInput(currentThreadId, targetThreadId, TRUE);
    }

    SetWindowPos(
        inputTarget.topLevelWindow,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(
        inputTarget.topLevelWindow,
        HWND_NOTOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(inputTarget.topLevelWindow);
    SetForegroundWindow(inputTarget.topLevelWindow);
    SetActiveWindow(inputTarget.topLevelWindow);

    if (IsValidTargetWindow(inputTarget.focusWindow)) {
        SetFocus(inputTarget.focusWindow);
    }

    if (IsChromiumWindow(inputTarget.topLevelWindow) && gFocusedAutomationElement != nullptr) {
        gFocusedAutomationElement->SetFocus();
    }

    if (attachInput) {
        AttachThreadInput(currentThreadId, targetThreadId, FALSE);
    }

    if (attachForeground) {
        AttachThreadInput(currentThreadId, foregroundThreadId, FALSE);
    }
}

void SendVirtualKey(UINT virtualKey) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = 0;
    inputs[0].ki.wScan = static_cast<WORD>(MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC));
    inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;

    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags |= KEYEVENTF_KEYUP;

    if (virtualKey == VK_DELETE) {
        inputs[0].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        inputs[1].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }

    SendInput(static_cast<UINT>(std::size(inputs)), inputs, sizeof(INPUT));
}

void ExecuteTrayAction(UINT virtualKey, const InputTarget& inputTarget) {
    RestoreInputTarget(inputTarget);
    Sleep(20);
    SendVirtualKey(virtualKey);
}

void FillRoundedTile(HDC deviceContext, const RECT& bounds, COLORREF accentColor) {
    HBRUSH backgroundBrush = CreateSolidBrush(accentColor);
    HPEN borderPen = CreatePen(PS_SOLID, 1, accentColor);
    HGDIOBJ previousBrush = SelectObject(deviceContext, backgroundBrush);
    HGDIOBJ previousPen = SelectObject(deviceContext, borderPen);
    RoundRect(deviceContext, bounds.left, bounds.top, bounds.right, bounds.bottom, 5, 5);
    SelectObject(deviceContext, previousBrush);
    SelectObject(deviceContext, previousPen);
    DeleteObject(backgroundBrush);
    DeleteObject(borderPen);
}

void DrawEnterGlyph(HDC deviceContext) {
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HGDIOBJ previousPen = SelectObject(deviceContext, pen);
    MoveToEx(deviceContext, 11, 4, nullptr);
    LineTo(deviceContext, 11, 9);
    LineTo(deviceContext, 5, 9);
    MoveToEx(deviceContext, 5, 9, nullptr);
    LineTo(deviceContext, 7, 7);
    MoveToEx(deviceContext, 5, 9, nullptr);
    LineTo(deviceContext, 7, 11);
    SelectObject(deviceContext, previousPen);
    DeleteObject(pen);
}

void DrawBackspaceGlyph(HDC deviceContext) {
    POINT points[] = {{5, 4}, {12, 4}, {13, 5}, {13, 11}, {12, 12}, {5, 12}, {2, 8}};
    HBRUSH bodyBrush = CreateSolidBrush(RGB(255, 255, 255));
    HPEN bodyPen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HGDIOBJ previousBrush = SelectObject(deviceContext, bodyBrush);
    HGDIOBJ previousPen = SelectObject(deviceContext, bodyPen);
    Polygon(deviceContext, points, static_cast<int>(std::size(points)));
    SelectObject(deviceContext, previousBrush);
    SelectObject(deviceContext, previousPen);
    DeleteObject(bodyBrush);
    DeleteObject(bodyPen);

    HPEN crossPen = CreatePen(PS_SOLID, 2, RGB(92, 62, 0));
    previousPen = SelectObject(deviceContext, crossPen);
    MoveToEx(deviceContext, 7, 6, nullptr);
    LineTo(deviceContext, 11, 10);
    MoveToEx(deviceContext, 11, 6, nullptr);
    LineTo(deviceContext, 7, 10);
    SelectObject(deviceContext, previousPen);
    DeleteObject(crossPen);
}

void DrawDeleteGlyph(HDC deviceContext) {
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ previousPen = SelectObject(deviceContext, pen);
    HGDIOBJ previousBrush = SelectObject(deviceContext, brush);

    Rectangle(deviceContext, 5, 6, 11, 12);
    Rectangle(deviceContext, 4, 4, 12, 6);
    MoveToEx(deviceContext, 6, 4, nullptr);
    LineTo(deviceContext, 7, 3);
    LineTo(deviceContext, 9, 3);
    LineTo(deviceContext, 10, 4);

    SelectObject(deviceContext, previousPen);
    SelectObject(deviceContext, previousBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

HICON CreateActionIcon(ActionIconKind iconKind, COLORREF accentColor) {
    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        return nullptr;
    }

    HDC colorDc = CreateCompatibleDC(screenDc);
    HDC maskDc = CreateCompatibleDC(screenDc);
    HBITMAP colorBitmap = CreateCompatibleBitmap(screenDc, kIconSize, kIconSize);
    HBITMAP maskBitmap = CreateBitmap(kIconSize, kIconSize, 1, 1, nullptr);
    HGDIOBJ previousColorBitmap = colorBitmap != nullptr ? SelectObject(colorDc, colorBitmap) : nullptr;
    HGDIOBJ previousMaskBitmap = maskBitmap != nullptr ? SelectObject(maskDc, maskBitmap) : nullptr;

    RECT iconBounds{0, 0, static_cast<LONG>(kIconSize), static_cast<LONG>(kIconSize)};
    FillRect(colorDc, &iconBounds, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    PatBlt(maskDc, 0, 0, kIconSize, kIconSize, WHITENESS);

    SetBkMode(colorDc, TRANSPARENT);
    FillRoundedTile(colorDc, iconBounds, accentColor);

    switch (iconKind) {
    case ActionIconKind::Enter:
        DrawEnterGlyph(colorDc);
        break;
    case ActionIconKind::Backspace:
        DrawBackspaceGlyph(colorDc);
        break;
    case ActionIconKind::Delete:
        DrawDeleteGlyph(colorDc);
        break;
    }

    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = colorBitmap;
    iconInfo.hbmMask = maskBitmap;
    HICON iconHandle = CreateIconIndirect(&iconInfo);

    if (previousColorBitmap != nullptr) {
        SelectObject(colorDc, previousColorBitmap);
    }
    if (previousMaskBitmap != nullptr) {
        SelectObject(maskDc, previousMaskBitmap);
    }

    if (colorBitmap != nullptr) {
        DeleteObject(colorBitmap);
    }
    if (maskBitmap != nullptr) {
        DeleteObject(maskBitmap);
    }
    if (colorDc != nullptr) {
        DeleteDC(colorDc);
    }
    if (maskDc != nullptr) {
        DeleteDC(maskDc);
    }
    ReleaseDC(nullptr, screenDc);
    return iconHandle;
}

void CreateTrayIcons() {
    for (size_t index = 0; index < kTrayActions.size(); ++index) {
        if (gTrayIcons[index] != nullptr) {
            DestroyIcon(gTrayIcons[index]);
        }
        gTrayIcons[index] = CreateActionIcon(kTrayActions[index].iconKind, kTrayActions[index].accentColor);
    }
}

void DestroyTrayIcons() {
    for (auto& iconHandle : gTrayIcons) {
        if (iconHandle != nullptr) {
            DestroyIcon(iconHandle);
            iconHandle = nullptr;
        }
    }
}

bool UpdateTrayIcon(HWND windowHandle, const TrayAction& action, DWORD message) {
    NOTIFYICONDATAW notifyIcon{};
    notifyIcon.cbSize = sizeof(notifyIcon);
    notifyIcon.hWnd = windowHandle;
    notifyIcon.uID = action.id;
    notifyIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    notifyIcon.uCallbackMessage = kTrayCallbackMessage;
    const size_t actionIndex = static_cast<size_t>(action.id - kTrayActions.front().id);
    notifyIcon.hIcon = actionIndex < gTrayIcons.size() && gTrayIcons[actionIndex] != nullptr
        ? gTrayIcons[actionIndex]
        : LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    lstrcpynW(notifyIcon.szTip, action.tooltip, static_cast<int>(std::size(notifyIcon.szTip)));

    return Shell_NotifyIconW(message, &notifyIcon) == TRUE;
}

void AddTrayIcons(HWND windowHandle) {
    for (const auto& action : kTrayActions) {
        UpdateTrayIcon(windowHandle, action, NIM_ADD);
    }
}

void RemoveTrayIcons(HWND windowHandle) {
    for (const auto& action : kTrayActions) {
        NOTIFYICONDATAW notifyIcon{};
        notifyIcon.cbSize = sizeof(notifyIcon);
        notifyIcon.hWnd = windowHandle;
        notifyIcon.uID = action.id;
        Shell_NotifyIconW(NIM_DELETE, &notifyIcon);
    }
}

const TrayAction* FindTrayAction(UINT trayId) {
    for (const auto& action : kTrayActions) {
        if (action.id == trayId) {
            return &action;
        }
    }

    return nullptr;
}

void ShowContextMenu(HWND windowHandle, POINT cursorPosition) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    for (const auto& action : kTrayActions) {
        AppendMenuW(menu, MF_STRING, action.id, action.tooltip);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(
        menu,
        MF_STRING | (gLaunchAtStartupEnabled ? MF_CHECKED : MF_UNCHECKED),
        kMenuStartupCommand,
        kStartupMenuText);
    AppendMenuW(menu, MF_STRING, kMenuExitCommand, L"Exit");

    SetForegroundWindow(windowHandle);
    const UINT command = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        cursorPosition.x,
        cursorPosition.y,
        0,
        windowHandle,
        nullptr);

    DestroyMenu(menu);

    if (command == kMenuStartupCommand) {
        const bool targetState = !gLaunchAtStartupEnabled;
        if (SetLaunchAtStartupEnabled(targetState)) {
            gLaunchAtStartupEnabled = targetState;
        } else {
            RefreshLaunchAtStartupState();
            MessageBoxW(windowHandle, kStartupUpdateErrorMessage, kWindowTitle, MB_OK | MB_ICONERROR);
        }
        return;
    }

    if (command == kMenuExitCommand) {
        DestroyWindow(windowHandle);
        return;
    }

    if (const TrayAction* action = FindTrayAction(command)) {
        const InputTarget targetSnapshot = gLastInputTarget;
        ExecuteTrayAction(action->virtualKey, targetSnapshot);
    }
}

LRESULT CALLBACK WindowProcedure(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == gTaskbarCreatedMessage) {
        AddTrayIcons(windowHandle);
        return 0;
    }

    switch (message) {
    case WM_CREATE:
        RefreshLaunchAtStartupState();
        CreateTrayIcons();
        InstallWindowTrackingHooks();
        AddTrayIcons(windowHandle);
        return 0;

    case kTrayCallbackMessage: {
        const UINT trayId = static_cast<UINT>(wParam);
        switch (static_cast<UINT>(lParam)) {
        case WM_LBUTTONDOWN:
            if (ShouldSuppressDuplicateTrayAction(trayId)) {
                return 0;
            }
            if (const TrayAction* action = FindTrayAction(trayId)) {
                const InputTarget targetSnapshot = gLastInputTarget;
                ExecuteTrayAction(action->virtualKey, targetSnapshot);
            }
            return 0;

        case WM_LBUTTONUP:
        case NIN_SELECT:
            if (ShouldSuppressDuplicateTrayAction(trayId)) {
                return 0;
            }
            if (const TrayAction* action = FindTrayAction(trayId)) {
                const InputTarget targetSnapshot = gLastInputTarget;
                ExecuteTrayAction(action->virtualKey, targetSnapshot);
            }
            return 0;

        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            POINT cursorPosition{};
            GetCursorPos(&cursorPosition);
            ShowContextMenu(windowHandle, cursorPosition);
            return 0;
        }

        default:
            return 0;
        }
    }

    case WM_DESTROY:
        RemoveTrayIcons(windowHandle);
        RemoveWindowTrackingHooks();
        DestroyTrayIcons();
        ReleaseAndNull(gFocusedAutomationElement);
        ReleaseAndNull(gAutomation);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(windowHandle, message, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instanceHandle, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&gAutomation));

    gTaskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instanceHandle;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));

    if (RegisterClassExW(&windowClass) == 0) {
        return 1;
    }

    HWND windowHandle = CreateWindowExW(
        0,
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        nullptr,
        nullptr,
        instanceHandle,
        nullptr);

    if (windowHandle == nullptr) {
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    ReleaseAndNull(gFocusedAutomationElement);
    ReleaseAndNull(gAutomation);
    CoUninitialize();

    return static_cast<int>(message.wParam);
}
