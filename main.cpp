#include "RetiUI.h"
#include <string>
#include <TlHelp32.h>  
#include <Psapi.h>     
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "windowsapp.lib")

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.System.h>

using namespace winrt;
using namespace Windows::Media::Control;

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_ICON 1000

#define WIDTH_RATIO_COLLAPSED  16
#define WIDTH_RATIO_EXPANDED   6
#define HEIGHT_COLLAPSED       17
#define MARGIN_TOP             10

#define GRID_GAP               8
#define TIME_HEIGHT            32
#define CLIP_ITEM_HEIGHT       34
#define BTN_HEIGHT             36
#define MAX_CLIP_TEXT          40
#define COLOR_PREVIEW_SIZE     16
#define PERF_HEIGHT            72
#define CARD_HEIGHT            60
#define HEIGHT_EXPANDED        (GRID_GAP + TIME_HEIGHT + GRID_GAP + CARD_HEIGHT + GRID_GAP + 2*BTN_HEIGHT + GRID_GAP + 3*CLIP_ITEM_HEIGHT + GRID_GAP + PERF_HEIGHT + GRID_GAP)

#define WINDOW_ALPHA           200
#define ANIMATION_DURATION     200
#define TIMER_ID               1
#define TIMER_INTERVAL         16
#define PERF_UPDATE_INTERVAL   3000  // 3秒更新一次性能

static int g_screenW = 0, g_widthCollapsed = 0, g_widthExpanded = 0;
static BOOL g_isExpanded = FALSE;
static WindowAnimation g_winAnim = { 0 };
static const ColorTheme* g_theme = &g_themeLight;
static HINSTANCE g_hInst = NULL;
static HWND g_hWnd = NULL;

static WCHAR g_timeStr[16], g_dateStr[32];
static WCHAR g_clipHistory[3][MAX_CLIP_TEXT + 1];
static BOOL g_clipValid[3] = { FALSE, FALSE, FALSE };
static int g_clipCount = 0;
static BOOL g_ignoreNextClip = FALSE;

// 按钮
static Button g_btnColorPicker;
static Button g_btnTheme;
static Button g_btnKeepAwake;  // 屏幕常亮
static Button g_btnSettings;   // 系统设置

static BOOL g_isColorPicking = FALSE;
static HHOOK g_mouseHook = NULL;
static BOOL g_isKeepAwake = FALSE;  // 常亮状态

// 性能监控数据
typedef struct {
    WCHAR name[16];
    SIZE_T memoryMB;
} TopProcess;

static TopProcess g_topProcs[3] = { 0 };
static int g_cpuUsage = 0;
static int g_memUsagePercent = 0;
static float g_memUsedGB = 0, g_memTotalGB = 0;
static DWORD g_lastPerfUpdate = 0;
static ULONGLONG g_lastIdleTime = 0, g_lastKernelTime = 0, g_lastUserTime = 0;

static GlobalSystemMediaTransportControlsSessionManager g_sessionManager = nullptr;
static event_token g_sessionAddedToken;
static event_token g_sessionRemovedToken;
static GlobalSystemMediaTransportControlsSession g_currentSession = nullptr;
static event_token g_mediaPropertiesChangedToken;
static event_token g_playbackInfoChangedToken;

static std::wstring g_musicTitle;
static std::wstring g_musicArtist;
static std::wstring g_musicAlbum;
static bool g_hasMusic = false;
static bool g_isPlaying = false;           // 当前是否正在播放
static float g_waveAmplitudes[4] = { 0.3f, 0.5f, 0.7f, 0.4f };
static float g_wavePhase = 0.0f;

void SetAutoStart() {
    /*自动注册自启动*/
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        WCHAR szPath[MAX_PATH];
        GetModuleFileNameW(NULL, szPath, MAX_PATH);
        RegSetValueExW(hKey, L"RetiIsland", 0, REG_SZ,
            (BYTE*)szPath, (DWORD)(wcslen(szPath) + 1) * sizeof(WCHAR));
        RegCloseKey(hKey);
    }
}

