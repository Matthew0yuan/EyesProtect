/*
 * EyesProtect - a tiny 20-20-20 eye-break reminder for Windows.
 *
 * After WorkMinutes of screen use, a small card appears in the bottom-right
 * corner asking you to look out at the trees. The countdown only runs while
 * your hands are off the keyboard and mouse; a chime plays when it is done.
 *
 * Why it is light
 *   - Plain Win32 in C, no C runtime, one thread that sleeps in GetMessage.
 *   - Activity comes from GetLastInputInfo (no input hooks), sampled by one
 *     coalescable timer (60 s). Lock/unlock and sleep/resume arrive as events,
 *     and the timer is stopped while you are away.
 *   - Normal priority on purpose: the widget is under the mouse and must answer
 *     at once; the thread sleeps in GetMessage, so this costs nothing.
 *
 * Optional EyesProtect.ini next to the exe (never created by the app):
 *   [EyesProtect]
 *   WorkMinutes=20     ; screen time before a break      (1-240)
 *   BreakSeconds=10    ; hands-off seconds per break     (3-300)
 *   IdleMinutes=5      ; no input this long = a break    (1-60)
 *
 * EyesProtect.exe /test   uses 30 s of work and a 15 s idle reset.
 * Build with build.cmd (MSVC).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <dwmapi.h>
#include <intrin.h>

/* ---- No C runtime: the compiler may still emit calls to these. ---- */

#pragma function(memset)
void *__cdecl memset(void *dst, int c, size_t n)
{
    __stosb((unsigned char *)dst, (unsigned char)c, n);
    return dst;
}

#pragma function(memcpy)
void *__cdecl memcpy(void *dst, const void *src, size_t n)
{
    __movsb((unsigned char *)dst, (const unsigned char *)src, n);
    return dst;
}

/* ---- Constants and state ---- */

#define APP_NAME      L"EyesProtect"
#define MAIN_CLASS    L"EyesProtectMain"
#define CARD_CLASS    L"EyesProtectCard"
#define RUN_KEY       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define WIDGET_CLASS  L"EyesProtectWidget"
#define APP_KEY       L"Software\\EyesProtect"
#define WM_TRAY       (WM_APP + 1)
#define PAUSE_MS      (60u * 60u * 1000u)
#define NO_POS        0x80000000u

enum { TIMER_TICK = 1, TIMER_PAUSE, TIMER_TRAY, TIMER_CARD };
enum { IDM_BREAK = 1, IDM_PAUSE, IDM_RESUME, IDM_WIDGET, IDM_AUTOSTART, IDM_EXIT };
enum { ST_ACTIVE, ST_BREAK, ST_PAUSED, ST_AWAY };

static struct { DWORD workMs, breakMs, idleMs, pollMs; } g_cfg;

static HINSTANCE g_inst;
static HWND      g_hwnd;                 /* hidden main window */
static int       g_state = ST_AWAY;
static BOOL      g_locked, g_asleep;
static ULONGLONG g_activeMs;             /* screen time since the last break */
static ULONGLONG g_lastTick;
static WORD      g_pauseH, g_pauseM;     /* local time the pause ends */
static UINT      g_msgTaskbarCreated, g_trayRetries;
static HICON     g_icoOn, g_icoOff;
static NOTIFYICONDATAW g_nid;

/* Card (exists only during a break) */
static HWND      g_card;
static HFONT     g_fTitle, g_fHint, g_fNum;
static RECT      g_rcSkip;
static ULONGLONG g_cardShown;
static DWORD     g_cardOffMs;            /* hands-off time so far */
static int       g_cardLeft;             /* seconds shown */
static BOOL      g_cardHandsOn, g_light;

/* Corner widget (always visible unless turned off) */
static HWND      g_widget;
static HFONT     g_fWidget, g_fWidgetUnit;
static HICON     g_icoWidget;
static BOOL      g_drag;                 /* widget is being dragged */
static POINT     g_dragOff;              /* cursor offset inside the widget */

static void CardShow(void);
static void CardClose(void);
static void WidgetUpdate(void);
static void WidgetSync(void);

/* ---- Small helpers ---- */

static int Scale(int v, UINT dpi) { return MulDiv(v, (int)dpi, 96); }

static UINT Clamp(UINT v, UINT lo, UINT hi) { return v < lo ? lo : v > hi ? hi : v; }

static void TrimMemory(void)
{
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

/* Milliseconds since the last keyboard/mouse/touch input, system-wide. */
static DWORD IdleMs(void)
{
    LASTINPUTINFO li;
    li.cbSize = sizeof li;
    return GetLastInputInfo(&li) ? GetTickCount() - li.dwTime : 0; /* wrap-safe */
}

static QUERY_USER_NOTIFICATION_STATE ShellState(void)
{
    QUERY_USER_NOTIFICATION_STATE s;
    return SUCCEEDED(SHQueryUserNotificationState(&s)) ? s : QUNS_ACCEPTS_NOTIFICATIONS;
}

/* The monitor a fullscreen app (video, game, slides) is covering, or NULL.
 * QUNS_BUSY is not used: it is also reported for an ordinary maximized window.
 * Instead the foreground window must cover its whole monitor, taskbar included.
 * A normal maximized window with a title bar does not count: with an auto-hide
 * taskbar it covers the whole monitor too. */
static HMONITOR FullscreenMonitor(void)
{
    HWND fg = GetForegroundWindow();
    HMONITOR mon;
    RECT r;
    MONITORINFO mi;
    WCHAR cls[32];

    /* Shell surfaces that cover the whole monitor but are not fullscreen apps:
     * the desktop, Alt+Tab / Task View, Start / Search / notification flyouts. */
    static const WCHAR *const shell[] = {
        L"Progman", L"WorkerW", L"XamlExplorerHostIslandWindow", L"MultitaskingViewFrame",
        L"Windows.UI.Core.CoreWindow", L"ForegroundStaging", L"Shell_TrayWnd",
    };
    int i;

    if (!fg || !GetWindowRect(fg, &r)) return NULL;
    if (GetClassNameW(fg, cls, 32))
        for (i = 0; i < (int)(sizeof shell / sizeof shell[0]); ++i)
            if (!lstrcmpW(cls, shell[i])) return NULL;
    if (IsZoomed(fg) && (GetWindowLongW(fg, GWL_STYLE) & WS_CAPTION) == WS_CAPTION)
        return NULL;
    mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, &mi)) return NULL;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom ? mon : NULL;
}

