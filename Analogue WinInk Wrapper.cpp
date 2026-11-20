#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")

// --- CONFIGURATION ---
constexpr float DEADZONE = 0.05f;          // 5% threshold to start stroke
constexpr float PRESSURE_CURVE = 1.2f;     // >1.0 = softer light strokes, <1.0 = faster ramp
constexpr int POLL_INTERVAL_MS = 1;        // 1000Hz polling rate
constexpr int TOGGLE_HOTKEY_VK = VK_F8;    // Activation hotkey (default: F8)

// --- ACTIVATION MODES ---
enum class ActivationMode {
    TogglePress = 0,    // Press F8 to toggle ON/OFF
    HoldToActivate = 1, // Active ONLY while holding F8
    HoldToPause = 2     // Active ALWAYS except while holding F8
};

// --- MENU & TRAY CONSTANTS ---
constexpr UINT WM_TRAYICON = WM_USER + 1;
constexpr UINT ID_TRAY_ICON = 1001;
constexpr UINT ID_MENU_TOGGLE = 2001;
constexpr UINT ID_MENU_STARTUP = 2002;
constexpr UINT ID_MENU_EXIT = 2003;
constexpr UINT ID_MENU_MODE_FULL = 2010;
constexpr UINT ID_MENU_MODE_CENTER = 2011;
constexpr UINT ID_MENU_ACT_TOGGLE = 2020;
constexpr UINT ID_MENU_ACT_HOLD_ON = 2021;
constexpr UINT ID_MENU_ACT_HOLD_OFF = 2022;
constexpr UINT ID_MENU_AXIS_BASE = 2100;
constexpr UINT ID_MENU_JOY_BASE = 2200;
constexpr UINT_PTR TIMER_POLL_ID = 1;

// --- GLOBAL STATE ---
HSYNTHETICPOINTERDEVICE g_penDevice = nullptr;
NOTIFYICONDATAW g_nid = {};
ActivationMode g_actMode = ActivationMode::HoldToActivate;
bool g_toggleState = false;
bool g_isPenDown = false;
bool g_keyWasDown = false;
std::wstring g_lastTip;

UINT g_joyId = 1;
int g_axisIndex = 4;        // 0=X, 1=Y, 2=Z, 3=R, 4=U, 5=V
bool g_centerRest = false;

const wchar_t* AXIS_NAMES[] = { L"X Axis", L"Y Axis", L"Z Axis", L"R Axis (Rz)", L"U Axis (Rx)", L"V Axis (Ry)" };
const wchar_t* RUN_REG_PATH = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* APP_REG_NAME = L"Z3PenBridge";

// --- STARTUP REGISTRY HELPERS ---
bool IsStartupEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_REG_PATH, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    bool exists = (RegQueryValueExW(hKey, APP_REG_NAME, nullptr, &type, nullptr, &size) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return exists;
}

std::wstring GetFriendlyJoystickName(UINT joyId, const JOYCAPSW& caps) {
    // 1. Find the active VID/PID hardware string for this joyId
    std::wstring configPath = L"SYSTEM\\CurrentControlSet\\Control\\MediaResources\\Joystick\\";
    configPath += caps.szRegKey;
    configPath += L"\\CurrentJoystickSettings";

    std::wstring valueName = L"Joystick" + std::to_wstring(joyId + 1) + L"OEMName";
    wchar_t oemKeyName[256] = {};
    DWORD dataSize = sizeof(oemKeyName);

    if (RegGetValueW(HKEY_CURRENT_USER, configPath.c_str(), valueName.c_str(),
        RRF_RT_REG_SZ, nullptr, oemKeyName, &dataSize) != ERROR_SUCCESS) {
        // Fallback to HKLM if not overridden in HKCU
        dataSize = sizeof(oemKeyName);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, configPath.c_str(), valueName.c_str(),
            RRF_RT_REG_SZ, nullptr, oemKeyName, &dataSize) != ERROR_SUCCESS) {
            return caps.szPname; // Fallback to generic WinMM name
        }
    }

    // 2. Look up the human-readable OEMName under MediaProperties
    std::wstring oemPath = L"SYSTEM\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\";
    oemPath += oemKeyName;

    wchar_t friendlyName[256] = {};
    dataSize = sizeof(friendlyName);

    if (RegGetValueW(HKEY_CURRENT_USER, oemPath.c_str(), L"OEMName",
        RRF_RT_REG_SZ, nullptr, friendlyName, &dataSize) == ERROR_SUCCESS) {
        return friendlyName;
    }

    dataSize = sizeof(friendlyName);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, oemPath.c_str(), L"OEMName",
        RRF_RT_REG_SZ, nullptr, friendlyName, &dataSize) == ERROR_SUCCESS) {
        return friendlyName;
    }

    return caps.szPname;
}