void InitSMTC() {
    /*初始化SMTC*/
    try {
        auto async = GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
        if (!async) {
            // 异步对象无效，SMTC 不可用
            return;
        }
        async.Completed([&](auto&& asyncInfo, auto&&) {
            try {
                g_sessionManager = asyncInfo.GetResults();
                if (!g_sessionManager) return;

                g_currentSession = g_sessionManager.GetCurrentSession();
                if (g_currentSession) {
                    auto props = g_currentSession.TryGetMediaPropertiesAsync().get();
                    if (props) {
                        g_musicTitle = props.Title();
                        g_musicArtist = props.Artist();
                        g_musicAlbum = props.AlbumTitle();
                        g_hasMusic = !g_musicTitle.empty();
                        InvalidateRect(g_hWnd, NULL, FALSE);
                    }
                    // 订阅媒体属性变化
                    g_mediaPropertiesChangedToken = g_currentSession.MediaPropertiesChanged([&](auto&&, auto&&) {
                        try {
                            auto props = g_currentSession.TryGetMediaPropertiesAsync().get();
                            if (props) {
                                g_musicTitle = props.Title();
                                g_musicArtist = props.Artist();
                                g_musicAlbum = props.AlbumTitle();
                                g_hasMusic = !g_musicTitle.empty();
                                InvalidateRect(g_hWnd, NULL, FALSE);
                            }
                        }
                        catch (...) {}
                        });
                    // 订阅播放信息变化
                    g_playbackInfoChangedToken = g_currentSession.PlaybackInfoChanged([&](auto&&, auto&&) {
                        try {
                            auto playbackInfo = g_currentSession.GetPlaybackInfo();
                            g_isPlaying = (playbackInfo.PlaybackStatus() == Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
                            InvalidateRect(g_hWnd, NULL, FALSE);
                        }
                        catch (...) {}
                        });
                }

                // 监听会话添加/移除
                g_sessionAddedToken = g_sessionManager.SessionsChanged([&](auto&&, auto&&) {
                    g_currentSession = g_sessionManager.GetCurrentSession();
                    if (g_currentSession) {
                        auto props = g_currentSession.TryGetMediaPropertiesAsync().get();
                        if (props) {
                            g_musicTitle = props.Title();
                            g_musicArtist = props.Artist();
                            g_musicAlbum = props.AlbumTitle();
                            g_hasMusic = !g_musicTitle.empty();
                        }
                        auto playbackInfo = g_currentSession.GetPlaybackInfo();
                        g_isPlaying = (playbackInfo.PlaybackStatus() == Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
                        InvalidateRect(g_hWnd, NULL, FALSE);
                    }
                    else {
                        g_hasMusic = false;
                        g_isPlaying = false;
                        InvalidateRect(g_hWnd, NULL, FALSE);
                    }
                    });
            }
            catch (...) {
                // 获取会话失败
                g_sessionManager = nullptr;
            }
            });
    }
    catch (...) {
        // RequestAsync 失败，SMTC 不可用
        g_sessionManager = nullptr;
    }
}

void CleanupSMTC() {
    /*释放SMTC*/
    if (g_currentSession) {
        try {
            if (g_mediaPropertiesChangedToken.value)
                g_currentSession.MediaPropertiesChanged(g_mediaPropertiesChangedToken);
            if (g_playbackInfoChangedToken.value)
                g_currentSession.PlaybackInfoChanged(g_playbackInfoChangedToken);
        }
        catch (...) {}
        g_currentSession = nullptr;
    }
    if (g_sessionManager) {
        try {
            if (g_sessionAddedToken.value)
                g_sessionManager.SessionsChanged(g_sessionAddedToken);
            if (g_sessionRemovedToken.value)
                g_sessionManager.SessionsChanged(g_sessionRemovedToken);
        }
        catch (...) {}
        g_sessionManager = nullptr;
    }
}

void DrawMusicCard(HDC hdc, int w, const ColorTheme* theme) {
    int cardY = GRID_GAP + TIME_HEIGHT + GRID_GAP;
    RECT cardRect = { GRID_GAP, cardY, w - GRID_GAP, cardY + CARD_HEIGHT };

    FillRoundRect(hdc, &cardRect, theme->bgToolHover, 12);

    if (!g_hasMusic) {
        HFONT font = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT oldFont = (HFONT)SelectObject(hdc, font);
        SetTextColor(hdc, theme->textNormal);
        SetBkMode(hdc, TRANSPARENT);
        RECT emptyRc = cardRect;
        DrawTextW(hdc, L"暂无播放", -1, &emptyRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(font);
        return;
    }

    // 1. 音频条（左侧）
    int barStartX = cardRect.left + 8;
    int barStartY = cardRect.top + (CARD_HEIGHT - 20) / 2;
    int barWidth = 4;
    int barSpacing = 4;
    for (int i = 0; i < 4; i++) {
        int barHeight = (int)(10 + g_waveAmplitudes[i] * 12);
        if (barHeight > 20) barHeight = 20;
        RECT barRc = { barStartX + i * (barWidth + barSpacing), barStartY + (20 - barHeight) / 2,
                       barStartX + i * (barWidth + barSpacing) + barWidth, barStartY + (20 + barHeight) / 2 };
        HBRUSH barBrush = CreateSolidBrush(theme->textNormal);
        FillRoundRect(hdc, &barRc, theme->textNormal, 2);
        DeleteObject(barBrush);
    }

    // 2. 标题和艺术家（中间）
    int textStartX = barStartX + 4 * (barWidth + barSpacing) + 8;
    int textEndX = cardRect.right - 48; // 为按钮留出空间
    HFONT titleFont = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT artistFont = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    HFONT oldFont = (HFONT)SelectObject(hdc, titleFont);
    SetTextColor(hdc, theme->textNormal);
    SetBkMode(hdc, TRANSPARENT);

    RECT titleRc = { textStartX, cardRect.top + 8, textEndX, cardRect.bottom - 24 };
    DrawTextW(hdc, g_musicTitle.c_str(), -1, &titleRc, DT_LEFT | DT_TOP | DT_WORD_ELLIPSIS);

    SelectObject(hdc, artistFont);
    RECT artistRc = { textStartX, cardRect.bottom - 20, textEndX, cardRect.bottom - 4 };
    DrawTextW(hdc, g_musicArtist.c_str(), -1, &artistRc, DT_LEFT | DT_VCENTER | DT_WORD_ELLIPSIS);

    SelectObject(hdc, oldFont);
    DeleteObject(titleFont);
    DeleteObject(artistFont);

    // 3. 播放/暂停按钮（右侧）
    int btnSize = 32;
    int btnX = cardRect.right - btnSize - 8;
    int btnY = cardRect.top + (CARD_HEIGHT - btnSize) / 2;
    RECT btnRect = { btnX, btnY, btnX + btnSize, btnY + btnSize };
    FillRoundRect(hdc, &btnRect, theme->bgIdle, btnSize / 2);
    const WCHAR* playPauseIcon = g_isPlaying ? L"⏸" : L"▶";
    HFONT iconFont = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI Emoji");
    SelectObject(hdc, iconFont);
    SetTextColor(hdc, theme->textNormal);
    RECT iconRc = btnRect;
    DrawTextW(hdc, playPauseIcon, -1, &iconRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(iconFont);
}

void InitClipboardHistory() {
    ZeroMemory(g_clipHistory, sizeof(g_clipHistory));
    g_clipCount = 0;
    for (int i = 0; i < 3; i++) g_clipValid[i] = FALSE;
}

void AddToClipboardHistory(LPCWSTR text) {
    if (!text || wcslen(text) == 0) return;
    if (g_clipValid[0] && wcscmp(g_clipHistory[0], text) == 0) return;

    for (int i = 2; i > 0; i--) {
        if (g_clipValid[i - 1]) {
            wcscpy_s(g_clipHistory[i], g_clipHistory[i - 1]);
            g_clipValid[i] = TRUE;
        }
    }

    wcsncpy_s(g_clipHistory[0], text, MAX_CLIP_TEXT);
    g_clipHistory[0][MAX_CLIP_TEXT] = L'\0';
    g_clipValid[0] = TRUE;
    if (g_clipCount < 3) g_clipCount++;
}

void DeleteClipItem(int index) {
    if (index < 0 || index >= 3 || !g_clipValid[index]) return;

    for (int i = index; i < 2; i++) {
        if (g_clipValid[i + 1]) {
            wcscpy_s(g_clipHistory[i], g_clipHistory[i + 1]);
            g_clipValid[i] = TRUE;
        }
        else {
            g_clipValid[i] = FALSE;
        }
    }
    g_clipValid[2] = FALSE;
    if (g_clipCount > 0) g_clipCount--;
}

void CopyClipItemToSystem(int index) {
    if (!g_clipValid[index]) return;

    g_ignoreNextClip = TRUE;
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        size_t len = (wcslen(g_clipHistory[index]) + 1) * sizeof(WCHAR);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
        if (hMem) {
            memcpy(GlobalLock(hMem), g_clipHistory[index], len);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
        CloseClipboard();
    }
}

// ==================== HEX颜色检测 ====================
BOOL IsHexColor(LPCWSTR str, COLORREF* outColor) {
    if (!str) return FALSE;

    int len = wcslen(str);
    if (len != 6 && len != 7) return FALSE;

    int start = 0;
    if (str[0] == L'#') {
        start = 1;
        if (len != 7) return FALSE;
    }
    else if (len != 6) {
        return FALSE;
    }

    for (int i = start; i < start + 6; i++) {
        wchar_t c = str[i];
        if (!((c >= L'0' && c <= L'9') ||
            (c >= L'A' && c <= L'F') ||
            (c >= L'a' && c <= L'f'))) {
            return FALSE;
        }
    }

    unsigned int r, g, b;
    swscanf_s(str + start, L"%2x%2x%2x", &r, &g, &b);
    if (outColor) *outColor = RGB(r, g, b);
    return TRUE;
}

BOOL IsSystemDarkMode() {
    HKEY hKey;
    DWORD value = 1;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        DWORD size = sizeof(value);
        RegQueryValueExW(hKey, L"SystemUsesLightTheme", NULL, NULL, (LPBYTE)&value, &size);
        RegCloseKey(hKey);
    }
    return value == 0;
}

void ToggleSystemTheme() {
    BOOL isDark = IsSystemDarkMode();
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        DWORD value = isDark ? 1 : 0;
        RegSetValueExW(hKey, L"SystemUsesLightTheme", 0, REG_DWORD, (BYTE*)&value, sizeof(value));
        RegCloseKey(hKey);
    }
    SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet");
}

void ApplySystemTheme() {
    g_theme = IsSystemDarkMode() ? &g_themeDark : &g_themeLight;
}

void UpdateDateTime() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf_s(g_timeStr, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    const WCHAR* weekdays[] = { L"日", L"一", L"二", L"三", L"四", L"五", L"六" };
    swprintf_s(g_dateStr, L"%04d-%02d-%02d 星期%s", st.wYear, st.wMonth, st.wDay, weekdays[st.wDayOfWeek]);
}

// 性能监控
void UpdateCPUsage() {
    FILETIME idleFt, kernelFt, userFt;
    if (!GetSystemTimes(&idleFt, &kernelFt, &userFt)) return;

    ULONGLONG idle = ((ULONGLONG)idleFt.dwHighDateTime << 32) | idleFt.dwLowDateTime;
    ULONGLONG kernel = ((ULONGLONG)kernelFt.dwHighDateTime << 32) | kernelFt.dwLowDateTime;
    ULONGLONG user = ((ULONGLONG)userFt.dwHighDateTime << 32) | userFt.dwLowDateTime;

    if (g_lastIdleTime == 0) {
        g_lastIdleTime = idle;
        g_lastKernelTime = kernel;
        g_lastUserTime = user;
        return;
    }

    ULONGLONG idleDiff = idle - g_lastIdleTime;
    ULONGLONG kernelDiff = kernel - g_lastKernelTime;
    ULONGLONG userDiff = user - g_lastUserTime;
    ULONGLONG totalDiff = kernelDiff + userDiff;

    if (totalDiff > 0) {
        g_cpuUsage = (int)((1.0 - ((double)idleDiff / (double)totalDiff)) * 100.0);
        if (g_cpuUsage < 0) g_cpuUsage = 0;
        if (g_cpuUsage > 100) g_cpuUsage = 100;
    }

    g_lastIdleTime = idle;
    g_lastKernelTime = kernel;
    g_lastUserTime = user;
}

void UpdatePerformanceData() {
    // 内存
    MEMORYSTATUSEX memStatus = { sizeof(MEMORYSTATUSEX) };
    if (GlobalMemoryStatusEx(&memStatus)) {
        g_memUsagePercent = (int)memStatus.dwMemoryLoad;
        g_memTotalGB = (float)(memStatus.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
        g_memUsedGB = g_memTotalGB * (g_memUsagePercent / 100.0f);
    }

    UpdateCPUsage();

    // 获取Top3进程
    typedef struct {
        WCHAR name[16];
        SIZE_T memory;
    } ProcInfo;

    ProcInfo procs[50] = { 0 };
    int procCount = 0;

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe32 = { sizeof(PROCESSENTRY32W) };
    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            if (procCount >= 50) break;

            HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe32.th32ProcessID);
            if (hProcess) {
                PROCESS_MEMORY_COUNTERS pmc = { sizeof(PROCESS_MEMORY_COUNTERS) };
                if (GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc))) {
                    SIZE_T memMB = pmc.WorkingSetSize / (1024 * 1024);
                    if (memMB > 20) { 
                        wcsncpy_s(procs[procCount].name, 16, pe32.szExeFile, _TRUNCATE);
                        WCHAR* dot = wcschr(procs[procCount].name, L'.');
                        if (dot) *dot = L'\0';
                        procs[procCount].memory = memMB;
                        procCount++;
                    }
                }
                CloseHandle(hProcess);
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);

    for (int i = 0; i < procCount - 1; i++) {
        for (int j = 0; j < procCount - i - 1; j++) {
            if (procs[j].memory < procs[j + 1].memory) {
                ProcInfo tmp = procs[j];
                procs[j] = procs[j + 1];
                procs[j + 1] = tmp;
            }
        }
    }

    for (int i = 0; i < 3; i++) {
        if (i < procCount) {
            wcscpy_s(g_topProcs[i].name, procs[i].name);
            g_topProcs[i].memoryMB = procs[i].memory;
        }
        else {
            g_topProcs[i].name[0] = L'\0';
            g_topProcs[i].memoryMB = 0;
        }
    }
}