static BOOL IsExclusive(QUERY_USER_NOTIFICATION_STATE s)
{
    return s == QUNS_RUNNING_D3D_FULL_SCREEN || s == QUNS_PRESENTATION_MODE;
}

static BOOL IsFullscreen(QUERY_USER_NOTIFICATION_STATE s)
{
    return IsExclusive(s) || FullscreenMonitor() != NULL;
}

/* ---- Settings ---- */

static BOOL HasTestFlag(void)
{
    const WCHAR *s = GetCommandLineW();
    if (*s == L'"') {                       /* skip the program name */
        for (++s; *s && *s != L'"'; ++s) {}
        if (*s) ++s;
    } else {
        while (*s && *s != L' ' && *s != L'\t') ++s;
    }
    for (; *s; ++s) {
        const WCHAR *a = s, *b = L"/test";
        while (*b && (*a | 0x20) == *b) { ++a; ++b; }
        if (!*b) return TRUE;
    }
    return FALSE;
}

static void LoadConfig(void)
{
    WCHAR ini[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, ini, MAX_PATH);
    UINT work = 20, brk = 10, idle = 5;

    if (n > 4 && n < MAX_PATH) {
        lstrcpyW(ini + n - 3, L"ini");      /* EyesProtect.exe -> EyesProtect.ini */
        work = GetPrivateProfileIntW(APP_NAME, L"WorkMinutes", 20, ini);
        brk  = GetPrivateProfileIntW(APP_NAME, L"BreakSeconds", 10, ini);
        idle = GetPrivateProfileIntW(APP_NAME, L"IdleMinutes", 5, ini);
    }
    g_cfg.workMs  = Clamp(work, 1, 240) * 60000u;
    g_cfg.breakMs = Clamp(brk, 3, 300) * 1000u;
    g_cfg.idleMs  = Clamp(idle, 1, 60) * 60000u;
    if (HasTestFlag()) {
        g_cfg.workMs = 30000;
        g_cfg.idleMs = 15000;
    }
    g_cfg.pollMs = g_cfg.idleMs / 5 < 60000 ? g_cfg.idleMs / 5 : 60000;
}

/* ---- Tray icon: a small tree drawn into a 32-bit DIB ---- */