void SetStartupEnabled(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_REG_PATH, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return;

    if (enable) {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        RegSetValueExW(hKey, APP_REG_NAME, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(exePath),
            static_cast<DWORD>((wcslen(exePath) + 1) * sizeof(wchar_t)));
    }
    else {
        RegDeleteValueW(hKey, APP_REG_NAME);
    }
    RegCloseKey(hKey);
}

void UpdateTrayTooltip(const std::wstring& text) {
    if (g_lastTip == text) return;
    g_lastTip = text;
    wcsncpy_s(g_nid.szTip, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

bool EnsurePenDevice() {
    if (!g_penDevice) {
        g_penDevice = CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_NONE);
    }
    return g_penDevice != nullptr;
}

void ReleasePenIfDown() {
    if (!g_isPenDown) return;

    if (g_penDevice) {
        POINT pt;
        GetCursorPos(&pt);

        POINTER_TYPE_INFO pti = {};
        pti.type = PT_PEN;
        pti.penInfo.pointerInfo.pointerType = PT_PEN;
        pti.penInfo.pointerInfo.pointerId = 0;
        pti.penInfo.pointerInfo.ptPixelLocation = pt;
        pti.penInfo.pointerInfo.pointerFlags = POINTER_FLAG_INRANGE | POINTER_FLAG_UP;
        pti.penInfo.penMask = PEN_MASK_PRESSURE;
        pti.penInfo.pressure = 0;

        InjectSyntheticPointerInput(g_penDevice, &pti, 1);

        // Cycle the device: destroy to drop the ~500ms mouse lock, 
        // then immediately recreate so Windows/CSP still detects a pen device.
        DestroySyntheticPointerDevice(g_penDevice);
        g_penDevice = CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_NONE);
    }

    g_isPenDown = false;
}

DWORD GetAxisRawValue(const JOYINFOEX& ji, int axisIdx) {
    switch (axisIdx) {
    case 0: return ji.dwXpos;
    case 1: return ji.dwYpos;
    case 2: return ji.dwZpos;
    case 3: return ji.dwRpos;
    case 4: return ji.dwUpos;
    case 5: return ji.dwVpos;
    default: return 0;
    }
}

bool EvaluateActiveState() {
    bool keyDown = (GetAsyncKeyState(TOGGLE_HOTKEY_VK) & 0x8000) != 0;

    switch (g_actMode) {
    case ActivationMode::TogglePress:
        if (keyDown && !g_keyWasDown) {
            g_toggleState = !g_toggleState;
        }
        g_keyWasDown = keyDown;
        return g_toggleState;

    case ActivationMode::HoldToActivate:
        g_keyWasDown = keyDown;
        return keyDown;

    case ActivationMode::HoldToPause:
        g_keyWasDown = keyDown;
        return !keyDown;
    }
    return false;
}

