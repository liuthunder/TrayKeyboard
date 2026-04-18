#include <windows.h>
#include <shellapi.h>

#include <array>

#pragma comment(lib, "gdi32.lib")

namespace {

constexpr wchar_t kWindowClassName[] = L"TrayKeyboardWindowClass";
constexpr wchar_t kWindowTitle[] = L"TrayKeyboard";
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kMenuExitCommand = 2000;
constexpr UINT kIconSize = 16;
constexpr COLORREF kIconBackground = RGB(32, 32, 32);
constexpr COLORREF kIconForeground = RGB(245, 245, 245);

struct InputTarget {
    HWND topLevelWindow = nullptr;
    HWND focusWindow = nullptr;
};

struct TrayAction {
    UINT id;
    const wchar_t* tooltip;
    UINT virtualKey;
    wchar_t glyph;
};

constexpr std::array<TrayAction, 3> kTrayActions{{
    {1001, L"TrayKeyboard: Enter", VK_RETURN, L'\u23CE'},
    {1002, L"TrayKeyboard: Backspace", VK_BACK, L'\u232B'},
    {1003, L"TrayKeyboard: Delete", VK_DELETE, L'\u2326'},
}};

UINT gTaskbarCreatedMessage = 0;
HWINEVENTHOOK gForegroundHook = nullptr;
HWINEVENTHOOK gFocusHook = nullptr;
std::array<HICON, kTrayActions.size()> gTrayIcons{};
InputTarget gLastInputTarget;

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

void RestoreInputTarget() {
    if (!IsValidTargetWindow(gLastInputTarget.topLevelWindow)) {
        return;
    }

    if (IsIconic(gLastInputTarget.topLevelWindow) != FALSE) {
        ShowWindow(gLastInputTarget.topLevelWindow, SW_RESTORE);
    }

    DWORD currentThreadId = GetCurrentThreadId();
    DWORD targetThreadId = GetWindowThreadProcessId(gLastInputTarget.topLevelWindow, nullptr);
    const bool attachInput = targetThreadId != 0 && targetThreadId != currentThreadId;

    if (attachInput) {
        AttachThreadInput(currentThreadId, targetThreadId, TRUE);
    }

    BringWindowToTop(gLastInputTarget.topLevelWindow);
    SetForegroundWindow(gLastInputTarget.topLevelWindow);
    SetActiveWindow(gLastInputTarget.topLevelWindow);

    if (IsValidTargetWindow(gLastInputTarget.focusWindow)) {
        SetFocus(gLastInputTarget.focusWindow);
    }

    if (attachInput) {
        AttachThreadInput(currentThreadId, targetThreadId, FALSE);
    }
}

bool SendKeyMessages(HWND windowHandle, UINT virtualKey) {
    if (!IsValidTargetWindow(windowHandle)) {
        return false;
    }

    const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    LPARAM keyDownFlags = 1 | (static_cast<LPARAM>(scanCode) << 16);
    LPARAM keyUpFlags = keyDownFlags | (1LL << 30) | (1LL << 31);

    if (virtualKey == VK_DELETE) {
        keyDownFlags |= 1LL << 24;
        keyUpFlags |= 1LL << 24;
    }

    DWORD_PTR ignoredResult = 0;
    if (SendMessageTimeoutW(windowHandle, WM_KEYDOWN, virtualKey, keyDownFlags, SMTO_ABORTIFHUNG, 100, &ignoredResult) == 0) {
        return false;
    }

    if (virtualKey == VK_RETURN || virtualKey == VK_BACK) {
        const WPARAM character = virtualKey == VK_RETURN ? L'\r' : L'\b';
        SendMessageTimeoutW(windowHandle, WM_CHAR, character, 1, SMTO_ABORTIFHUNG, 100, &ignoredResult);
    }

    SendMessageTimeoutW(windowHandle, WM_KEYUP, virtualKey, keyUpFlags, SMTO_ABORTIFHUNG, 100, &ignoredResult);
    return true;
}

void SendVirtualKey(UINT virtualKey) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = static_cast<WORD>(virtualKey);

    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

    if (virtualKey == VK_DELETE) {
        inputs[0].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        inputs[1].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }

    SendInput(static_cast<UINT>(std::size(inputs)), inputs, sizeof(INPUT));
}

void ExecuteTrayAction(UINT virtualKey) {
    if (SendKeyMessages(gLastInputTarget.focusWindow, virtualKey)) {
        return;
    }

    if (gLastInputTarget.focusWindow != gLastInputTarget.topLevelWindow &&
        SendKeyMessages(gLastInputTarget.topLevelWindow, virtualKey)) {
        return;
    }

    RestoreInputTarget();
    SendVirtualKey(virtualKey);
}

HICON CreateGlyphIcon(wchar_t glyph, COLORREF backgroundColor, COLORREF foregroundColor) {
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
    HBRUSH backgroundBrush = CreateSolidBrush(backgroundColor);
    FillRect(colorDc, &iconBounds, backgroundBrush);
    DeleteObject(backgroundBrush);
    PatBlt(maskDc, 0, 0, kIconSize, kIconSize, WHITENESS);

    SetBkMode(colorDc, TRANSPARENT);
    SetTextColor(colorDc, foregroundColor);

    LOGFONTW font{};
    font.lfHeight = -14;
    font.lfWeight = FW_BOLD;
    font.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(font.lfFaceName, L"Segoe UI Symbol", LF_FACESIZE);
    HFONT fontHandle = CreateFontIndirectW(&font);
    HGDIOBJ previousFont = fontHandle != nullptr ? SelectObject(colorDc, fontHandle) : nullptr;

    wchar_t glyphText[2]{glyph, L'\0'};
    DrawTextW(colorDc, glyphText, 1, &iconBounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = colorBitmap;
    iconInfo.hbmMask = maskBitmap;
    HICON iconHandle = CreateIconIndirect(&iconInfo);

    if (previousFont != nullptr) {
        SelectObject(colorDc, previousFont);
    }
    if (fontHandle != nullptr) {
        DeleteObject(fontHandle);
    }

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
        gTrayIcons[index] = CreateGlyphIcon(kTrayActions[index].glyph, kIconBackground, kIconForeground);
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

    if (command == kMenuExitCommand) {
        DestroyWindow(windowHandle);
        return;
    }

    if (const TrayAction* action = FindTrayAction(command)) {
        ExecuteTrayAction(action->virtualKey);
    }
}

LRESULT CALLBACK WindowProcedure(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == gTaskbarCreatedMessage) {
        AddTrayIcons(windowHandle);
        return 0;
    }

    switch (message) {
    case WM_CREATE:
        CreateTrayIcons();
        InstallWindowTrackingHooks();
        AddTrayIcons(windowHandle);
        return 0;

    case kTrayCallbackMessage: {
        const UINT trayId = static_cast<UINT>(wParam);
        switch (static_cast<UINT>(lParam)) {
        case WM_LBUTTONUP:
        case NIN_SELECT:
            if (const TrayAction* action = FindTrayAction(trayId)) {
                ExecuteTrayAction(action->virtualKey);
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
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(windowHandle, message, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instanceHandle, HINSTANCE, PWSTR, int) {
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

    return static_cast<int>(message.wParam);
}