static HICON MakeTreeIcon(int size, BOOL gray)
{
    /* Shapes on a 32x32 grid: three canopy circles {x, y, r} and a trunk. */
    static const int canopy[3][3] = { { 16, 11, 9 }, { 10, 16, 7 }, { 22, 16, 7 } };
    static BYTE zeroMask[64 * 8];           /* all zero (BSS): alpha decides */
    COLORREF leaf = gray ? RGB(150, 150, 150) : RGB(67, 160, 71);
    COLORREF bark = gray ? RGB(110, 110, 110) : RGB(121, 85, 72);
    BITMAPINFO bi;
    ICONINFO ii;
    DWORD *px;
    HBITMAP color, mask;
    HICON ico;
    int x, y, i, j, k;

    size = size < 16 ? 16 : size > 64 ? 64 : size;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;          /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    color = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!color) return NULL;

    for (y = 0; y < size; ++y) {
        for (x = 0; x < size; ++x) {
            int nl = 0, nb = 0, n;
            for (j = 0; j < 4; ++j) {           /* 4x4 supersampling */
                for (i = 0; i < 4; ++i) {
                    /* v / size is the sample position on the 32-unit grid */
                    int vx = 4 * (8 * x + 2 * i + 1), vy = 4 * (8 * y + 2 * j + 1);
                    BOOL hit = FALSE;
                    for (k = 0; k < 3 && !hit; ++k) {
                        int dx = vx - canopy[k][0] * size, dy = vy - canopy[k][1] * size;
                        int r = canopy[k][2] * size;
                        hit = dx * dx + dy * dy <= r * r;
                    }
                    if (hit) ++nl;
                    else if (vx >= 14 * size && vx <= 18 * size && vy >= 18 * size && vy <= 30 * size) ++nb;
                }
            }
            n = nl + nb;
            if (!n) {
                px[y * size + x] = 0;
            } else {                            /* straight (non-premultiplied) alpha */
                DWORD r = (GetRValue(leaf) * nl + GetRValue(bark) * nb) / n;
                DWORD g = (GetGValue(leaf) * nl + GetGValue(bark) * nb) / n;
                DWORD b = (GetBValue(leaf) * nl + GetBValue(bark) * nb) / n;
                DWORD a = (DWORD)n * 255 / 16;
                px[y * size + x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }

    mask = CreateBitmap(size, size, 1, 1, zeroMask);
    memset(&ii, 0, sizeof ii);
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    ico = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return ico;
}

/* ---- Activity tracking ---- */

static void ScheduleTick(void)
{
    UINT ms = g_cfg.pollMs;
    if (g_activeMs < g_cfg.workMs && g_cfg.workMs - g_activeMs < ms)
        ms = (UINT)(g_cfg.workMs - g_activeMs);     /* land on the 20-minute mark */
    if (ms < 250) ms = 250;
    SetCoalescableTimer(g_hwnd, TIMER_TICK, ms, NULL, ms / 12);
}

static void TrayIcon(void);

static void StartActive(void)
{
    g_state = ST_ACTIVE;
    g_activeMs = 0;
    g_lastTick = GetTickCount64();
    ScheduleTick();
    TrayIcon();
}

static void OnTick(void)
{
    ULONGLONG now = GetTickCount64(), delta = now - g_lastTick;
    QUERY_USER_NOTIFICATION_STATE q;

    g_lastTick = now;
    WidgetSync();
    if (g_state == ST_PAUSED) {                 /* keep the widget in sync while paused */
        SetCoalescableTimer(g_hwnd, TIMER_TICK, g_cfg.pollMs, NULL, g_cfg.pollMs / 12);
        return;
    }
    if (g_state != ST_ACTIVE) {                 /* timers repeat: don't keep waking while away */
        KillTimer(g_hwnd, TIMER_TICK);
        return;
    }

    if (delta > 3ull * g_cfg.pollMs + 5000)
        g_activeMs = 0;                         /* missed ticks: the PC slept */
    else if (IdleMs() >= g_cfg.idleMs && !IsFullscreen(ShellState()))
        g_activeMs = 0;                         /* away from the keys: a natural break */
    else
        g_activeMs += delta;                    /* reading counts as screen time too */

    if (g_activeMs >= g_cfg.workMs) {
        q = ShellState();                       /* don't pop over exclusive games / slides */
        if (q != QUNS_RUNNING_D3D_FULL_SCREEN && q != QUNS_PRESENTATION_MODE) {
            CardShow();
            return;
        }
    }
    ScheduleTick();
}

/* Locked or asleep: stop everything. Back: start a fresh 20 minutes. */
static void SyncAway(void)
{
    if (g_locked || g_asleep) {
        if (g_state == ST_ACTIVE || g_state == ST_BREAK) {
            CardClose();
            KillTimer(g_hwnd, TIMER_TICK);
            g_state = ST_AWAY;
        }
    } else if (g_state == ST_AWAY) {
        StartActive();
    }
}

static void Pause(void)
{
    SYSTEMTIME st;
    CardClose();
    g_state = ST_PAUSED;
    /* The tick keeps running while paused, but only to keep the widget in sync. */
    SetCoalescableTimer(g_hwnd, TIMER_TICK, g_cfg.pollMs, NULL, g_cfg.pollMs / 12);
    GetLocalTime(&st);
    g_pauseH = (WORD)((st.wHour + 1) % 24);
    g_pauseM = st.wMinute;
    SetCoalescableTimer(g_hwnd, TIMER_PAUSE, PAUSE_MS, NULL, 30000);
    TrayIcon();
}

static void Resume(void)
{
    KillTimer(g_hwnd, TIMER_PAUSE);
    g_state = ST_AWAY;
    SyncAway();
    TrayIcon();
}

static void TakeBreakNow(void)
{
    if (g_state == ST_ACTIVE || g_state == ST_PAUSED) CardShow();
}

static void BreakDone(BOOL chime)
{
    CardClose();
    if (chime) MessageBeep(MB_ICONASTERISK);
    StartActive();
    TrimMemory();
}

/* ---- Tray ---- */

static void StatusText(WCHAR *buf)
{
    switch (g_state) {
    case ST_ACTIVE: {
        ULONGLONG used = g_activeMs + (GetTickCount64() - g_lastTick);
        ULONGLONG left = used < g_cfg.workMs ? g_cfg.workMs - used : 0;
        if (left >= 60000)
            wsprintfW(buf, L"Next eye break in %u min", (UINT)((left + 59999) / 60000));
        else
            wsprintfW(buf, L"Next eye break in %u sec", (UINT)((left + 999) / 1000));
        break;
    }
    case ST_BREAK:  lstrcpyW(buf, L"Eye break — look far away"); break;
    case ST_PAUSED: wsprintfW(buf, L"Paused until %02u:%02u", (UINT)g_pauseH, (UINT)g_pauseM); break;
    default:        lstrcpyW(buf, L"Away"); break;
    }
}

static void TrayAdd(void)
{
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_state == ST_PAUSED ? g_icoOff : g_icoOn;
    lstrcpyW(g_nid.szTip, APP_NAME);
    /* At logon Explorer may not be ready yet: retry a few times. */
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid) && !Shell_NotifyIconW(NIM_MODIFY, &g_nid) &&
        g_trayRetries++ < 20)
        SetTimer(g_hwnd, TIMER_TRAY, 3000, NULL);
}

/* Called on every state change: refresh the widget and the tray icon. */
static void TrayIcon(void)
{
    HICON want = g_state == ST_PAUSED ? g_icoOff : g_icoOn;
    WidgetSync();
    if (g_nid.hIcon == want) return;
    g_nid.hIcon = want;
    g_nid.uFlags = NIF_ICON;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

/* Called only when the mouse hovers the icon, so Explorer isn't pinged every tick. */
static void TrayTip(void)
{
    WCHAR s[128];
    lstrcpyW(s, APP_NAME L"\n");
    StatusText(s + lstrlenW(s));
    if (lstrcmpW(s, g_nid.szTip) == 0) return;
    lstrcpyW(g_nid.szTip, s);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static BOOL AutostartGet(void)
{
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, NULL, NULL) ==
           ERROR_SUCCESS;
}

static void AutostartSet(BOOL on)
{
    if (on) {
        WCHAR cmd[MAX_PATH + 3];
        DWORD n = GetModuleFileNameW(NULL, cmd + 1, MAX_PATH);
        if (!n || n >= MAX_PATH) return;
        cmd[0] = L'"';
        cmd[n + 1] = L'"';
        cmd[n + 2] = 0;
        RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, REG_SZ, cmd, (n + 3) * sizeof(WCHAR));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME);
    }
}

static DWORD RegReadDword(const WCHAR *name, DWORD def)
{
    DWORD v, cb = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, APP_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &cb) == ERROR_SUCCESS ? v : def;
}

static void RegWriteDword(const WCHAR *name, DWORD v)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, APP_KEY, name, REG_DWORD, &v, sizeof v);
}