void PollAndInject() {
    bool isActive = EvaluateActiveState();

    if (!isActive) {
        ReleasePenIfDown();
        if (g_actMode == ActivationMode::HoldToActivate) {
            UpdateTrayTooltip(L"Z3 Pen Bridge (Hold F8 to Draw)");
        }
        else if (g_actMode == ActivationMode::HoldToPause) {
            UpdateTrayTooltip(L"Z3 Pen Bridge (PAUSED while holding F8)");
        }
        else {
            UpdateTrayTooltip(L"Z3 Pen Bridge (PAUSED - Press F8 to Resume)");
        }
        return;
    }

    JOYINFOEX ji = {};
    ji.dwSize = sizeof(JOYINFOEX);
    ji.dwFlags = JOY_RETURNALL;

    MMRESULT res = joyGetPosEx(g_joyId, &ji);
    DWORD rawVal = (res == JOYERR_NOERROR) ? GetAxisRawValue(ji, g_axisIndex) : 0;

    float normPressure = 0.0f;
    if (res == JOYERR_NOERROR) {
        if (g_centerRest) {
            normPressure = (static_cast<float>(rawVal) - 32767.5f) / 32767.5f;
        }
        else {
            normPressure = static_cast<float>(rawVal) / 65535.0f;
        }
        normPressure = std::clamp(normPressure, 0.0f, 1.0f);
    }

    BOOL injectOk = TRUE;
    DWORD lastErr = 0;
    UINT32 pressure = 0;

    if (normPressure > DEADZONE) {
        if (!EnsurePenDevice()) {
            lastErr = GetLastError();
            injectOk = FALSE;
        }
        else {
            POINT pt;
            GetCursorPos(&pt);

            float scaled = std::pow((normPressure - DEADZONE) / (1.0f - DEADZONE), PRESSURE_CURVE);
            pressure = std::clamp(static_cast<UINT32>(scaled * 1024.0f), 1u, 1024u);

            POINTER_TYPE_INFO pti = {};
            pti.type = PT_PEN;
            pti.penInfo.pointerInfo.pointerType = PT_PEN;
            pti.penInfo.pointerInfo.pointerId = 0;
            pti.penInfo.pointerInfo.ptPixelLocation = pt;
            pti.penInfo.penMask = PEN_MASK_PRESSURE;
            pti.penInfo.pressure = pressure;

            if (!g_isPenDown) {
                pti.penInfo.pointerInfo.pointerFlags =
                    POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT |
                    POINTER_FLAG_FIRSTBUTTON | POINTER_FLAG_DOWN;
                g_isPenDown = true;
            }
            else {
                pti.penInfo.pointerInfo.pointerFlags =
                    POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT |
                    POINTER_FLAG_FIRSTBUTTON | POINTER_FLAG_UPDATE;
            }
            injectOk = InjectSyntheticPointerInput(g_penDevice, &pti, 1);
            if (!injectOk) lastErr = GetLastError();
        }
    }
    else if (g_isPenDown) {
        ReleasePenIfDown();
    }
}

void ShowContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();

    if (g_actMode == ActivationMode::TogglePress) {
        AppendMenuW(hMenu, MF_STRING | (g_toggleState ? MF_CHECKED : MF_UNCHECKED), ID_MENU_TOGGLE, L"Active (F8)");
    }

    HMENU hActMenu = CreatePopupMenu();
    AppendMenuW(hActMenu, MF_STRING | (g_actMode == ActivationMode::TogglePress ? MF_CHECKED : MF_UNCHECKED),
        ID_MENU_ACT_TOGGLE, L"Toggle on Press (F8)");
    AppendMenuW(hActMenu, MF_STRING | (g_actMode == ActivationMode::HoldToActivate ? MF_CHECKED : MF_UNCHECKED),
        ID_MENU_ACT_HOLD_ON, L"Hold to Activate (Hold F8)");
    AppendMenuW(hActMenu, MF_STRING | (g_actMode == ActivationMode::HoldToPause ? MF_CHECKED : MF_UNCHECKED),
        ID_MENU_ACT_HOLD_OFF, L"Hold to Pause (Hold F8)");
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hActMenu), L"Activation Mode");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    HMENU hJoyMenu = CreatePopupMenu();
    UINT numDevs = joyGetNumDevs();
    bool foundAny = false;

    for (UINT i = 0; i < numDevs && i < 16; ++i) {
        JOYCAPSW caps = {};
        if (joyGetDevCapsW(i, &caps, sizeof(caps)) == JOYERR_NOERROR) {
            JOYINFOEX ji = {};
            ji.dwSize = sizeof(JOYINFOEX);
            ji.dwFlags = JOY_RETURNALL;
            if (joyGetPosEx(i, &ji) == JOYERR_NOERROR) {
                foundAny = true;
                std::wstring devName = GetFriendlyJoystickName(i, caps);
                std::wstring itemText = L"ID " + std::to_wstring(i) + L": " + devName;
                AppendMenuW(hJoyMenu, MF_STRING | (g_joyId == i ? MF_CHECKED : MF_UNCHECKED),
                    ID_MENU_JOY_BASE + i, itemText.c_str());
            }
        }
    }

    if (!foundAny) {
        AppendMenuW(hJoyMenu, MF_STRING | MF_GRAYED, 0, L"No active controllers detected");
    }
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hJoyMenu), L"Select Controller");

    HMENU hAxisMenu = CreatePopupMenu();
    for (int i = 0; i < 6; ++i) {
        AppendMenuW(hAxisMenu, MF_STRING | (g_axisIndex == i ? MF_CHECKED : MF_UNCHECKED), ID_MENU_AXIS_BASE + i, AXIS_NAMES[i]);
    }
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hAxisMenu), L"Select Pressure Axis");
    HMENU hRangeMenu = CreatePopupMenu();
    AppendMenuW(hRangeMenu, MF_STRING | (!g_centerRest ? MF_CHECKED : MF_UNCHECKED), ID_MENU_MODE_FULL, L"0 -> Max (Trigger / Slider)");
    AppendMenuW(hRangeMenu, MF_STRING | (g_centerRest ? MF_CHECKED : MF_UNCHECKED), ID_MENU_MODE_CENTER, L"Center -> Max (Half-Stick)");
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hRangeMenu), L"Axis Rest Position");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : MF_UNCHECKED), ID_MENU_STARTUP, L"Run on Windows Startup");
    AppendMenuW(hMenu, MF_STRING, ID_MENU_EXIT, L"Exit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);
}
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TIMER:
        if (wParam == TIMER_POLL_ID) PollAndInject();
        return 0;
    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP && g_actMode == ActivationMode::TogglePress) {
            g_toggleState = !g_toggleState;
            if (!g_toggleState) ReleasePenIfDown();
        }
        else if (lParam == WM_RBUTTONUP) {
            ShowContextMenu(hwnd);
        }
        return 0;
    case WM_COMMAND: {
        UINT id = LOWORD(wParam);
        if (id == ID_MENU_TOGGLE) {
            g_toggleState = !g_toggleState;
            if (!g_toggleState) ReleasePenIfDown();
        }
        else if (id == ID_MENU_ACT_TOGGLE) {
            ReleasePenIfDown();
            g_actMode = ActivationMode::TogglePress;
            g_toggleState = true;
        }
        else if (id == ID_MENU_ACT_HOLD_ON) {
            ReleasePenIfDown();
            g_actMode = ActivationMode::HoldToActivate;
        }
        else if (id == ID_MENU_ACT_HOLD_OFF) {
            ReleasePenIfDown();
            g_actMode = ActivationMode::HoldToPause;
        }
        else if (id == ID_MENU_STARTUP) {
            SetStartupEnabled(!IsStartupEnabled());
        }
        else if (id == ID_MENU_MODE_FULL) {
            ReleasePenIfDown();
            g_centerRest = false;
        }
        else if (id == ID_MENU_MODE_CENTER) {
            ReleasePenIfDown();
            g_centerRest = true;
        }
        else if (id >= ID_MENU_AXIS_BASE && id < ID_MENU_AXIS_BASE + 6) {
            ReleasePenIfDown();
            g_axisIndex = static_cast<int>(id - ID_MENU_AXIS_BASE);
        }
        else if (id >= ID_MENU_JOY_BASE && id < ID_MENU_JOY_BASE + 16) {
            ReleasePenIfDown();
            g_joyId = id - ID_MENU_JOY_BASE;
        }
        else if (id == ID_MENU_EXIT) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_POLL_ID);
        ReleasePenIfDown();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_penDevice) DestroySyntheticPointerDevice(g_penDevice);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Global\Z3PenBridgeSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    if (!EnsurePenDevice()) {
        MessageBoxW(nullptr, L"Failed to create synthetic Windows Ink pen device.", L"Z3 Pen Bridge", MB_ICONERROR);
        return 1;
    }
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"Z3PenBridgeHiddenWnd";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Z3PenBridge", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = ID_TRAY_ICON;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Z3 Pen Bridge");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    SetTimer(hwnd, TIMER_POLL_ID, POLL_INTERVAL_MS, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(hMutex);
    return 0;
}