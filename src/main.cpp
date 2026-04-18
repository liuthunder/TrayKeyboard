#include <windows.h>
#include <shellapi.h>

#include <array>

namespace {

constexpr wchar_t kWindowClassName[] = L"TrayKeyboardWindowClass";
constexpr wchar_t kWindowTitle[] = L"TrayKeyboard";
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kMenuExitCommand = 2000;

struct InputTarget {
    HWND topLevelWindow = nullptr;
    HWND focusWindow = nullptr;
};

struct TrayAction {
    UINT id;
    const wchar_t* tooltip;
    UINT virtualKey;
    WORD iconId;
};

constexpr std::array<TrayAction, 3> kTrayActions{{
    {1001, L"TrayKeyboard: Enter", VK_RETURN, 32516},
    {1002, L"TrayKeyboard: Backspace", VK_BACK, 32515},
    {1003, L"TrayKeyboard: Delete", VK_DELETE, 32513},
}};

UINT gTaskbarCreatedMessage = 0;
InputTarget gLastInputTarget;

bool IsValidTargetWindow(HWND windowHandle) {
    return windowHandle != nullptr && IsWindow(windowHandle) != FALSE;
}

void CaptureInputTarget(HWND currentWindow) {
    HWND foregroundWindow = GetForegroundWindow();
    if (!IsValidTargetWindow(foregroundWindow) || foregroundWindow == currentWindow) {
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
    RestoreInputTarget();
    SendVirtualKey(virtualKey);
}

bool UpdateTrayIcon(HWND windowHandle, const TrayAction& action, DWORD message) {
    NOTIFYICONDATAW notifyIcon{};
    notifyIcon.cbSize = sizeof(notifyIcon);
    notifyIcon.hWnd = windowHandle;
    notifyIcon.uID = action.id;
    notifyIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    notifyIcon.uCallbackMessage = kTrayCallbackMessage;
    notifyIcon.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(action.iconId));
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
        AddTrayIcons(windowHandle);
        return 0;

    case kTrayCallbackMessage: {
        const UINT trayId = static_cast<UINT>(wParam);
        switch (static_cast<UINT>(lParam)) {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
            CaptureInputTarget(windowHandle);
            return 0;

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