static void WidgetCreate(void);
static void WidgetDestroy(void);

static void TrayMenu(void)
{
    HMENU m = CreatePopupMenu();
    WCHAR s[96];
    POINT pt;
    UINT cmd;

    StatusText(s);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, s);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_state == ST_BREAK ? MF_GRAYED : 0), IDM_BREAK, L"Take a break now");
    if (g_state == ST_PAUSED)
        AppendMenuW(m, MF_STRING, IDM_RESUME, L"Resume");
    else
        AppendMenuW(m, MF_STRING, IDM_PAUSE, L"Pause for 1 hour");
    AppendMenuW(m, MF_STRING | (g_widget ? MF_CHECKED : 0), IDM_WIDGET, L"Show widget");
    AppendMenuW(m, MF_STRING | (AutostartGet() ? MF_CHECKED : 0), IDM_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");

    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);                /* so the menu closes when clicking away */
    cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);

    switch (cmd) {
    case IDM_BREAK:     TakeBreakNow(); break;
    case IDM_PAUSE:     Pause(); break;
    case IDM_RESUME:    Resume(); break;
    case IDM_WIDGET:
        if (g_widget) WidgetDestroy(); else WidgetCreate();
        RegWriteDword(L"Widget", g_widget != NULL);
        break;
    case IDM_AUTOSTART: AutostartSet(!AutostartGet()); break;
    case IDM_EXIT:      DestroyWindow(g_hwnd); break;
    }
}

/* ---- Break card ---- */

static BOOL IsLightTheme(void)
{
    DWORD v = 1, cb = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &cb) != ERROR_SUCCESS)
        v = 1;
    return v != 0;
}