// 取色功能
void PickColorAtPoint(int x, int y) {
    HDC hScreenDC = GetDC(NULL);
    COLORREF color = GetPixel(hScreenDC, x, y);
    ReleaseDC(NULL, hScreenDC);

    int r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
    WCHAR hexStr[10];
    swprintf_s(hexStr, L"#%02X%02X%02X", r, g, b);

    g_ignoreNextClip = TRUE;
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        size_t len = (wcslen(hexStr) + 1) * sizeof(WCHAR);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
        if (hMem) {
            memcpy(GlobalLock(hMem), hexStr, len);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
        CloseClipboard();
    }

    AddToClipboardHistory(hexStr);
    MessageBeep(MB_OK);
}

LRESULT CALLBACK GlobalMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && g_isColorPicking) {
        MSLLHOOKSTRUCT* info = (MSLLHOOKSTRUCT*)lParam;

        if (wParam == WM_LBUTTONDOWN) {
            PickColorAtPoint(info->pt.x, info->pt.y);

            g_isColorPicking = FALSE;
            if (g_mouseHook) {
                UnhookWindowsHookEx(g_mouseHook);
                g_mouseHook = NULL;
            }
            SetCursor(LoadCursor(NULL, IDC_ARROW));

            ShowWindow(g_hWnd, SW_SHOWNORMAL);
            SetWindowPos(g_hWnd, HWND_TOPMOST,
                (g_screenW - g_widthExpanded) / 2, MARGIN_TOP,
                g_widthExpanded, HEIGHT_EXPANDED,
                SWP_NOACTIVATE);
            InvalidateRect(g_hWnd, NULL, FALSE);

            return 1;
        }
        else if (wParam == WM_RBUTTONDOWN) {
            g_isColorPicking = FALSE;
            if (g_mouseHook) {
                UnhookWindowsHookEx(g_mouseHook);
                g_mouseHook = NULL;
            }
            SetCursor(LoadCursor(NULL, IDC_ARROW));

            ShowWindow(g_hWnd, SW_SHOWNORMAL);
            SetWindowPos(g_hWnd, HWND_TOPMOST,
                (g_screenW - (g_isExpanded ? g_widthExpanded : g_widthCollapsed)) / 2, MARGIN_TOP,
                g_isExpanded ? g_widthExpanded : g_widthCollapsed,
                g_isExpanded ? HEIGHT_EXPANDED : HEIGHT_COLLAPSED,
                SWP_NOACTIVATE);
            InvalidateRect(g_hWnd, NULL, FALSE);

            return 1;
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

void StartColorPicking() {
    if (g_isColorPicking) return;

    g_isColorPicking = TRUE;
    ShowWindow(g_hWnd, SW_HIDE);
    g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, GlobalMouseProc, g_hInst, 0);

    if (!g_mouseHook) {
        g_isColorPicking = FALSE;
        ShowWindow(g_hWnd, SW_SHOWNORMAL);
        return;
    }

    SetCursor(LoadCursor(NULL, IDC_CROSS));
}

// 窗口过程
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_CREATE: {
        g_hWnd = hWnd;
        g_hInst = ((LPCREATESTRUCT)lParam)->hInstance;
        ApplySystemTheme();
        g_screenW = GetSystemMetrics(SM_CXSCREEN);
        g_widthCollapsed = g_screenW / WIDTH_RATIO_COLLAPSED;
        g_widthExpanded = g_screenW / WIDTH_RATIO_EXPANDED;

        InitClipboardHistory();
        AddClipboardFormatListener(hWnd);
        UpdateDateTime();

        // 初始性能数据
        UpdatePerformanceData();
        g_lastPerfUpdate = GetTickCount();

        int x = (g_screenW - g_widthCollapsed) / 2;
        SetWindowPos(hWnd, HWND_TOPMOST, x, MARGIN_TOP,
            g_widthCollapsed, HEIGHT_COLLAPSED, SWP_SHOWWINDOW);

        int cardBottom = GRID_GAP + TIME_HEIGHT + GRID_GAP + CARD_HEIGHT + GRID_GAP;
        int row1Y = cardBottom;
        int row2Y = row1Y + BTN_HEIGHT + GRID_GAP / 2;

        int btnW = (g_widthExpanded - 3 * GRID_GAP) / 2;
        BtnInit(&g_btnColorPicker, GRID_GAP, row1Y, btnW, BTN_HEIGHT, L"🧪取色", FALSE, NULL);
        BtnInit(&g_btnTheme, 2 * GRID_GAP + btnW, row1Y, btnW, BTN_HEIGHT,
            IsSystemDarkMode() ? L"☀️明亮" : L"🌙暗黑", FALSE, NULL);
        BtnInit(&g_btnKeepAwake, GRID_GAP, row2Y, btnW, BTN_HEIGHT, L"☕常亮", TRUE, NULL);
        BtnInit(&g_btnSettings, 2 * GRID_GAP + btnW, row2Y, btnW, BTN_HEIGHT, L"⚙️设置", FALSE, NULL);

        SetTimer(hWnd, TIMER_ID, TIMER_INTERVAL, NULL);

        NOTIFYICONDATA nid = { sizeof(nid) };
        nid.hWnd = hWnd;
        nid.uID = ID_TRAY_ICON;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        wcscpy_s(nid.szTip, L"DynamicIsland-Windows的灵动岛");
        Shell_NotifyIcon(NIM_ADD, &nid);

        InitSMTC();

        SetAutoStart();
        return 0;
    }

    case WM_CLIPBOARDUPDATE: {
        if (g_ignoreNextClip) {
            g_ignoreNextClip = FALSE;
            return 0;
        }

        if (!OpenClipboard(hWnd)) return 0;

        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            HANDLE hData = GetClipboardData(CF_UNICODETEXT);
            if (hData) {
                WCHAR* pText = (WCHAR*)GlobalLock(hData);
                if (pText && wcslen(pText) < 1000) {
                    AddToClipboardHistory(pText);
                    if (!g_isExpanded && !g_winAnim.isActive && !g_isColorPicking) {
                        WinAnimStart(&g_winAnim, hWnd, TRUE,
                            g_widthExpanded, HEIGHT_EXPANDED,
                            g_widthCollapsed, HEIGHT_COLLAPSED);
                    }
                    InvalidateRect(hWnd, NULL, FALSE);
                }
                GlobalUnlock(hData);
            }
        }
        CloseClipboard();
        return 0;
    }

    case WM_TIMER: {
        if (wParam != TIMER_ID) break;

        static DWORD lastSecond = 0;
        DWORD now = GetTickCount();
        if (now - lastSecond >= 1000) {
            lastSecond = now;
            UpdateDateTime();
            if (g_isExpanded) InvalidateRect(hWnd, NULL, FALSE);
        }

        // 性能数据更新
        if (now - g_lastPerfUpdate >= PERF_UPDATE_INTERVAL) {
            UpdatePerformanceData();
            g_lastPerfUpdate = now;
            if (g_isExpanded) InvalidateRect(hWnd, NULL, FALSE);
        }

        // 音频条动画
        if (g_hasMusic && g_isExpanded) {
            static DWORD lastWaveUpdate = 0;
            if (now - lastWaveUpdate >= 50) {
                lastWaveUpdate = now;
                g_wavePhase += 0.2f;
                for (int i = 0; i < 4; i++) {
                    g_waveAmplitudes[i] = 0.3f + 0.4f * (0.5f + 0.5f * sinf(g_wavePhase + i * 0.8f));
                }
                InvalidateRect(hWnd, NULL, FALSE);
            }
        }

        // 鼠标悬停检测
        if (!g_isColorPicking) {
            POINT pt;
            GetCursorPos(&pt);
            RECT winRect;
            GetWindowRect(hWnd, &winRect);

            RECT detectRect = winRect;
            if (!g_isExpanded && !g_winAnim.isActive) detectRect.bottom += 25;
            else if (g_winAnim.isActive && !g_winAnim.isExpanding) detectRect.bottom += 15;

            BOOL mouseIn = PtInRect(&detectRect, pt);

            if (mouseIn && !g_isExpanded && !g_winAnim.isActive) {
                WinAnimStart(&g_winAnim, hWnd, TRUE, g_widthExpanded, HEIGHT_EXPANDED,
                    g_widthCollapsed, HEIGHT_COLLAPSED);
            }
            else if (!mouseIn && g_isExpanded && !g_winAnim.isActive) {
                WinAnimStart(&g_winAnim, hWnd, FALSE, g_widthExpanded, HEIGHT_EXPANDED,
                    g_widthCollapsed, HEIGHT_COLLAPSED);
            }
        }

        // 动画更新
        BOOL animating = WinAnimUpdate(&g_winAnim, hWnd, g_screenW, MARGIN_TOP, ANIMATION_DURATION);
        if (!animating && !g_winAnim.isActive && g_winAnim.startTime != 0) {
            g_isExpanded = g_winAnim.isExpanding;
            int finalW = g_isExpanded ? g_widthExpanded : g_widthCollapsed;
            int finalH = g_isExpanded ? HEIGHT_EXPANDED : HEIGHT_COLLAPSED;
            int finalX = (g_screenW - finalW) / 2;
            SetWindowPos(hWnd, HWND_TOPMOST, finalX, MARGIN_TOP, finalW, finalH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        if (animating) InvalidateRect(hWnd, NULL, FALSE);

        // 按钮动画更新
        if (g_isExpanded || g_winAnim.isActive) {
            float old1 = g_btnColorPicker.animHover;
            float old2 = g_btnTheme.animHover;
            float old3 = g_btnKeepAwake.animHover;
            float old4 = g_btnSettings.animHover;

            BtnUpdate(&g_btnColorPicker);
            BtnUpdate(&g_btnTheme);
            BtnUpdate(&g_btnKeepAwake);
            BtnUpdate(&g_btnSettings);

            // 同步主题按钮文字
            BOOL isDark = IsSystemDarkMode();
            const WCHAR* expectedText = isDark ? L"☀️明亮" : L"🌙暗黑";
            if (wcscmp(g_btnTheme.text, expectedText) != 0) {
                wcsncpy_s(g_btnTheme.text, MAX_BTN_TEXT, expectedText, _TRUNCATE);
                InvalidateRect(hWnd, NULL, FALSE);
            }

            // 同步常亮按钮状态
            if (g_btnKeepAwake.isActive != g_isKeepAwake) {
                g_btnKeepAwake.isActive = g_isKeepAwake;
                InvalidateRect(hWnd, NULL, FALSE);
            }

            if (fabs(old1 - g_btnColorPicker.animHover) > 0.001f ||
                fabs(old2 - g_btnTheme.animHover) > 0.001f ||
                fabs(old3 - g_btnKeepAwake.animHover) > 0.001f ||
                fabs(old4 - g_btnSettings.animHover) > 0.001f) {
                InvalidateRect(hWnd, NULL, FALSE);
            }
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdcScreen = BeginPaint(hWnd, &ps);

        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        int w = rcClient.right, h = rcClient.bottom;

        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        HBITMAP hbmMem = CreateCompatibleBitmap(hdcScreen, w, h);
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

        HBRUSH brush = CreateSolidBrush(g_theme->bgIdle);
        FillRect(hdcMem, &rcClient, brush);
        DeleteObject(brush);

        BOOL shouldDraw = g_winAnim.isActive ? g_winAnim.isExpanding : g_isExpanded;

        if (shouldDraw) {
            int halfW = w / 2;

            // 时间日期
            HFONT timeFont = CreateFontW(20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT oldFont = (HFONT)SelectObject(hdcMem, timeFont);
            SetTextColor(hdcMem, g_theme->textNormal);
            SetBkMode(hdcMem, TRANSPARENT);

            RECT timeRc = { GRID_GAP, GRID_GAP, halfW - GRID_GAP / 2, GRID_GAP + TIME_HEIGHT };
            DrawTextW(hdcMem, g_timeStr, -1, &timeRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            HFONT dateFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            SelectObject(hdcMem, dateFont);
            RECT dateRc = { halfW + GRID_GAP / 2, GRID_GAP, w - GRID_GAP, GRID_GAP + TIME_HEIGHT };
            DrawTextW(hdcMem, g_dateStr, -1, &dateRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            DeleteObject(dateFont);
            DeleteObject(timeFont);

            DrawMusicCard(hdcMem, w, g_theme);

            // 按钮
            BtnDraw(&g_btnColorPicker, hdcMem, g_theme);
            BtnDraw(&g_btnTheme, hdcMem, g_theme);
            BtnDraw(&g_btnKeepAwake, hdcMem, g_theme);
            BtnDraw(&g_btnSettings, hdcMem, g_theme);

            // 剪贴板历史
            int cardBottom = GRID_GAP + TIME_HEIGHT + GRID_GAP + CARD_HEIGHT + GRID_GAP;
            int clipStartY = cardBottom + 2 * BTN_HEIGHT + GRID_GAP;
            HFONT clipFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Consolas");
            SelectObject(hdcMem, clipFont);

            for (int i = 0; i < 3; i++) {
                if (!g_clipValid[i]) continue;

                int itemY = clipStartY + i * CLIP_ITEM_HEIGHT;

                COLORREF colorValue;
                BOOL isColor = IsHexColor(g_clipHistory[i], &colorValue);

                if (isColor) {
                    int boxX = GRID_GAP + 4;
                    int boxY = itemY + (CLIP_ITEM_HEIGHT - COLOR_PREVIEW_SIZE) / 2;

                    HBRUSH colorBrush = CreateSolidBrush(colorValue);
                    HPEN borderPen = CreatePen(PS_SOLID, 1, (g_theme == &g_themeDark) ? RGB(100, 100, 100) : RGB(200, 200, 200));
                    HBRUSH oldBr = (HBRUSH)SelectObject(hdcMem, colorBrush);
                    HPEN oldPen = (HPEN)SelectObject(hdcMem, borderPen);

                    RoundRect(hdcMem, boxX, boxY, boxX + COLOR_PREVIEW_SIZE, boxY + COLOR_PREVIEW_SIZE, 4, 4);

                    SelectObject(hdcMem, oldBr);
                    SelectObject(hdcMem, oldPen);
                    DeleteObject(colorBrush);
                    DeleteObject(borderPen);
                }

                SetTextColor(hdcMem, g_theme->textNormal);
                int textOffset = isColor ? (COLOR_PREVIEW_SIZE + 8) : 4;
                RECT textRc = { GRID_GAP + textOffset, itemY, w - 40, itemY + CLIP_ITEM_HEIGHT };
                DrawTextW(hdcMem, g_clipHistory[i], -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                // 删除按钮
                int cx = w - 20 - GRID_GAP;
                int cy = itemY + CLIP_ITEM_HEIGHT / 2;
                int r = 10;

                COLORREF xBg = ColorLerp(g_theme->bgToolHover, g_theme->bgIdle, 0.5f);
                HBRUSH xBrush = CreateSolidBrush(xBg);
                HPEN xPen = CreatePen(PS_SOLID, 1, xBg);
                HBRUSH oldBr = (HBRUSH)SelectObject(hdcMem, xBrush);
                HPEN oldPen = (HPEN)SelectObject(hdcMem, xPen);
                Ellipse(hdcMem, cx - r, cy - r, cx + r, cy + r);
                SelectObject(hdcMem, oldBr);
                SelectObject(hdcMem, oldPen);
                DeleteObject(xBrush);
                DeleteObject(xPen);

                SetTextColor(hdcMem, g_theme->textNormal);
                HFONT xFont = CreateFontW(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                SelectObject(hdcMem, xFont);
                RECT xRc = { cx - r, itemY, cx + r, itemY + CLIP_ITEM_HEIGHT };
                DrawTextW(hdcMem, L"×", -1, &xRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                DeleteObject(xFont);
            }
            DeleteObject(clipFont);

            //性能监控绘制 
            int perfY = clipStartY + 3 * CLIP_ITEM_HEIGHT + GRID_GAP;

            // 分隔线
            HPEN linePen = CreatePen(PS_SOLID, 1, (g_theme == &g_themeDark) ? RGB(60, 60, 60) : RGB(220, 220, 220));
            HPEN oldPen = (HPEN)SelectObject(hdcMem, linePen);
            MoveToEx(hdcMem, GRID_GAP, perfY - GRID_GAP / 2, NULL);
            LineTo(hdcMem, w - GRID_GAP, perfY - GRID_GAP / 2);
            SelectObject(hdcMem, oldPen);
            DeleteObject(linePen);

            HFONT perfFont = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Consolas");
            SelectObject(hdcMem, perfFont);
            SetTextColor(hdcMem, g_theme->textNormal);

            // 总体占用
            WCHAR line1[64];
            swprintf_s(line1, L"CPU:%2d%%  Mem:%2d%% (%.1f/%.0fGB)",
                g_cpuUsage, g_memUsagePercent, g_memUsedGB, g_memTotalGB);
            RECT rc1 = { GRID_GAP, perfY, w - GRID_GAP, perfY + 20 };
            DrawTextW(hdcMem, line1, -1, &rc1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // 进程
            int lineHeight = 18;
            for (int i = 0; i < 3; i++) {
                if (g_topProcs[i].name[0] == L'\0') continue;
                WCHAR line[32];
                swprintf_s(line, L"%d.%s %dMB", i + 1, g_topProcs[i].name, (int)g_topProcs[i].memoryMB);

                int col = i % 2;
                int row = i / 2;
                int xPos = GRID_GAP + col * (w / 2 - GRID_GAP);
                int yPos = perfY + 20 + row * lineHeight;

                RECT rc = { xPos, yPos, xPos + (w / 2 - GRID_GAP * 2), yPos + lineHeight };
                DrawTextW(hdcMem, line, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }

            DeleteObject(perfFont);
        }

        BitBlt(hdcScreen, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, hbmOld);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (!g_isExpanded) return DefWindowProc(hWnd, msg, wParam, lParam);

        POINT pt = { LOWORD(lParam), HIWORD(lParam) };

        // 按钮点击检测
        if (BtnHitTest(&g_btnColorPicker, pt.x, pt.y)) {
            StartColorPicking();
            return 0;
        }

        if (BtnHitTest(&g_btnTheme, pt.x, pt.y)) {
            ToggleSystemTheme();
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }

        if (BtnHitTest(&g_btnKeepAwake, pt.x, pt.y)) {
            g_isKeepAwake = !g_isKeepAwake;
            if (g_isKeepAwake) {
                SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
            }
            else {
                SetThreadExecutionState(ES_CONTINUOUS);
            }
            g_btnKeepAwake.isActive = g_isKeepAwake;
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }

        if (BtnHitTest(&g_btnSettings, pt.x, pt.y)) {
            ShellExecuteW(NULL, L"open", L"ms-settings:", NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }

        // 检测播放/暂停按钮
        if (g_isExpanded && g_hasMusic) {
            int cardY = GRID_GAP + TIME_HEIGHT + GRID_GAP;
            RECT cardRect = { GRID_GAP, cardY, g_widthExpanded - GRID_GAP, cardY + CARD_HEIGHT };
            int btnSize = 32;
            int btnX = cardRect.right - btnSize - 8;
            int btnY = cardRect.top + (CARD_HEIGHT - btnSize) / 2;
            RECT btnRect = { btnX, btnY, btnX + btnSize, btnY + btnSize };
            if (PtInRect(&btnRect, pt)) {
                if (g_currentSession) {
                    if (g_isPlaying) {
                        g_currentSession.TryPauseAsync();
                    }
                    else {
                        g_currentSession.TryPlayAsync();
                    }
                }
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
        }

        // 剪贴板点击检测
        int cardBottom = GRID_GAP + TIME_HEIGHT + GRID_GAP + CARD_HEIGHT + GRID_GAP;
        int clipStartY = cardBottom + 2 * BTN_HEIGHT + GRID_GAP;
        for (int i = 0; i < 3; i++) {
            if (!g_clipValid[i]) continue;

            int itemY = clipStartY + i * CLIP_ITEM_HEIGHT;
            RECT itemRc = { GRID_GAP, itemY, g_widthExpanded - GRID_GAP, itemY + CLIP_ITEM_HEIGHT };

            if (PtInRect(&itemRc, pt)) {
                if (pt.x > g_widthExpanded - 40) {
                    DeleteClipItem(i);
                }
                else {
                    CopyClipItemToSystem(i);
                    MessageBeep(MB_OK);
                }
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
        }

        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    case WM_MOUSEMOVE:
        if (g_isColorPicking) return 0;

        if (g_isExpanded) {
            POINT pt = { LOWORD(lParam), HIWORD(lParam) };
            BtnHandleMouse(&g_btnColorPicker, msg, pt.x, pt.y);
            BtnHandleMouse(&g_btnTheme, msg, pt.x, pt.y);
            BtnHandleMouse(&g_btnKeepAwake, msg, pt.x, pt.y);
            BtnHandleMouse(&g_btnSettings, msg, pt.x, pt.y);
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);

    case WM_LBUTTONUP:
        if (g_isExpanded && !g_isColorPicking) {
            POINT pt = { LOWORD(lParam), HIWORD(lParam) };
            if (BtnHandleMouse(&g_btnColorPicker, msg, pt.x, pt.y) ||
                BtnHandleMouse(&g_btnTheme, msg, pt.x, pt.y) ||
                BtnHandleMouse(&g_btnKeepAwake, msg, pt.x, pt.y) ||
                BtnHandleMouse(&g_btnSettings, msg, pt.x, pt.y)) {
                InvalidateRect(hWnd, NULL, FALSE);
            }
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);

    case WM_SETTINGCHANGE:
        ApplySystemTheme();
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;

    case WM_DESTROY: {
        KillTimer(hWnd, TIMER_ID);
        RemoveClipboardFormatListener(hWnd);
        if (g_isColorPicking && g_mouseHook) {
            UnhookWindowsHookEx(g_mouseHook);
            g_mouseHook = NULL;
        }
        // 恢复电源状态
        if (g_isKeepAwake) {
            SetThreadExecutionState(ES_CONTINUOUS);
        }

        NOTIFYICONDATA nid = { sizeof(nid) };
        nid.hWnd = hWnd;
        nid.uID = ID_TRAY_ICON;
        Shell_NotifyIcon(NIM_DELETE, &nid);
        
        CleanupSMTC();
        PostQuitMessage(0);
        return 0;
    }

    case WM_TRAYICON: {
        if (lParam == WM_LBUTTONUP || lParam == WM_RBUTTONUP) {
            if (IsWindowVisible(hWnd)) {
                ShowWindow(hWnd, SW_HIDE);
            }
            else {
                ShowWindow(hWnd, SW_SHOWNORMAL);
                SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            }
        }
        return 0;
    }
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    /*主函数*/
    g_hInst = hInst;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"RetiIsland";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"RetiIsland", L"RetiIsland", WS_POPUP,
        0, 0, 100, HEIGHT_COLLAPSED,
        NULL, NULL, hInst, NULL);

    if (!hwnd) return 1;

    SetLayeredWindowAttributes(hwnd, 0, WINDOW_ALPHA, LWA_ALPHA);
    DWM_WINDOW_CORNER_PREFERENCE round = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &round, sizeof(round));

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}