static HFONT MakeFont(UINT dpi, int px, const WCHAR *face)
{
    return CreateFontW(-Scale(px, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

/* The countdown is derived from input idle time; no separate counter needed. */
static void CardUpdate(void)
{
    DWORD idle = IdleMs(), off, rest, next;
    ULONGLONG since = GetTickCount64() - g_cardShown;

    off = since < idle ? (DWORD)since : idle;   /* hands-off time since the card appeared */
    if (off >= g_cfg.breakMs) {
        BreakDone(TRUE);
        return;
    }
    rest = g_cfg.breakMs - off;
    g_cardLeft = (int)((rest + 999) / 1000);
    g_cardOffMs = off;
    g_cardHandsOn = since >= 1000 && off < 1000;
    InvalidateRect(g_card, NULL, FALSE);

    next = rest % 1000;                         /* wake exactly at the next whole second */
    SetTimer(g_card, TIMER_CARD, (next ? next : 1000) + 15, NULL);
}

static void CardPaint(HDC dc, const RECT *rc)
{
    COLORREF bg  = g_light ? RGB(249, 249, 249) : RGB(32, 32, 32);
    COLORREF fg  = g_light ? RGB(26, 26, 26)    : RGB(255, 255, 255);
    COLORREF dim = g_light ? RGB(96, 96, 96)    : RGB(170, 170, 170);
    COLORREF acc = g_light ? RGB(15, 123, 15)   : RGB(108, 203, 95);
    COLORREF trk = g_light ? RGB(222, 222, 222) : RGB(64, 64, 64);
    HBRUSH br = (HBRUSH)GetStockObject(DC_BRUSH);
    UINT dpi = GetDpiForWindow(g_card);
    int pad = Scale(16, dpi), numW = Scale(80, dpi);
    WCHAR num[8];
    RECT r;

    SetDCBrushColor(dc, bg);
    FillRect(dc, rc, br);
    SetBkMode(dc, TRANSPARENT);

    /* big countdown number */
    wsprintfW(num, L"%d", g_cardLeft);
    SelectObject(dc, g_fNum);
    SetTextColor(dc, acc);
    SetRect(&r, rc->right - pad - numW, Scale(4, dpi), rc->right - pad, Scale(60, dpi));
    DrawTextW(dc, num, -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    /* title + hint */
    SelectObject(dc, g_fTitle);
    SetTextColor(dc, fg);
    SetRect(&r, pad, Scale(14, dpi), rc->right - pad - numW, Scale(38, dpi));
    DrawTextW(dc, L"Look out at the trees", -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    SelectObject(dc, g_fHint);
    SetTextColor(dc, dim);
    r.top = Scale(40, dpi);
    r.bottom = Scale(58, dpi);
    DrawTextW(dc, g_cardHandsOn ? L"Hands off the keyboard & mouse" : L"Look far away · relax your eyes",
              -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    /* Skip link */
    DrawTextW(dc, L"Skip", -1, &g_rcSkip, DT_SINGLELINE | DT_NOPREFIX);

    /* progress bar: fills while your hands stay off */
    {
        int h = Scale(3, dpi) > 2 ? Scale(3, dpi) : 2;
        int cy = (g_rcSkip.top + g_rcSkip.bottom) / 2;
        int x0 = pad, x1 = g_rcSkip.left - Scale(16, dpi);
        SetRect(&r, x0, cy - h / 2, x1, cy - h / 2 + h);
        SetDCBrushColor(dc, trk);
        FillRect(dc, &r, br);
        r.right = x0 + (int)((LONGLONG)(x1 - x0) * g_cardOffMs / g_cfg.breakMs);
        SetDCBrushColor(dc, acc);
        FillRect(dc, &r, br);
    }
}

static LRESULT CALLBACK CardProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_CARD) CardUpdate();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;                   /* never steal focus */
    case WM_SETCURSOR: {
        POINT pt;
        RECT r = g_rcSkip;
        GetCursorPos(&pt);
        ScreenToClient(h, &pt);
        InflateRect(&r, 8, 6);
        if (PtInRect(&r, pt)) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONUP: {
        POINT pt;
        RECT r = g_rcSkip;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        InflateRect(&r, 8, 6);
        if (PtInRect(&r, pt)) BreakDone(FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {                            /* double-buffered, no flicker */
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(h, &ps), mem = CreateCompatibleDC(dc);
        HBITMAP bmp;
        HGDIOBJ old;
        GetClientRect(h, &rc);
        bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        old = SelectObject(mem, bmp);
        CardPaint(mem, &rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        DeleteObject(g_fTitle);
        DeleteObject(g_fHint);
        DeleteObject(g_fNum);
        g_fTitle = g_fHint = g_fNum = NULL;
        g_card = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void CardShow(void)
{
    MONITORINFO mi;
    POINT pt;
    UINT dpi;
    int w, h, pad, margin;
    HDC dc;
    HGDIOBJ old;
    DWORD corner = DWMWCP_ROUND;
    BOOL dark;

    if (g_card) return;
    KillTimer(g_hwnd, TIMER_TICK);
    KillTimer(g_hwnd, TIMER_PAUSE);
    g_state = ST_BREAK;
    TrayIcon();

    /* Show it on the monitor where the mouse is. */
    GetCursorPos(&pt);
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    g_card = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, CARD_CLASS, APP_NAME,
                             WS_POPUP, mi.rcWork.left, mi.rcWork.top, 1, 1, NULL, NULL, g_inst, NULL);
    if (!g_card) {
        StartActive();
        return;
    }

    dpi = GetDpiForWindow(g_card);
    w = Scale(320, dpi);
    h = Scale(96, dpi);
    pad = Scale(16, dpi);
    margin = Scale(12, dpi);
    g_light = IsLightTheme();
    g_fTitle = MakeFont(dpi, 15, L"Segoe UI Semibold");
    g_fHint  = MakeFont(dpi, 12, L"Segoe UI");
    g_fNum   = MakeFont(dpi, 40, L"Segoe UI Light");

    /* Place the Skip link at the bottom-right. */
    dc = GetDC(g_card);
    old = SelectObject(dc, g_fHint);
    SetRectEmpty(&g_rcSkip);
    DrawTextW(dc, L"Skip", -1, &g_rcSkip, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(g_card, dc);
    OffsetRect(&g_rcSkip, w - pad - g_rcSkip.right, h - Scale(14, dpi) - g_rcSkip.bottom);

    dark = !g_light;
    DwmSetWindowAttribute(g_card, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
    DwmSetWindowAttribute(g_card, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);

    g_cardShown = GetTickCount64();
    g_cardLeft = (int)(g_cfg.breakMs / 1000);
    g_cardOffMs = 0;
    g_cardHandsOn = FALSE;
    SetWindowPos(g_card, HWND_TOPMOST, mi.rcWork.right - w - margin, mi.rcWork.bottom - h - margin, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    CardUpdate();
}

static void CardClose(void)
{
    if (g_card) DestroyWindow(g_card);
}

/* ---- Corner widget: tree + time left. Drag to move, right-click for the menu,
 *      double-click for a break now. Repainted only on state changes and ticks. ---- */

/* Style: pixel art. A tiny pixel tree, a hand-drawn 3x5 pixel font and a game-style
 * health bar that drains as the minutes pass. Everything is drawn on a small logical
 * canvas and blown up by a whole-number factor, so every pixel stays a crisp square.
 * Repainted only on ticks and state changes. */

#define PX_W 36                                 /* logical canvas, in "big pixels" */
#define PX_H 13

/* Size of one big pixel: a whole number of screen pixels, growing with the DPI. */
static int PixelUnit(UINT dpi)
{
    int u = (int)dpi / 48;
    return u < 2 ? 2 : u;
}

/* Window shape: a rectangle with each corner notched off by one big pixel. */
static void WidgetLayout(UINT dpi)
{
    int u = PixelUnit(dpi), w = PX_W * u, h = PX_H * u, i;
    const int cx[4] = { 0, w - u, 0, w - u }, cy[4] = { 0, 0, h - u, h - u };
    HRGN rgn;
    if (!g_widget) return;
    rgn = CreateRectRgn(0, 0, w, h);
    for (i = 0; i < 4; ++i) {
        HRGN cut = CreateRectRgn(cx[i], cy[i], cx[i] + u, cy[i] + u);
        CombineRgn(rgn, rgn, cut, RGN_DIFF);
        DeleteObject(cut);
    }
    SetWindowRgn(g_widget, rgn, TRUE);          /* the system owns rgn from here */
}

static void WidgetUpdate(void)
{
    if (g_widget) InvalidateRect(g_widget, NULL, FALSE);
}

/* Keep r (screen coords) inside the work area of its monitor, so the widget
 * never sits under the taskbar or off a monitor that was unplugged. */
static void ClampToWork(RECT *r)
{
    MONITORINFO mi;
    int dx = 0, dy = 0;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromRect(r, MONITOR_DEFAULTTONEAREST), &mi)) return;
    if (r->right > mi.rcWork.right) dx = mi.rcWork.right - r->right;
    if (r->left + dx < mi.rcWork.left) dx = mi.rcWork.left - r->left;
    if (r->bottom > mi.rcWork.bottom) dy = mi.rcWork.bottom - r->bottom;
    if (r->top + dy < mi.rcWork.top) dy = mi.rcWork.top - r->top;
    OffsetRect(r, dx, dy);
}

/* Hide over fullscreen apps; otherwise make sure it is shown, inside the work
 * area and still on top (the taskbar and other topmost windows can cover it).
 * Runs on every tick and state change, so it comes back even when paused. */
static void WidgetSync(void)
{
    RECT r, c;
    if (!g_widget) return;
    if (g_drag && GetCapture() != g_widget)     /* self-heal a drag that lost its capture */
        g_drag = FALSE;
    if (g_drag) {                               /* never move or hide it under the user's hand */
        WidgetUpdate();
        return;
    }
    /* Hide only when the fullscreen app is on the widget's own monitor. */
    if (IsExclusive(ShellState()) ||
        FullscreenMonitor() == MonitorFromWindow(g_widget, MONITOR_DEFAULTTONEAREST)) {
        if (IsWindowVisible(g_widget)) ShowWindow(g_widget, SW_HIDE);
        return;
    }
    GetWindowRect(g_widget, &r);
    c = r;
    ClampToWork(&c);
    SetWindowPos(g_widget, HWND_TOPMOST, c.left, c.top, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW |
                 (EqualRect(&r, &c) ? SWP_NOMOVE : 0));
    WidgetUpdate();
}

/* Retro 16-colour palette (0xRRGGBB, also the DIB pixel layout) */
#define P_BLACK   0x000000
#define P_NAVY    0x1D2B53
#define P_NAVY_HI 0x2E4178
#define P_NAVY_LO 0x111A33
#define P_SLOT_HI 0x2B3A66
#define P_SLOT_LO 0x162040
#define P_PLUM    0x7E2553
#define P_DGREEN  0x008751
#define P_BROWN   0xAB5236
#define P_RUST    0xD8580A
#define P_DGRAY   0x5F574F
#define P_LGRAY   0xC2C3C7
#define P_WHITE   0xFFF1E8
#define P_RED     0xFF004D
#define P_ORANGE  0xFFA300
#define P_YELLOW  0xFFEC27
#define P_GREEN   0x00E436
#define P_LIME    0xA8F060

/* 3x5 pixel font (M and N are wider). Each row is a bit pattern, leftmost pixel = high bit. */
typedef struct { char ch; BYTE w; BYTE row[5]; } Glyph;
static const Glyph g_font[] = {
    { '0', 3, { 7, 5, 5, 5, 7 } }, { '1', 3, { 2, 6, 2, 2, 7 } }, { '2', 3, { 7, 1, 7, 4, 7 } },
    { '3', 3, { 7, 1, 7, 1, 7 } }, { '4', 3, { 5, 5, 7, 1, 1 } }, { '5', 3, { 7, 4, 7, 1, 7 } },
    { '6', 3, { 7, 4, 7, 5, 7 } }, { '7', 3, { 7, 1, 1, 2, 2 } }, { '8', 3, { 7, 5, 7, 5, 7 } },
    { '9', 3, { 7, 5, 7, 1, 7 } }, { '<', 3, { 1, 2, 4, 2, 1 } }, { '-', 3, { 0, 0, 7, 0, 0 } },
    { 'M', 5, { 17, 27, 21, 17, 17 } }, { 'I', 3, { 7, 2, 2, 2, 7 } }, { 'N', 4, { 9, 13, 11, 9, 9 } },
    { 'R', 3, { 6, 5, 6, 5, 5 } }, { 'E', 3, { 7, 4, 6, 4, 7 } }, { 'S', 3, { 3, 4, 2, 1, 6 } },
    { 'T', 3, { 7, 2, 2, 2, 2 } }, { 'H', 3, { 5, 5, 7, 5, 5 } }, { 'O', 3, { 2, 5, 5, 5, 2 } },
    { 'L', 3, { 4, 4, 4, 4, 7 } }, { 'D', 3, { 6, 5, 5, 5, 6 } },
};

/* Tree sprite: L leaf, H highlight, D shade, T trunk. */
static const char *const g_tree[9] = {
    "..LLL..", ".LHLLL.", "LHLLLLL", "LLLLLLD", "LLLLLDD", ".LDDDD.", "...T...", "...T...", "..TTT..",
};

static DWORD g_cv[PX_H * PX_W];                 /* the logical canvas */

static void Px(int x, int y, DWORD c)
{
    if (x >= 0 && x < PX_W && y >= 0 && y < PX_H) g_cv[y * PX_W + x] = c;
}

static const Glyph *FindGlyph(char ch)
{
    int i;
    for (i = 0; i < (int)(sizeof g_font / sizeof g_font[0]); ++i)
        if (g_font[i].ch == ch) return &g_font[i];
    return NULL;
}

static int PxTextWidth(const char *s)
{
    int w = 0;
    for (; *s; ++s) {
        const Glyph *g = FindGlyph(*s);
        w += (g ? g->w : 3) + 1;
    }
    return w ? w - 1 : 0;
}

static void PxText(int x, int y, const char *s, DWORD c)
{
    int r, k;
    for (; *s; ++s) {
        const Glyph *g = FindGlyph(*s);
        if (g)
            for (r = 0; r < 5; ++r)
                for (k = 0; k < g->w; ++k)
                    if (g->row[r] >> (g->w - 1 - k) & 1) Px(x + k, y + r, c);
        x += (g ? g->w : 3) + 1;
    }
}

static void WidgetRender(HDC dc, int w, int h)
{
    const int barX0 = 11, barX1 = PX_W - 3, barLen = barX1 - barX0 + 1, textY = 2, barY = 9;
    DWORD leaf = P_GREEN, hi = P_LIME, shade = P_DGREEN, bark = P_BROWN;
    DWORD numCol = P_WHITE, barHi = P_GREEN, barLo = P_DGREEN;
    char num[8], unit[4];
    int fill = 0, x, y, nw;
    BITMAPINFO bi;

    unit[0] = 0;
    switch (g_state) {
    case ST_ACTIVE: {
        ULONGLONG used = g_activeMs + (GetTickCount64() - g_lastTick);
        ULONGLONG left = used < g_cfg.workMs ? g_cfg.workMs - used : 0;
        fill = (int)((left * barLen + g_cfg.workMs - 1) / g_cfg.workMs);
        if (left <= 3 * 60000) {                /* low HP: autumn leaves, red bar */
            leaf = P_ORANGE; hi = P_YELLOW; shade = P_RUST;
            numCol = P_YELLOW; barHi = P_RED; barLo = P_PLUM;
        }
        if (left >= 60000) wsprintfA(num, "%u", (UINT)((left + 59999) / 60000));
        else lstrcpyA(num, "<1");
        lstrcpyA(unit, "MIN");
        break;
    }
    case ST_BREAK:
        fill = barLen; numCol = P_GREEN;
        lstrcpyA(num, "REST");
        break;
    case ST_PAUSED:
        leaf = P_LGRAY; hi = P_WHITE; shade = P_DGRAY; bark = P_DGRAY; numCol = P_LGRAY;
        lstrcpyA(num, "HOLD");
        break;
    default:
        numCol = P_LGRAY;
        lstrcpyA(num, "--");
        break;
    }

    /* background with a bevelled frame (the notched corners are cut by the window region) */
    for (y = 0; y < PX_H; ++y)
        for (x = 0; x < PX_W; ++x) {
            DWORD c = P_NAVY;
            if (y == 0 || y == PX_H - 1 || x == 0 || x == PX_W - 1) c = P_BLACK;
            else if (y == 1 || x == 1) c = P_NAVY_HI;
            else if (y == PX_H - 2 || x == PX_W - 2) c = P_NAVY_LO;
            g_cv[y * PX_W + x] = c;
        }

    /* tree */
    for (y = 0; y < 9; ++y)
        for (x = 0; x < 7; ++x) {
            char t = g_tree[y][x];
            if (t != '.') Px(2 + x, 2 + y, t == 'L' ? leaf : t == 'H' ? hi : t == 'D' ? shade : bark);
        }

    /* time left, with the unit if it fits */
    PxText(barX0, textY, num, numCol);
    nw = PxTextWidth(num);
    if (unit[0] && barX0 + nw + 2 + PxTextWidth(unit) - 1 <= barX1)
        PxText(barX0 + nw + 2, textY, unit, P_LGRAY);

    /* health bar: two rows for a little depth */
    for (x = 0; x < barLen; ++x) {
        BOOL on = x < fill;
        Px(barX0 + x, barY, on ? barHi : P_SLOT_HI);
        Px(barX0 + x, barY + 1, on ? barLo : P_SLOT_LO);
    }

    /* blow it up with hard edges */
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = PX_W;
    bi.bmiHeader.biHeight = -PX_H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, w, h, 0, 0, PX_W, PX_H, g_cv, &bi, DIB_RGB_COLORS, SRCCOPY);
}

static void WidgetSize(UINT dpi, int *w, int *h)
{
    *w = PX_W * PixelUnit(dpi);
    *h = PX_H * PixelUnit(dpi);
}

/* The one way a drag ends. It clears the flag first and only then lets go of the
 * mouse, so it never depends on Windows sending WM_CAPTURECHANGED back. */
static void WidgetEndDrag(HWND h)
{
    RECT r;
    if (!g_drag) return;
    g_drag = FALSE;
    if (GetCapture() == h) ReleaseCapture();
    GetWindowRect(h, &r);
    RegWriteDword(L"WidgetX", (DWORD)r.left);
    RegWriteDword(L"WidgetY", (DWORD)r.top);
}

static LRESULT CALLBACK WidgetProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    /* Dragging is done here, not by returning HTCAPTION: that hands the mouse to
     * Windows' modal move loop, which holds capture until it sees a button-up and
     * could leave every click on screen swallowed. Our own capture only lives while
     * the button is down, and is dropped as soon as the button is seen released. */
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN: {
        RECT r;
        GetCursorPos(&g_dragOff);
        GetWindowRect(h, &r);
        g_dragOff.x -= r.left;
        g_dragOff.y -= r.top;
        SetCapture(h);
        g_drag = GetCapture() == h;             /* no capture, no drag: never a half-started one */
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_drag) {
            POINT pt;
            RECT r;
            if (!(wp & MK_LBUTTON) || GetCapture() != h) {  /* button already up / capture gone */
                WidgetEndDrag(h);
                return 0;
            }
            GetCursorPos(&pt);
            GetWindowRect(h, &r);
            OffsetRect(&r, pt.x - g_dragOff.x - r.left, pt.y - g_dragOff.y - r.top);
            ClampToWork(&r);                            /* can't be dropped onto the taskbar */
            SetWindowPos(h, NULL, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_LBUTTONUP:
        WidgetEndDrag(h);
        return 0;
    case WM_CAPTURECHANGED:                     /* capture taken away (Esc, another window, ...) */
        WidgetEndDrag(h);
        return 0;
    case WM_LBUTTONDBLCLK:
        TakeBreakNow();
        return 0;
    case WM_RBUTTONUP:
        WidgetEndDrag(h);
        TrayMenu();
        return 0;
    case WM_DISPLAYCHANGE:                      /* resolution / monitor layout changed */
        WidgetSync();
        return 0;
    case WM_DPICHANGED: {                       /* dragged to a monitor with another scale */
        const RECT *s = (const RECT *)lp;
        int w, hh;
        WidgetLayout(HIWORD(wp));
        WidgetSize(HIWORD(wp), &w, &hh);
        SetWindowPos(h, NULL, s->left, s->top, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_SETTINGCHANGE:                      /* light/dark theme switched */
        g_light = IsLightTheme();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(h, &ps);
        GetClientRect(h, &rc);
        WidgetRender(dc, rc.right, rc.bottom);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (g_fWidget) DeleteObject(g_fWidget);
        if (g_fWidgetUnit) DeleteObject(g_fWidgetUnit);
        g_fWidget = g_fWidgetUnit = NULL;
        g_widget = NULL;
        g_drag = FALSE;                         /* else a re-created widget would never sync */
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void WidgetCreate(void)
{
    POINT pt;
    MONITORINFO mi;
    DWORD corner = DWMWCP_DONOTROUND;
    UINT dpi;
    int w, h;

    if (g_widget) return;
    pt.x = (LONG)RegReadDword(L"WidgetX", NO_POS);
    pt.y = (LONG)RegReadDword(L"WidgetY", NO_POS);
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if ((DWORD)pt.x == NO_POS || !MonitorFromPoint(pt, MONITOR_DEFAULTTONULL)) {
        /* default: top-right of the primary monitor's work area */
        pt.x = pt.y = 0;
        GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
        pt.x = NO_POS;
    }
    g_widget = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WIDGET_CLASS, APP_NAME,
                               WS_POPUP, (DWORD)pt.x == NO_POS ? mi.rcWork.right - 1 : pt.x,
                               (DWORD)pt.x == NO_POS ? mi.rcWork.top : pt.y, 1, 1, NULL, NULL, g_inst, NULL);
    if (!g_widget) return;
    dpi = GetDpiForWindow(g_widget);
    WidgetLayout(dpi);
    WidgetSize(dpi, &w, &h);
    if ((DWORD)pt.x == NO_POS) {
        pt.x = mi.rcWork.right - w - Scale(12, dpi);
        pt.y = mi.rcWork.top + Scale(12, dpi);
    } else {                                    /* a saved spot may now be under the taskbar */
        RECT r;
        SetRect(&r, pt.x, pt.y, pt.x + w, pt.y + h);
        ClampToWork(&r);
        pt.x = r.left;
        pt.y = r.top;
    }
    /* Pixel art wants hard edges: no DWM rounding; the notched corners come from
     * the window region set in WidgetLayout. */
    DwmSetWindowAttribute(g_widget, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
    SetWindowPos(g_widget, HWND_TOPMOST, pt.x, pt.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void WidgetDestroy(void)
{
    if (g_widget) DestroyWindow(g_widget);
}

/* The foreground window changed: re-check fullscreen right away, so the widget
 * hides the moment a video goes fullscreen and comes back the moment it leaves,
 * instead of waiting up to a minute for the next tick. Out-of-context hook: the
 * call arrives through our own message loop, only on foreground switches. */
static void CALLBACK OnForeground(HWINEVENTHOOK hook, DWORD ev, HWND h, LONG obj, LONG child, DWORD thread,
                                  DWORD ms)
{
    UNREFERENCED_PARAMETER(hook);
    UNREFERENCED_PARAMETER(ev);
    UNREFERENCED_PARAMETER(h);
    UNREFERENCED_PARAMETER(obj);
    UNREFERENCED_PARAMETER(child);
    UNREFERENCED_PARAMETER(thread);
    UNREFERENCED_PARAMETER(ms);
    WidgetSync();
}

/* ---- Hidden main window: receives timers, session, power and tray messages ---- */

static LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_TICK) {
            OnTick();
        } else if (wp == TIMER_PAUSE) {
            if (g_state == ST_PAUSED) Resume();
            else KillTimer(h, TIMER_PAUSE);
        } else if (wp == TIMER_TRAY) {
            KillTimer(h, TIMER_TRAY);
            TrayAdd();
        }
        return 0;
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_MOUSEMOVE:     TrayTip(); break;
        case WM_LBUTTONDBLCLK: TakeBreakNow(); break;
        case WM_RBUTTONUP:     TrayMenu(); break;
        }
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) { g_locked = TRUE; SyncAway(); }
        else if (wp == WTS_SESSION_UNLOCK) { g_locked = FALSE; SyncAway(); }
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) { g_asleep = TRUE; SyncAway(); }
        else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) { g_asleep = FALSE; SyncAway(); }
        return TRUE;
    case WM_DESTROY:
        CardClose();
        WidgetDestroy();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        WTSUnRegisterSessionNotification(h);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_msgTaskbarCreated && msg) {    /* Explorer restarted */
        g_trayRetries = 0;
        TrayAdd();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---- Entry point (no C runtime) ---- */

void Start(void)
{
    PROCESS_POWER_THROTTLING_STATE eco;
    WNDCLASSW wc;
    MSG msg;
    int iconSize;

    CreateMutexW(NULL, FALSE, L"Local\\EyesProtect.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) ExitProcess(1);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    /* No efficiency mode and no lowered priority: the widget sits under the user's
     * mouse and must answer at once. At below-normal (or idle / EcoQoS) a busy CPU
     * starved this thread for seconds, so the widget froze and could not be dragged.
     * Normal priority costs nothing here: the thread sleeps in GetMessage and wakes
     * about once a minute. Opt out of automatic EcoQoS for background apps, too. */
    memset(&eco, 0, sizeof eco);
    eco.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    eco.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    eco.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &eco, sizeof eco);

    g_inst = GetModuleHandleW(NULL);
    LoadConfig();

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = MainProc;
    wc.hInstance = g_inst;
    wc.lpszClassName = MAIN_CLASS;
    RegisterClassW(&wc);
    wc.lpfnWndProc = CardProc;
    wc.style = CS_DROPSHADOW;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = CARD_CLASS;
    RegisterClassW(&wc);
    wc.lpfnWndProc = WidgetProc;
    wc.style = CS_DBLCLKS;
    wc.lpszClassName = WIDGET_CLASS;
    RegisterClassW(&wc);

    /* A hidden top-level window (not HWND_MESSAGE) so broadcasts reach it. */
    g_hwnd = CreateWindowExW(0, MAIN_CLASS, APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_inst, NULL);
    if (!g_hwnd) ExitProcess(2);

    iconSize = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    g_icoOn = MakeTreeIcon(iconSize, FALSE);
    g_icoOff = MakeTreeIcon(iconSize, TRUE);
    if (!g_icoOn) g_icoOn = LoadIconW(NULL, IDI_APPLICATION);
    if (!g_icoOff) g_icoOff = g_icoOn;

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WTSRegisterSessionNotification(g_hwnd, NOTIFY_FOR_THIS_SESSION);
    TrayAdd();
    if (RegReadDword(L"Widget", 1)) WidgetCreate();
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, OnForeground, 0, 0,
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    StartActive();
    TrimMemory();

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
        DispatchMessageW(&msg);
    ExitProcess(0);
}
