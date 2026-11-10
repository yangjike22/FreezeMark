// FreezeMark v1.15 - freeze screen & annotate (zhiyi)
// v1.15: the real cause of the width mismatch, pinpointed by the user:
//        below 10 both states agree; at two digits the COLLAPSED ball
//        breaks. The ball's label rect was ~74px wide (still reserving
//        room for the stroke preview removed in v1.6) and Segoe Print
//        is wide, so "10 px" and up overflowed and got center-clipped
//        into a wrong-looking value. Fix: reclaim the full width for
//        the label and auto-fit the font size by measuring the string
//        in DrawWidthLabel, stepping down until it fits whole.
// v1.14: "expanded vs collapsed can show different widths" - both
//        surfaces always read the same live g_lineWidth, so a mismatch
//        can only be a stale frame surviving a reveal. Fixes: one
//        shared DrawWidthLabel for both surfaces, and a synchronous
//        repaint (InvalidateRect + UpdateWindow) at every settle point
//        (fade-in finalize, morph finalize, fade-in start, middle-
//        button force-show), with the settled width logged.
// v1.13: middle-press color change now force-shows the capsule
//        synchronously in the message handler (v1.12's route through
//        the fade-in machinery did not bring it back on the real
//        machine): cancel fades, restore opacity 255, drop layered,
//        ShowWindow + topmost, repaint - all before the handler
//        returns. The whole middle-press path is logged (arrival,
//        prior state, resulting visibility) for diagnosis.
// v1.12: pressing middle to change color while the capsule is faded out
//        (mid-drag) now fades it straight back in, so the new color is
//        always observable on the capsule itself; plus a reshow guard
//        if the capsule is ever hidden outside the fade state machine.
// v1.11: middle mouse button cycles the line color through the palette
//        (red -> orange -> ... -> white -> red), on the canvas and on
//        the capsule (forwarded); a mid-drag press recolors the preview.
// v1.10: "capsule occasionally turns square after several marks" - the
//        1.2 s watchdog ran while the user was mid-drag, saw the capsule
//        hidden BY DESIGN (fade), mistook it for the old vanishing
//        fault and stripped the window region (stage B). Watchdog now
//        defers while dragging/fading (bounded retries), stage C is
//        likewise guarded, and every fade-in finalize re-verifies the
//        region so any loss self-heals within one marking cycle.
// v1.9: content crossfade during the morph (user's idea): the ball
//       content (dot + "N px") and the panel content are rendered on
//       separate offscreen layers and alpha-blended by eased progress
//       g_tbMorphE - ball fades out over the first 60%, panel fades in
//       over the last 60%; body radius interpolates alongside. Collapse
//       runs the same crossfade in reverse.
// v1.8: user reported the fades stutter badly. Two causes fixed: (1) all
//       animations moved off WM_TIMER (starved by mouse/paint traffic)
//       onto worker threads stepping every 5 ms; (2) the freeze paint
//       rebuilt a full-screen bitmap per mouse move - now it paints
//       only the update region and drags invalidate only the changed
//       rect. Behavior change per request: starting a mark retracts the
//       panel to the ball, and after the mark the capsule fades back in
//       AS THE BALL (no panel restore).
// v1.7: capsule visibility is now tied to the mouse button, as requested:
//       left button down on the canvas -> capsule fades out (110 ms);
//       button released -> it fades back in (190 ms), restoring the
//       panel if it was open before the draw. Layered alpha is enabled
//       only during a fade and removed at full opacity. Morph: region
//       rebuild throttled, 300 ms. (This also supersedes the "capsule
//       gone after first mark" report: it now hides BY DESIGN while
//       drawing and always returns on release.)
// v1.6: the user's real-machine photo of v1.5 showed the panel IS visible
//       (topmost re-assert + watchdog era), with three geometry faults:
//       (1) mini-zone "N px" wrapped into two lines -> NoWrap + smaller
//       font + wider zone; (2) window region (full semicircle) clipped
//       the painted borders/horizontal shadow -> ONE radius source
//       (ToolbarRegionEllipse: panel 36 / ball h-6) shared by region,
//       morph and paint, and the painted body is inset 2px from the
//       region edge; (3) diagnostic chip removed (user request). Also:
//       collapsed capsule no longer shows a stroke preview (dot + number
//       only), and the morph is smoother: 260 ms cubic-bezier
//       (0.3,1.25,0.45,1) with 1 ms timer resolution (timeBeginPeriod).
// v1.4: (a) instance takeover - a new build asks the old resident process
//       to exit (and after 600 ms ends it by name) instead of being
//       silently blocked by it; (b) toolbar region read-back check - an
//       empty region makes a window fully invisible, so verify and drop
//       it if empty; (c) wheel during left-drag now repaints the preview
//       immediately (real-time width change) + sub-notch accumulation.
// v1.3: ball redesigned after sketchbook-ui (paper capsule, ink double
//       border, hard shadow, Segoe Print) + beUI Dynamic Island (pill
//       morphs ball<->panel, 170ms easeOutCubic). Vanishing-ball fix:
//       dropped WS_EX_LAYERED + elliptic region (new in v1.2, prime
//       suspect), back to a plain opaque window + round-rect region;
//       WM_DESTROY logging + 1.2s visibility watchdog with re-show.
// v1.1: autostart (UseStats scheme, default on, off remembered) +
//       tray-resident low-footprint idle (blocking message loop).
// v1.2: floating window = glass oval ball (only "N px" + stroke preview),
//       click expands an icon-only glass panel; no instruction text on
//       screen. Own app icon (resource 101): glass-blue tile, coral
//       annotation frame, snowflake.
// Trigger: Enter + Backspace together (low-level keyboard hook)
// Frozen mode: left-drag draws a shape, wheel changes line width,
//              right-click exits. Floating toolbar: undo / clear /
//              shape (rect, rounded rect, ellipse) / color / solid-dash.
// Offline by construction: no network DLLs, data = one debug log beside exe.
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <objidl.h>
#include <propidl.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#include <cmath>
#include <gdiplus.h>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

static const UINT WM_TOGGLE_FREEZE = WM_APP + 1;
static const UINT WM_TRAYICON      = WM_APP + 2;
static const UINT WM_EXIT_FREEZE   = WM_APP + 3;
static const UINT WM_TAKEOVER_EXIT = WM_APP + 30;
static const int  TRAY_ID          = 1;
static const int  MENU_FREEZE = 1001;
static const int  MENU_EXIT   = 1002;
static const int  MENU_AUTOSTART = 1003;
#define IDI_APPICON 101

// ---------- log ----------
static wchar_t g_exeDir[MAX_PATH] = L"";
static void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\freezemark_debug.log", g_exeDir);
    FILE* f = _wfopen(path, L"a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, buf);
    fclose(f);
}

// ---------- autostart (same scheme as UseStats) ----------
// Truth: Run value under HKCU\...\Run\FreezeMark = quoted exe path.
// Preference: HKCU\Software\FreezeMark DWORD AutoStart (missing=never set,
// 1=on, 0=off). Missing -> default ON. Off is remembered: never resurrected.
// Moving the exe -> next launch refreshes the Run target to the new path.
static bool g_autoStart = false;

static void LogReg(const char* what, LONG rc) {
    if (rc == ERROR_SUCCESS) Log("autostart: %s ok", what);
    else Log("autostart: %s FAILED rc=%ld", what, rc);
}
static std::wstring ExePath() {
    wchar_t p[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}
static std::wstring QuotedExePath() { return L"\"" + ExePath() + L"\""; }

static int ReadAutoStartPref() {  // -1 missing, 0 off, 1 on
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\FreezeMark", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return -1;
    DWORD val = 0, type = 0, size = sizeof(val);
    LONG rc = RegQueryValueExW(k, L"AutoStart", nullptr, &type, (BYTE*)&val, &size);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) return -1;
    return val ? 1 : 0;
}
static void WriteAutoStartPref(bool on) {
    HKEY k = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\FreezeMark", 0, nullptr,
                              0, KEY_WRITE, nullptr, &k, nullptr);
    if (rc != ERROR_SUCCESS) { LogReg("pref key create", rc); return; }
    DWORD val = on ? 1 : 0;
    rc = RegSetValueExW(k, L"AutoStart", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    LogReg("pref write", rc);
    RegCloseKey(k);
}
static bool ReadRunValue(std::wstring& out) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[MAX_PATH * 2] = L"";
    DWORD type = 0, size = sizeof(buf) - sizeof(wchar_t);
    LONG rc = RegQueryValueExW(k, L"FreezeMark", nullptr, &type, (BYTE*)buf, &size);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) return false;
    out = buf;
    return true;
}
static void WriteRunValue() {
    HKEY k = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                            0, KEY_WRITE, &k);
    if (rc != ERROR_SUCCESS) { LogReg("Run key open", rc); return; }
    std::wstring q = QuotedExePath();
    rc = RegSetValueExW(k, L"FreezeMark", 0, REG_SZ, (BYTE*)q.c_str(),
                        (DWORD)((q.size() + 1) * sizeof(wchar_t)));
    LogReg("Run write", rc);
    RegCloseKey(k);
}
static void DeleteRunValue() {
    HKEY k = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                            0, KEY_WRITE, &k);
    if (rc != ERROR_SUCCESS) { LogReg("Run key open (delete)", rc); return; }
    rc = RegDeleteValueW(k, L"FreezeMark");
    if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;  // already gone = goal reached
    LogReg("Run delete", rc);
    RegCloseKey(k);
}
static void InitAutoStart() {
    int pref = ReadAutoStartPref();
    if (pref < 0) {
        Log("autostart: no pref -> default-on applied");
        WriteRunValue();
        WriteAutoStartPref(true);
        g_autoStart = true;
    } else if (pref == 1) {
        g_autoStart = true;
        std::wstring cur;
        if (!ReadRunValue(cur)) {
            Log("autostart: Run missing -> rewriting");
            WriteRunValue();
        } else if (cur != QuotedExePath()) {
            Log("autostart: Run stale (exe moved) -> refreshing target");
            WriteRunValue();
        } else {
            Log("autostart: Run ok");
        }
    } else {
        g_autoStart = false;
        std::wstring cur;
        if (ReadRunValue(cur)) {
            Log("autostart: pref off but Run present -> removing");
            DeleteRunValue();
        } else {
            Log("autostart: off (remembered), Run absent");
        }
    }
    Log("autostart: state=%s", g_autoStart ? "on" : "off");
}
static void SetAutoStart(bool on) {
    Log("autostart: toggled -> %s", on ? "on" : "off");
    if (on) WriteRunValue(); else DeleteRunValue();
    WriteAutoStartPref(on);
    g_autoStart = on;
}

// ---------- state ----------
struct Shape {
    int type;        // 0 rect, 1 rounded rect, 2 ellipse
    COLORREF color;
    int width;       // px
    int dash;        // 0 solid, 1 dash
    RECT rc;         // normalized, freeze-window client coords
};

static HINSTANCE g_inst = nullptr;
static HWND g_mainWnd = nullptr;    // hidden owner window (tray + hook target)
static HWND g_freezeWnd = nullptr;
static HWND g_toolbarWnd = nullptr;
static HHOOK g_hook = nullptr;
static HBITMAP g_frozenBmp = nullptr;
static int g_vx = 0, g_vy = 0, g_vw = 0, g_vh = 0;  // virtual screen
static bool g_frozen = false;

static std::vector<Shape> g_shapes;
static bool g_dragging = false;
static POINT g_dragStart{0,0}, g_dragCur{0,0};

static int g_shapeType = 0;                    // default rectangle
static int g_dash = 0;                         // default solid
static int g_lineWidth = 3;                    // 1..20
static COLORREF g_color = RGB(255, 59, 48);    // default red
static DWORD g_hintUntil = 0;                  // (legacy, unused since v1.2: no on-screen text hints)

static const COLORREF kPalette[8] = {
    RGB(255, 59, 48),   // red
    RGB(255, 149, 0),   // orange
    RGB(255, 204, 0),   // yellow
    RGB(52, 199, 89),   // green
    RGB(0, 122, 255),   // blue
    RGB(175, 82, 222),  // purple
    RGB(0, 0, 0),       // black
    RGB(255, 255, 255), // white
};

// ---------- toolbar layout (single source for paint + hit-test) ----------
// Floating window v1.2: glass oval ball (collapsed) / glass panel (expanded).
// Collapsed ball shows ONLY the line width ("N px") + a stroke preview;
// no instruction text anywhere. Panel controls are icon-only.
static const int BALL_W = 132, BALL_H = 64;    // window size (body 124x56 + hard shadow)
static const int PANEL_W = 592, PANEL_H = 120; // window size (body 584x112 + hard shadow)
static bool g_toolbarExpanded = false;
static bool g_tbPlainFallback = false;  // watchdog stage C: no-region plain bar
// fade state: 0 shown, 1 fading out, 2 hidden (while drawing), 3 fading in
static int g_tbVisState = 0;
static int g_tbCurAlpha = 255;
// Animation generations: bumped whenever a new animation starts or the
// toolbar is destroyed, so a stale animation thread aborts instantly.
// v1.8: animations run on worker threads, NOT on WM_TIMER - timer
// messages starve behind mouse-move/paint traffic while drawing, which
// is exactly what made the v1.7 fades stutter.
static volatile long g_fadeGen = 0;
static volatile long g_morphGen = 0;
static int g_wdRetry = 0;  // watchdog deferrals while drawing/fading
static const RECT kBallZone{10, 15, 94, 97};   // panel coords of the mini-ball (collapse)
struct TbItem { RECT rc; int id; };
// ids: 1 undo, 2 clear, 10/11/12 shapes, 20/21 dash, 30+i colors (panel coords)
static std::vector<TbItem> LayoutPanel() {
    std::vector<TbItem> v;
    v.push_back({{98, 12, 146, 50}, 1});    // undo
    v.push_back({{152, 12, 200, 50}, 2});   // clear
    v.push_back({{210, 12, 266, 50}, 10});  // rect
    v.push_back({{272, 12, 328, 50}, 11});  // rounded rect
    v.push_back({{334, 12, 390, 50}, 12});  // ellipse
    v.push_back({{400, 12, 456, 50}, 20});  // solid
    v.push_back({{462, 12, 518, 50}, 21});  // dash
    int cx = 98; const int s = 34;
    for (int i = 0; i < 8; i++) {
        v.push_back({{cx, 62, cx+s, 62+s}, 30+i});
        cx += s + 8;
    }
    return v;
}
static void RefreshToolbar() { if (g_toolbarWnd) InvalidateRect(g_toolbarWnd, nullptr, TRUE); }

// ---------- drawing helpers ----------
static void DrawOneShape(Graphics& g, const Shape& s) {
    Pen pen(Color(255, GetRValue(s.color), GetGValue(s.color), GetBValue(s.color)),
            (REAL)s.width);
    pen.SetDashStyle(s.dash ? DashStyleDash : DashStyleSolid);
    pen.SetLineJoin(LineJoinMiter);
    RectF r((REAL)s.rc.left, (REAL)s.rc.top,
            (REAL)(s.rc.right - s.rc.left), (REAL)(s.rc.bottom - s.rc.top));
    if (r.Width <= 0 || r.Height <= 0) return;
    if (s.type == 0) {
        g.DrawRectangle(&pen, r);
    } else if (s.type == 2) {
        g.DrawEllipse(&pen, r);
    } else {
        REAL rad = 18.0f;
        REAL d = rad * 2;
        if (r.Width < d) d = r.Width;
        if (r.Height < d) d = r.Height;
        GraphicsPath path;
        path.AddArc(r.X, r.Y, d, d, 180, 90);
        path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
        path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
        path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
        path.CloseFigure();
        g.DrawPath(&pen, &path);
    }
}
static RECT NormRect(POINT a, POINT b) {
    RECT r{ std::min(a.x, b.x), std::min(a.y, b.y),
            std::max(a.x, b.x), std::max(a.y, b.y) };
    return r;
}

// ---------- freeze on/off ----------
static bool CaptureScreen() {
    g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (g_vw <= 0 || g_vh <= 0) { Log("capture: bad virtual size %dx%d", g_vw, g_vh); return false; }
    HDC screen = GetDC(nullptr);
    if (!screen) { Log("capture: GetDC failed gle=%lu", GetLastError()); return false; }
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, g_vw, g_vh);
    if (!mem || !bmp) {
        Log("capture: dc/bmp failed mem=%p bmp=%p gle=%lu", (void*)mem, (void*)bmp, GetLastError());
        if (mem) DeleteDC(mem); if (bmp) DeleteObject(bmp);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    BOOL ok = BitBlt(mem, 0, 0, g_vw, g_vh, screen, g_vx, g_vy, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (!ok) { Log("capture: BitBlt failed gle=%lu", GetLastError()); DeleteObject(bmp); return false; }
    g_frozenBmp = bmp;
    Log("capture: ok %dx%d at %d,%d", g_vw, g_vh, g_vx, g_vy);
    return true;
}

static void ExitFreeze();  // fwd
static void SnapToolbarCollapsed();  // fwd
static void StartToolbarFade(bool out);  // fwd
static void CycleColor();  // fwd
static void SetToolbarLayered(bool on);  // fwd

static LRESULT CALLBACK FreezeProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        // Paint ONLY the update region (v1.8): the old code rebuilt a
        // full-virtual-screen bitmap on every paint, so each mouse move
        // during a drag cost several full-screen copies - that flood is
        // what starved the UI thread and made animations stutter.
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT pr = ps.rcPaint;
        int pw = (int)(pr.right - pr.left), ph = (int)(pr.bottom - pr.top);
        if (pw <= 0 || ph <= 0) { EndPaint(h, &ps); return 0; }
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP buf = CreateCompatibleBitmap(dc, pw, ph);
        HGDIOBJ old = SelectObject(mem, buf);
        if (g_frozenBmp) {
            HDC src = CreateCompatibleDC(dc);
            HGDIOBJ o2 = SelectObject(src, g_frozenBmp);
            BitBlt(mem, 0, 0, pw, ph, src, (int)pr.left, (int)pr.top, SRCCOPY);
            SelectObject(src, o2); DeleteDC(src);
        }
        {
            Graphics g(mem);
            g.TranslateTransform(-(REAL)pr.left, -(REAL)pr.top);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            for (auto& s : g_shapes) DrawOneShape(g, s);
            if (g_dragging) {
                Shape cur{g_shapeType, g_color, g_lineWidth, g_dash, NormRect(g_dragStart, g_dragCur)};
                DrawOneShape(g, cur);
            }
        }
        BitBlt(dc, (int)pr.left, (int)pr.top, pw, ph, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(buf); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        // Drawing starts: the panel auto-retracts to the ball (v1.8
        // request) and the capsule fades out for as long as the button
        // is held; on release it fades back in AS THE BALL.
        SnapToolbarCollapsed();
        StartToolbarFade(true);
        g_dragging = true;
        g_dragStart = g_dragCur = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SetCapture(h);
        return 0;
    case WM_MOUSEMOVE:
        if (g_dragging) {
            POINT np{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT u = NormRect(g_dragStart, g_dragCur);
            RECT v = NormRect(g_dragStart, np);
            UnionRect(&u, &u, &v);
            InflateRect(&u, g_lineWidth + 4, g_lineWidth + 4);
            g_dragCur = np;
            InvalidateRect(h, &u, FALSE);  // only the changed region repaints
        }
        return 0;
    case WM_LBUTTONUP: {
        if (!g_dragging) return 0;
        g_dragging = false; ReleaseCapture();
        g_dragCur = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT r = NormRect(g_dragStart, g_dragCur);
        if (r.right - r.left >= 5 && r.bottom - r.top >= 5) {
            g_shapes.push_back({g_shapeType, g_color, g_lineWidth, g_dash, r});
            Log("shape: add type=%d dash=%d width=%d rect=%ld,%ld-%ld,%ld total=%u",
                g_shapeType, g_dash, g_lineWidth, r.left, r.top, r.right, r.bottom,
                (unsigned)g_shapes.size());
            RefreshToolbar();
        }
        RECT iv = r; InflateRect(&iv, g_lineWidth + 4, g_lineWidth + 4);
        InvalidateRect(h, &iv, FALSE);
        StartToolbarFade(false);  // button released: ball fades back in
        return 0;
    }
    case WM_MOUSEWHEEL: {
        // Accumulate sub-notch deltas (high-res wheels) so every notch = 1px.
        // While dragging, the freeze window holds mouse capture, so wheel
        // messages land here mid-drag; repainting the preview immediately is
        // what makes the width change visible in real time (v1.4 fix: the
        // preview used to only repaint on the next mouse move).
        static int s_wheelAcc = 0;
        s_wheelAcc += GET_WHEEL_DELTA_WPARAM(wp);
        int steps = s_wheelAcc / WHEEL_DELTA;
        s_wheelAcc -= steps * WHEEL_DELTA;
        if (steps != 0) {
            int nw = g_lineWidth + steps;
            if (nw < 1) nw = 1; if (nw > 20) nw = 20;
            if (nw != g_lineWidth) {
                g_lineWidth = nw;
                Log("wheel: lineWidth=%d%s", g_lineWidth, g_dragging ? " (during drag)" : "");
                if (g_dragging) {
                    RECT u = NormRect(g_dragStart, g_dragCur);
                    InflateRect(&u, 28, 28);
                    InvalidateRect(h, &u, FALSE);
                }
                RefreshToolbar();
            }
        }
        return 0;
    }
    case WM_MBUTTONDOWN: {
        // Middle button: cycle to the next palette color (v1.11 request).
        Log("mbutton: down (dragging=%d visState=%d alpha=%d)",
            g_dragging ? 1 : 0, g_tbVisState, g_tbCurAlpha);
        CycleColor();
        if (g_dragging) {  // recolor the in-progress preview immediately
            RECT u = NormRect(g_dragStart, g_dragCur);
            InflateRect(&u, g_lineWidth + 4, g_lineWidth + 4);
            InvalidateRect(h, &u, FALSE);
        }
        // v1.13: the user must SEE the new color on the capsule. v1.12
        // routed this through the fade-in machinery and on the real
        // machine the capsule still did not come back, so now the
        // capsule is forced visible SYNCHRONOUSLY in this handler - no
        // threads, no posted messages, no state machine: cancel any
        // fade, restore full opacity, drop the layered style, show.
        if (g_toolbarWnd) {
            ++g_fadeGen;  // any running fade thread aborts
            if (GetWindowLongPtrW(g_toolbarWnd, GWL_EXSTYLE) & WS_EX_LAYERED) {
                SetLayeredWindowAttributes(g_toolbarWnd, 0, 255, LWA_ALPHA);
                SetToolbarLayered(false);
            }
            g_tbVisState = 0;
            g_tbCurAlpha = 255;
            ShowWindow(g_toolbarWnd, SW_SHOW);
            SetWindowPos(g_toolbarWnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            InvalidateRect(g_toolbarWnd, nullptr, TRUE);
            UpdateWindow(g_toolbarWnd);
            Log("mbutton: capsule force-shown, visible=%d",
                IsWindowVisible(g_toolbarWnd) ? 1 : 0);
        }
        return 0;
    }
    case WM_RBUTTONDOWN:
        Log("freeze: exit by right-click");
        PostMessage(g_mainWnd, WM_EXIT_FREEZE, 0, 0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { Log("freeze: exit by Esc"); PostMessage(g_mainWnd, WM_EXIT_FREEZE, 0, 0); }
        return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}

// ---------- toolbar: glass ball / glass panel ----------
// Middle-button color cycling: advance to the next palette entry,
// wrapping from white back to red. The ball's dot and the panel's
// selection ring follow via RefreshToolbar.
static void CycleColor() {
    int idx = 0;
    for (int i = 0; i < 8; i++) if (kPalette[i] == g_color) { idx = i; break; }
    idx = (idx + 1) % 8;
    g_color = kPalette[idx];
    Log("mbutton: cycle color idx=%d", idx);
    RefreshToolbar();
}

static void ToolbarAction(int id) {
    if (id == 1) {
        if (!g_shapes.empty()) { g_shapes.pop_back(); Log("shape: undo total=%u", (unsigned)g_shapes.size()); }
    } else if (id == 2) {
        if (!g_shapes.empty()) { g_shapes.clear(); Log("shape: clear all"); }
    } else if (id >= 10 && id <= 12) {
        g_shapeType = id - 10; Log("toolbar: shapeType=%d", g_shapeType);
    } else if (id == 20 || id == 21) {
        g_dash = id - 20; Log("toolbar: dash=%d", g_dash);
    } else if (id >= 30 && id < 38) {
        g_color = kPalette[id - 30];
        Log("toolbar: color idx=%d", id - 30);
    }
    if (g_freezeWnd) InvalidateRect(g_freezeWnd, nullptr, FALSE);
    RefreshToolbar();
}

static void RoundedPath(GraphicsPath& path, RectF r, REAL rad) {
    REAL d = rad * 2;
    if (r.Width < d) d = r.Width;
    if (r.Height < d) d = r.Height;
    path.Reset();
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

// Sketchbook-style body (v1.3): cream paper capsule, hard offset ink shadow,
// hand-drawn double ink border (second stroke slightly offset = wobble).
// Body occupies (2,2,W-12,H-12) of the window; the shadow peeks right/bottom.
static const Color kInk(255, 32, 36, 46);
static const Color kPaper(255, 255, 253, 244);
static void PaintSketchBody(Graphics& g, int W, int H, REAL rad) {
    // body inset by 2 so its border stroke never touches the window region
    // edge (region radius == paint radius, see ToolbarRegionEllipse)
    RectF body(2, 2, (REAL)W - 12, (REAL)H - 12);
    GraphicsPath shadow; RoundedPath(shadow, RectF(8, 9, body.Width, body.Height), rad);
    SolidBrush shb(kInk); g.FillPath(&shb, &shadow);
    GraphicsPath bp; RoundedPath(bp, body, rad);
    SolidBrush pb(kPaper); g.FillPath(&pb, &bp);
    Pen ink1(kInk, 2.2f); g.DrawPath(&ink1, &bp);
    GraphicsPath bp2; RoundedPath(bp2, RectF(3.5f, 1.0f, body.Width, body.Height), rad - 2);
    Pen ink2(Color(110, 32, 36, 46), 1.1f); g.DrawPath(&ink2, &bp2);
}

// The one and only rendering of the width label, shared by the ball
// face and the panel's mini zone (v1.14): both states format the same
// live g_lineWidth through this single function.
static void DrawWidthLabel(Graphics& g, RectF r, REAL fontPt) {
    wchar_t txt[32]; swprintf(txt, 32, L"%d px", g_lineWidth);
    // Auto-fit (v1.15): Segoe Print is a wide font - on the ball, the
    // two-digit labels ("10 px" and up) overflowed the layout rect and
    // were center-clipped into a wrong-looking value, while one-digit
    // labels fit. Measure first and step the size down until the whole
    // string fits the rect, so the label is never clipped.
    REAL pt = fontPt;
    while (pt > 9.0f) {
        Font probe(L"Segoe Print", pt, FontStyleBold);
        RectF bound;
        g.MeasureString(txt, -1, &probe, PointF(0, 0), &bound);
        if (bound.Width <= r.Width - 2) break;
        pt -= 1.0f;
    }
    Font font(L"Segoe Print", pt, FontStyleBold);
    SolidBrush fg(kInk);
    StringFormat sf; sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(txt, -1, &font, r, &sf, &fg);
}

// the ball face (paper capsule): color dot + ONLY "N px" in a hand style
// font + a stroke sample in the current color/width. r = body rect.
static void PaintBallFace(Graphics& g, RectF r) {
    REAL cy = r.Y + r.Height / 2;
    Color cur(255, GetRValue(g_color), GetGValue(g_color), GetBValue(g_color));
    // current-color dot with ink ring (left)
    REAL dr = r.Height * 0.17f; if (dr < 5) dr = 5;
    SolidBrush dot(cur);
    g.FillEllipse(&dot, r.X + 13, cy - dr, dr * 2, dr * 2);
    Pen ring(kInk, 1.6f);
    g.DrawEllipse(&ring, r.X + 13, cy - dr, dr * 2, dr * 2);
    // "N px" text (ink, Segoe Print = hand-drawn feel, falls back gracefully)
    // v1.15: the rect used to keep a 28px right margin for the stroke
    // preview removed in v1.6 - reclaim that width for the label.
    REAL fs = r.Height * 0.34f; if (fs < 9) fs = 9; if (fs > 17) fs = 17;
    RectF tr(r.X + 15 + dr * 2, r.Y, r.Width - (21 + dr * 2), r.Height);
    DrawWidthLabel(g, tr, fs);
    // (v1.6: no stroke preview on the collapsed capsule, per user request -
    //  just the color dot and the "N px" number.)
    (void)cy;
}

static void PaintPanelIcon(Graphics& g, const TbItem& it) {
    RectF r((REAL)it.rc.left, (REAL)it.rc.top,
            (REAL)(it.rc.right - it.rc.left), (REAL)(it.rc.bottom - it.rc.top));
    bool disabled = (it.id == 1 || it.id == 2) && g_shapes.empty();
    bool sel = (it.id >= 10 && it.id <= 12 && g_shapeType == it.id - 10) ||
               (it.id == 20 && g_dash == 0) || (it.id == 21 && g_dash == 1);
    GraphicsPath bg; RoundedPath(bg, r, 9);
    SolidBrush fill(sel ? Color(255, 255, 217, 61)                 // sketchbook highlighter yellow
                        : Color(255, 255, 255, 255));              // plain paper white
    g.FillPath(&fill, &bg);
    Pen btnRim(kInk, 1.6f); g.DrawPath(&btnRim, &bg);
    Color ink = disabled ? Color(255, 154, 160, 168) : kInk;
    Pen pen(ink, 2.2f);
    REAL cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
    if (it.id == 1) {           // undo: open arc + arrowhead
        g.DrawArc(&pen, cx - 11.0f, cy - 11.0f, 22.0f, 22.0f, 40.0f, 300.0f);
        PointF pts[3] = { PointF(cx + 8.5f, cy + 9.5f), PointF(cx + 0.5f, cy + 8.0f),
                          PointF(cx + 7.0f, cy + 0.5f) };
        SolidBrush ah(ink); g.FillPolygon(&ah, pts, 3);
    } else if (it.id == 2) {    // clear: frame + X
        g.DrawRectangle(&pen, cx - 10.0f, cy - 10.0f, 20.0f, 20.0f);
        Pen px(ink, 2.0f);
        g.DrawLine(&px, cx - 6, cy - 6, cx + 6, cy + 6);
        g.DrawLine(&px, cx + 6, cy - 6, cx - 6, cy + 6);
    } else if (it.id == 10) {
        g.DrawRectangle(&pen, cx - 13.0f, cy - 10.0f, 26.0f, 20.0f);
    } else if (it.id == 11) {
        GraphicsPath p; RoundedPath(p, RectF(cx - 13, cy - 10, 26, 20), 6);
        g.DrawPath(&pen, &p);
    } else if (it.id == 12) {
        g.DrawEllipse(&pen, cx - 13.0f, cy - 10.0f, 26.0f, 20.0f);
    } else if (it.id == 20 || it.id == 21) {
        Pen lp(ink, 3.5f);
        if (it.id == 21) lp.SetDashStyle(DashStyleDash);
        g.DrawLine(&lp, r.X + 8, cy, r.X + r.Width - 8, cy);
    }
}

static void PaintColorSwatch(Graphics& g, const TbItem& it) {
    COLORREF c = kPalette[it.id - 30];
    bool sel = (c == g_color);
    RectF r((REAL)it.rc.left + 3, (REAL)it.rc.top + 2,
            (REAL)(it.rc.right - it.rc.left) - 6, (REAL)(it.rc.bottom - it.rc.top) - 4);
    SolidBrush b(Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
    g.FillEllipse(&b, r);
    Pen rim(kInk, sel ? 2.6f : 1.4f);
    g.DrawEllipse(&rim, r);
    if (sel) { Pen outer(kInk, 1.2f); g.DrawEllipse(&outer, r.X - 3, r.Y - 3, r.Width + 6, r.Height + 6); }
}

// toolbar drag state (ball click toggles panel; drag moves the window)
static bool s_tbDown = false, s_tbMoved = false;
static POINT s_tbDownPt{0, 0};
static RECT s_tbWin0{0, 0, 0, 0};

static void SetToolbarLayered(bool on);  // fwd (defined with the fade code)

// morph animation state (driven by MorphThread; ToolbarProc finalizes)
static bool g_tbAnimating = false;
static int g_tbFromW = 0, g_tbFromH = 0, g_tbToW = 0, g_tbToH = 0;
// Eased morph progress, 0 = ball, 1 = panel (v1.9 content crossfade:
// the paint routine reads this to fade the two content layers).
static float g_tbMorphE = 0.0f;
// Set the capsule region, then READ IT BACK: if it came back empty/error
// (a window with an empty region is fully invisible - the v1.2/v1.3
// vanishing suspect), drop the region so the window at least shows.
// ONE source for the window-region corner size, matching the painted
// radius (v1.5 photo bug: region semicircle clipped the painted borders).
// Returns the CreateRoundRectRgn ellipse diameter for the current state.
static int ToolbarRegionEllipse(int h) {
    int e = g_toolbarExpanded ? 36 : (h - 6);
    if (e > h) e = h;
    if (e < 4) e = 4;
    return e;
}
static void SetToolbarRegionVerified(int w, int h) {
    if (!g_toolbarWnd) return;
    int e = ToolbarRegionEllipse(h);
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, e, e);
    SetWindowRgn(g_toolbarWnd, rgn, TRUE);
    HRGN probe = CreateRectRgn(0, 0, 0, 0);
    int type = GetWindowRgn(g_toolbarWnd, probe);  // 0 err, 1 null, 2 simple, 3 complex
    DeleteObject(probe);
    Log("toolbar: region readback type=%d", type);
    if (type == 0 || type == 1) {
        Log("toolbar: region EMPTY -> removing region (rectangular fallback)");
        SetWindowRgn(g_toolbarWnd, nullptr, TRUE);
    }
}
static void ApplyToolbarSize(int nw, int nh) {
    if (!g_toolbarWnd) return;
    RECT cur; GetWindowRect(g_toolbarWnd, &cur);
    int cx = (cur.left + cur.right) / 2;
    int nx = cx - nw / 2;
    if (g_vw > 0) {
        if (nx < g_vx) nx = g_vx;
        if (nx + nw > g_vx + g_vw) nx = g_vx + g_vw - nw;
    }
    SetWindowPos(g_toolbarWnd, HWND_TOPMOST, nx, cur.top, nw, nh,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    // region rebuild throttled during morph steps (final size is always
    // re-applied exactly by SetToolbarRegionVerified at the end)
    static int s_lastRgnW = -1, s_lastRgnH = -1;
    if (abs(nw - s_lastRgnW) >= 2 || abs(nh - s_lastRgnH) >= 2 || s_lastRgnW < 0) {
        s_lastRgnW = nw; s_lastRgnH = nh;
        int e = ToolbarRegionEllipse(nh);
        HRGN rgn = CreateRoundRectRgn(0, 0, nw + 1, nh + 1, e, e);
        SetWindowRgn(g_toolbarWnd, rgn, TRUE);
    }
}

// Panel content (mini zone + icons), drawn in final panel coordinates.
// Used directly for the static expanded state, and into an offscreen
// layer for the morph crossfade.
static void PaintPanelContent(Graphics& g) {
    RectF zr((REAL)kBallZone.left, (REAL)kBallZone.top,
             (REAL)(kBallZone.right - kBallZone.left),
             (REAL)(kBallZone.bottom - kBallZone.top));
    GraphicsPath zp; RoundedPath(zp, zr, 14);
    SolidBrush zf(Color(255, 243, 238, 221)); g.FillPath(&zf, &zp);
    Pen zrim(kInk, 1.8f); g.DrawPath(&zrim, &zp);
    // single line, never wraps: "9" over "px" was the v1.5 photo bug
    DrawWidthLabel(g, zr, 11.5f);
    for (auto& it : LayoutPanel()) {
        if (it.id >= 30) PaintColorSwatch(g, it);
        else PaintPanelIcon(g, it);
    }
}

// Composite an offscreen content layer onto g with a global alpha
// (GDI+ color matrix), used by the morph crossfade (v1.9).
static void DrawLayerWithAlpha(Graphics& g, Bitmap& bmp, REAL x, REAL y, float alpha) {
    if (alpha <= 0.003f) return;
    if (alpha >= 0.997f) { g.DrawImage(&bmp, x, y); return; }
    ColorMatrix cm{};
    cm.m[0][0] = 1; cm.m[1][1] = 1; cm.m[2][2] = 1; cm.m[3][3] = alpha; cm.m[4][4] = 1;
    ImageAttributes ia; ia.SetColorMatrix(&cm);
    RectF dst(x, y, (REAL)bmp.GetWidth(), (REAL)bmp.GetHeight());
    g.DrawImage(&bmp, dst, 0, 0, (REAL)bmp.GetWidth(), (REAL)bmp.GetHeight(),
                UnitPixel, &ia);
}

static LRESULT CALLBACK ToolbarProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT crc; GetClientRect(h, &crc);
        int W = (int)(crc.right - crc.left), H = (int)(crc.bottom - crc.top);
        if (W <= 0 || H <= 0) { EndPaint(h, &ps); return 0; }
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP buf = CreateCompatibleBitmap(dc, W, H);
        HGDIOBJ old = SelectObject(mem, buf);
        {
            Graphics g(mem);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            if (g_tbPlainFallback) {
                // watchdog stage C: simplest possible paint, whole rectangle
                g.Clear(kPaper);
                Pen bd(kInk, 2.0f);
                g.DrawRectangle(&bd, 1.0f, 1.0f, (REAL)W - 2, (REAL)H - 2);
                PaintBallFace(g, RectF(0, 0, (REAL)W, (REAL)H));
                BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old); DeleteObject(buf); DeleteDC(mem);
                EndPaint(h, &ps);
                return 0;
            }
            // clear corners to the frozen screenshot color is impossible here;
            // window region clips corners, buffer corners just get paper color
            g.Clear(kPaper);
            if (g_tbAnimating) {
                // Content crossfade (v1.9): the paper body stays opaque
                // and morphs; the ball content fades out over the first
                // 60% of progress while the panel content fades in over
                // the last 60%, both pre-rendered on offscreen layers.
                float e = g_tbMorphE; if (e < 0) e = 0; if (e > 1) e = 1;
                REAL radBall = (REAL)(BALL_H - 12) / 2;
                PaintSketchBody(g, W, H, radBall + (18.0f - radBall) * e);
                float ballA = 1.0f - e / 0.6f; if (ballA < 0) ballA = 0;
                float panelA = (e - 0.4f) / 0.6f;
                if (panelA < 0) panelA = 0; if (panelA > 1) panelA = 1;
                if (ballA > 0.003f) {
                    Bitmap lb(BALL_W, BALL_H, PixelFormat32bppARGB);
                    Graphics lg(&lb);
                    lg.SetSmoothingMode(SmoothingModeAntiAlias);
                    lg.Clear(Color(0, 0, 0, 0));
                    PaintBallFace(lg, RectF(2, 2, (REAL)BALL_W - 12, (REAL)BALL_H - 12));
                    DrawLayerWithAlpha(g, lb, (REAL)(W - BALL_W) / 2, 0, ballA);
                }
                if (panelA > 0.003f) {
                    Bitmap pb(PANEL_W, PANEL_H, PixelFormat32bppARGB);
                    Graphics pg(&pb);
                    pg.SetSmoothingMode(SmoothingModeAntiAlias);
                    pg.Clear(Color(0, 0, 0, 0));
                    PaintPanelContent(pg);
                    DrawLayerWithAlpha(g, pb, (REAL)(W - PANEL_W) / 2, 0, panelA);
                }
            } else if (!g_toolbarExpanded) {
                PaintSketchBody(g, W, H, (REAL)(H - 12) / 2);
                PaintBallFace(g, RectF(2, 2, (REAL)W - 12, (REAL)H - 12));
            } else {
                PaintSketchBody(g, W, H, 18.0f);
                PaintPanelContent(g);
            }
        }
        BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(buf); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_APP + 41: {  // fade finished (posted by the fade worker thread)
        if ((long)wp != g_fadeGen) return 0;  // stale animation
        if ((int)lp == 0) {
            g_tbCurAlpha = 0;
            SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
            ShowWindow(h, SW_HIDE);
            g_tbVisState = 2;
            Log("toolbar: hidden while drawing");
        } else {
            g_tbCurAlpha = 255;
            SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
            SetToolbarLayered(false);
            g_tbVisState = 0;
            // Self-heal: whatever may have stripped the window region,
            // re-applying it at every fade-in keeps square corners from
            // ever persisting past one marking cycle.
            RECT crc; GetClientRect(h, &crc);
            SetToolbarRegionVerified((int)(crc.right - crc.left),
                                     (int)(crc.bottom - crc.top));
            // Synchronous repaint BEFORE the window settles visible, so
            // it can never present a stale frame painted before the last
            // width change (v1.14 expanded/collapsed value mismatch).
            InvalidateRect(h, nullptr, TRUE);
            UpdateWindow(h);
            Log("toolbar: fade in settled, width=%d", g_lineWidth);
            if (!IsWindowVisible(h)) {
                ShowWindow(h, SW_SHOW);
                Log("toolbar: fade-in recovery reshow");
            }
            Log("toolbar: fade in done");
        }
        return 0;
    }
    case WM_APP + 42: {  // morph finished (posted by the morph worker thread)
        if ((long)wp != g_morphGen) return 0;  // stale animation
        g_tbAnimating = false;
        g_tbMorphE = g_toolbarExpanded ? 1.0f : 0.0f;
        ApplyToolbarSize(g_tbToW, g_tbToH);
        SetToolbarRegionVerified(g_tbToW, g_tbToH);
        InvalidateRect(h, nullptr, TRUE);
        UpdateWindow(h);  // settle on a fresh frame, never a stale one
        Log("toolbar: morph done -> %s, width=%d",
            g_toolbarExpanded ? "panel" : "ball", g_lineWidth);
        return 0;
    }
    case WM_DESTROY:
        Log("toolbar: WM_DESTROY frozen=%d", g_frozen ? 1 : 0);
        return 0;
    case WM_LBUTTONDOWN: {
        if (g_tbAnimating) return 0;
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (g_toolbarExpanded) {
            if (PtInRect(&kBallZone, pt)) { /* collapse handled on mouse-up via drag logic */ }
            else {
                bool hit = false;
                for (auto& it : LayoutPanel())
                    if (PtInRect(&it.rc, pt)) { ToolbarAction(it.id); hit = true; break; }
                if (hit) return 0;
            }
        }
        s_tbDown = true; s_tbMoved = false;
        GetCursorPos(&s_tbDownPt);
        GetWindowRect(h, &s_tbWin0);
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!s_tbDown) return 0;
        POINT cur; GetCursorPos(&cur);
        int dx = cur.x - s_tbDownPt.x, dy = cur.y - s_tbDownPt.y;
        if (dx * dx + dy * dy > 16) s_tbMoved = true;
        if (s_tbMoved)
            SetWindowPos(h, nullptr, s_tbWin0.left + dx, s_tbWin0.top + dy, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!s_tbDown) return 0;
        s_tbDown = false; ReleaseCapture();
        if (!s_tbMoved) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (!g_toolbarExpanded) {
                // plain click on the ball expands the panel (defined below)
                PostMessage(h, WM_APP + 10, 1, 0);
            } else if (PtInRect(&kBallZone, pt)) {
                PostMessage(h, WM_APP + 10, 0, 0);
            }
        }
        return 0;
    }
    case WM_APP + 10: {  // toggle expanded state, deferred so capture is released
        extern void SetToolbarExpanded(bool);  // (function follows this proc)
        SetToolbarExpanded(wp != 0);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (g_freezeWnd) SendMessage(g_freezeWnd, WM_MOUSEWHEEL, wp, lp);
        return 0;
    case WM_MBUTTONDOWN:
        Log("mbutton: down on capsule -> forwarding to canvas");
        if (g_freezeWnd) SendMessage(g_freezeWnd, WM_MBUTTONDOWN, wp, lp);
        return 0;
    case WM_RBUTTONDOWN:
        Log("freeze: exit by toolbar right-click");
        PostMessage(g_mainWnd, WM_EXIT_FREEZE, 0, 0);
        return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}

// ---------- animation worker threads (v1.8) ----------
// WM_TIMER messages are the lowest priority in the queue: while the user
// drags, mouse-move + full-screen paint traffic starves them, so v1.7's
// timer-driven fades advanced in a few coarse jumps. These workers run on
// their own threads with a 5 ms sleep loop, so step timing no longer
// depends on the UI thread's queue. Fades only touch the layered alpha
// (pure DWM composition, no paint needed); morphs call SetWindowPos.
// Both post a finalize message back to ToolbarProc when done, and both
// abort instantly when their generation is bumped.
static float SpringBezier(float t) {
    // cubic-bezier(0.3, 1.25, 0.45, 1): solve x(u)=t by Newton, take y(u)
    auto bx = [](float u) { float v = 1 - u; return 3 * v * v * u * 0.3f + 3 * v * u * u * 0.45f + u * u * u; };
    auto by = [](float u) { float v = 1 - u; return 3 * v * v * u * 1.25f + 3 * v * u * u * 1.0f + u * u * u; };
    float u = t;
    for (int i = 0; i < 5; i++) {
        float err = bx(u) - t;
        if (fabsf(err) < 0.001f) break;
        float d = 3 * (1 - u) * (1 - u) * 0.3f + 6 * (1 - u) * u * 0.15f + 3 * u * u * 0.55f;
        if (d == 0) break;
        u -= err / d;
    }
    return by(u);  // slight overshoot past 1 mid-way = spring settle
}
struct FadeJob { long gen; int from; int target; int dur; HWND hwnd; };
static DWORD WINAPI FadeThread(LPVOID p) {
    FadeJob* j = (FadeJob*)p;
    DWORD t0 = timeGetTime();
    while (j->gen == g_fadeGen && IsWindow(j->hwnd)) {
        float t = (float)(timeGetTime() - t0) / (float)j->dur;
        if (t >= 1.0f) {
            PostMessageW(j->hwnd, WM_APP + 41, (WPARAM)j->gen, (LPARAM)j->target);
            break;
        }
        float e = 1.0f - (1.0f - t) * (1.0f - t);
        g_tbCurAlpha = j->from + (int)((j->target - j->from) * e);
        SetLayeredWindowAttributes(j->hwnd, 0, (BYTE)g_tbCurAlpha, LWA_ALPHA);
        Sleep(5);
    }
    delete j;
    return 0;
}
struct MorphJob { long gen; int fromW, fromH, toW, toH; HWND hwnd; };
static DWORD WINAPI MorphThread(LPVOID p) {
    MorphJob* j = (MorphJob*)p;
    DWORD t0 = timeGetTime();
    while (j->gen == g_morphGen && IsWindow(j->hwnd)) {
        float t = (float)(timeGetTime() - t0) / 300.0f;
        if (t >= 1.0f) {
            PostMessageW(j->hwnd, WM_APP + 42, (WPARAM)j->gen, 0);
            break;
        }
        float e = SpringBezier(t);
        g_tbMorphE = (j->toW > j->fromW) ? e : 1.0f - e;  // 0=ball, 1=panel
        int w = j->fromW + (int)((j->toW - j->fromW) * e);
        int hh = j->fromH + (int)((j->toH - j->fromH) * e);
        if (w < 40) w = 40;
        if (hh < 30) hh = 30;
        ApplyToolbarSize(w, hh);
        InvalidateRect(j->hwnd, nullptr, TRUE);
        Sleep(5);
    }
    delete j;
    return 0;
}

void SetToolbarExpanded(bool exp) {
    if (!g_toolbarWnd || g_toolbarExpanded == exp) return;
    g_toolbarExpanded = exp;
    RECT cur; GetWindowRect(g_toolbarWnd, &cur);
    g_tbFromW = (int)(cur.right - cur.left);
    g_tbFromH = (int)(cur.bottom - cur.top);
    g_tbToW = exp ? PANEL_W : BALL_W;
    g_tbToH = exp ? PANEL_H : BALL_H;
    g_tbMorphE = exp ? 0.0f : 1.0f;
    g_tbAnimating = true;
    MorphJob* j = new MorphJob{++g_morphGen, g_tbFromW, g_tbFromH, g_tbToW, g_tbToH, g_toolbarWnd};
    HANDLE th = CreateThread(nullptr, 0, MorphThread, j, 0, nullptr);
    if (th) CloseHandle(th);
    else {  // thread creation failed: snap instantly instead of animating
        delete j;
        g_tbAnimating = false;
        ApplyToolbarSize(g_tbToW, g_tbToH);
        SetToolbarRegionVerified(g_tbToW, g_tbToH);
    }
    Log("toolbar: morph -> %s", exp ? "panel" : "ball");
    InvalidateRect(g_toolbarWnd, nullptr, TRUE);
}

static void SnapToolbarCollapsed() {
    if (!g_toolbarWnd) return;
    if (g_tbAnimating) { ++g_morphGen; g_tbAnimating = false; }  // aborts the morph thread
    g_tbMorphE = 0.0f;
    if (g_toolbarExpanded) {
        g_toolbarExpanded = false;
        ApplyToolbarSize(BALL_W, BALL_H);
        SetToolbarRegionVerified(BALL_W, BALL_H);
        InvalidateRect(g_toolbarWnd, nullptr, TRUE);
    }
}

// --- fade in/out (v1.7): layered alpha is enabled ONLY during a fade and
// removed again at full opacity, so the steady state stays the plain
// opaque window that has proven visible on the user's machine.
static void SetToolbarLayered(bool on) {
    if (!g_toolbarWnd) return;
    LONG_PTR ex = GetWindowLongPtrW(g_toolbarWnd, GWL_EXSTYLE);
    LONG_PTR nx = on ? (ex | WS_EX_LAYERED) : (ex & ~WS_EX_LAYERED);
    if (nx != ex) {
        SetWindowLongPtrW(g_toolbarWnd, GWL_EXSTYLE, nx);
        SetWindowPos(g_toolbarWnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}
static void StartToolbarFade(bool out) {
    if (!g_toolbarWnd) return;
    if (out) {
        if (g_tbVisState == 1 || g_tbVisState == 2) return;
        SetToolbarLayered(true);
        FadeJob* j = new FadeJob{++g_fadeGen, g_tbCurAlpha, 0, 110, g_toolbarWnd};
        g_tbVisState = 1;
        HANDLE th = CreateThread(nullptr, 0, FadeThread, j, 0, nullptr);
        if (th) CloseHandle(th);
        else { delete j; g_tbCurAlpha = 0; ShowWindow(g_toolbarWnd, SW_HIDE); g_tbVisState = 2; }
        Log("toolbar: fade out (draw started)");
    } else {
        if (g_tbVisState == 0 || g_tbVisState == 3) return;
        // The capsule comes back as the BALL: starting a mark retracted
        // the panel (v1.8 request), and it stays retracted after the mark.
        SetToolbarLayered(true);
        if (!IsWindowVisible(g_toolbarWnd)) {
            SetLayeredWindowAttributes(g_toolbarWnd, 0, (BYTE)g_tbCurAlpha, LWA_ALPHA);
            ShowWindow(g_toolbarWnd, SW_SHOW);
        }
        FadeJob* j = new FadeJob{++g_fadeGen, g_tbCurAlpha, 255, 190, g_toolbarWnd};
        g_tbVisState = 3;
        HANDLE th = CreateThread(nullptr, 0, FadeThread, j, 0, nullptr);
        if (th) CloseHandle(th);
        else {
            delete j;
            g_tbCurAlpha = 255;
            SetLayeredWindowAttributes(g_toolbarWnd, 0, 255, LWA_ALPHA);
            SetToolbarLayered(false);
            g_tbVisState = 0;
        }
        InvalidateRect(g_toolbarWnd, nullptr, TRUE);
        UpdateWindow(g_toolbarWnd);
        Log("toolbar: fade in (draw ended)");
    }
}

// Create (or recreate) the toolbar window as the capsule. Factored out so
// the watchdog can rebuild it plain (no region) as a last resort.
static void CreateToolbarWindow(bool withRegion) {
    ++g_fadeGen; ++g_morphGen;  // abort any animation threads of the old window
    if (g_toolbarWnd) { DestroyWindow(g_toolbarWnd); g_toolbarWnd = nullptr; }
    g_toolbarExpanded = false;
    g_tbAnimating = false;
    g_tbMorphE = 0.0f;
    g_tbVisState = 0;
    g_tbCurAlpha = 255;
    g_toolbarWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"FreezeMarkToolbar",
                                   L"", WS_POPUP,
                                   g_vx + (g_vw - BALL_W) / 2, g_vy + 14, BALL_W, BALL_H,
                                   nullptr, nullptr, g_inst, nullptr);
    Log("toolbar: create(%s) hwnd=%p gle=%lu", withRegion ? "region" : "plain",
        (void*)g_toolbarWnd, GetLastError());
    if (!g_toolbarWnd) return;
    if (withRegion) SetToolbarRegionVerified(BALL_W, BALL_H);
    ShowWindow(g_toolbarWnd, SW_SHOW);
}


static void EnterFreeze() {
    if (g_frozen) return;
    if (!CaptureScreen()) { Log("freeze: enter aborted (capture failed)"); return; }
    g_shapes.clear();
    g_dragging = false;
    g_freezeWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"FreezeMarkFreeze",
                                  L"", WS_POPUP, g_vx, g_vy, g_vw, g_vh,
                                  nullptr, nullptr, g_inst, nullptr);
    if (!g_freezeWnd) { Log("freeze: window create failed gle=%lu", GetLastError()); DeleteObject(g_frozenBmp); g_frozenBmp = nullptr; return; }
    g_tbPlainFallback = false;
    g_frozen = true;
    ShowWindow(g_freezeWnd, SW_SHOW);
    CreateToolbarWindow(true);
    SetForegroundWindow(g_freezeWnd);
    SetFocus(g_freezeWnd);
    // re-assert the toolbar above the just-activated freeze window
    if (g_toolbarWnd) SetWindowPos(g_toolbarWnd, HWND_TOPMOST, 0, 0, 0, 0,
                                   SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    if (g_toolbarWnd) {
        RECT r; GetWindowRect(g_toolbarWnd, &r);
        Log("toolbar: after show visible=%d rect=%ld,%ld %ldx%ld",
            IsWindowVisible(g_toolbarWnd) ? 1 : 0, r.left, r.top,
            r.right - r.left, r.bottom - r.top);
        g_wdRetry = 0;
        SetTimer(g_mainWnd, 9, 1200, nullptr);  // one-shot watchdog, see MainProc
    }
    Log("freeze: enter ok toolbar=%p", (void*)g_toolbarWnd);
}

static void ExitFreeze() {
    if (!g_frozen && !g_freezeWnd) return;
    g_frozen = false;
    g_tbAnimating = false;
    ++g_fadeGen; ++g_morphGen;  // animation threads abort before the window dies
    if (g_mainWnd) { KillTimer(g_mainWnd, 9); KillTimer(g_mainWnd, 10); }
    if (g_toolbarWnd) { DestroyWindow(g_toolbarWnd); g_toolbarWnd = nullptr; }
    if (g_freezeWnd) { DestroyWindow(g_freezeWnd); g_freezeWnd = nullptr; }
    if (g_frozenBmp) { DeleteObject(g_frozenBmp); g_frozenBmp = nullptr; }
    g_shapes.clear();
    Log("freeze: exited");
}

// ---------- keyboard hook (Enter + Backspace) ----------
static bool g_enterDown = false, g_backDown = false, g_comboFired = false;
static LRESULT CALLBACK KbHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        bool up   = (wp == WM_KEYUP   || wp == WM_SYSKEYUP);
        if (k->vkCode == VK_RETURN) g_enterDown = down ? true : (up ? false : g_enterDown);
        if (k->vkCode == VK_BACK)   g_backDown  = down ? true : (up ? false : g_backDown);
        if (down && g_enterDown && g_backDown && !g_comboFired) {
            g_comboFired = true;
            Log("hook: Enter+Backspace combo -> toggle freeze");
            PostMessage(g_mainWnd, WM_TOGGLE_FREEZE, 0, 0);
            return 1;  // swallow the second key so it doesn't act on the app below
        }
        if (!g_enterDown || !g_backDown) g_comboFired = false;
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

// ---------- main hidden window / tray ----------
static void AddTray(HWND h) {
    NOTIFYICONDATAW nid{}; nid.cbSize = sizeof(nid);
    nid.hWnd = h; nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APPICON));
    if (!nid.hIcon) {
        Log("tray: app icon resource missing, falling back to default");
        nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    } else {
        Log("tray: app icon loaded from resource");
    }
    wcscpy(nid.szTip, L"冻结标注 · Enter+Backspace 冻结屏幕");
    BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
    Log("tray: add ok=%d gle=%lu", ok, GetLastError());
}
static void RemoveTray(HWND h) {
    NOTIFYICONDATAW nid{}; nid.cbSize = sizeof(nid);
    nid.hWnd = h; nid.uID = TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void LogToolbarState(const char* tag) {
    if (!g_toolbarWnd) { Log("%s: toolbar hwnd=NULL", tag); return; }
    RECT r; GetWindowRect(g_toolbarWnd, &r);
    HRGN probe = CreateRectRgn(0, 0, 0, 0);
    int rt = GetWindowRgn(g_toolbarWnd, probe);
    DeleteObject(probe);
    Log("%s: visible=%d rect=%ld,%ld %ldx%ld style=%08lx exstyle=%08lx rgnType=%d plain=%d",
        tag, IsWindowVisible(g_toolbarWnd) ? 1 : 0, r.left, r.top,
        r.right - r.left, r.bottom - r.top,
        (unsigned long)GetWindowLongW(g_toolbarWnd, GWL_STYLE),
        (unsigned long)GetWindowLongW(g_toolbarWnd, GWL_EXSTYLE),
        rt, g_tbPlainFallback ? 1 : 0);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_TOGGLE_FREEZE:
        if (g_frozen) { Log("toggle: exit via hotkey"); ExitFreeze(); }
        else EnterFreeze();
        return 0;
    case WM_EXIT_FREEZE:
        ExitFreeze();
        return 0;
    case WM_TAKEOVER_EXIT:
        Log("takeover: newer instance starting -> this instance exits");
        DestroyWindow(h);
        return 0;
    case WM_TIMER:
        if (wp == 9) {
            KillTimer(h, 9);
            if (g_frozen && g_toolbarWnd && (g_dragging || g_tbVisState != 0)) {
                // The capsule is hidden BY DESIGN (user mid-marking, or a
                // fade is running). The watchdog used to mistake this for
                // the vanishing fault and strip the window region (stage
                // B), leaving square corners for the rest of the session -
                // the "occasionally turns square after several marks" bug.
                if (g_wdRetry < 5) {
                    ++g_wdRetry;
                    Log("watchdog: deferred (drawing/fading), retry %d", g_wdRetry);
                    SetTimer(h, 9, 1000, nullptr);
                } else Log("watchdog: skipped (drawing/fading)");
            } else if (g_frozen && g_toolbarWnd) {
                LogToolbarState("watchdog1");
                RECT r; GetWindowRect(g_toolbarWnd, &r);
                bool off = r.right < g_vx || r.left > g_vx + g_vw ||
                           r.bottom < g_vy || r.top > g_vy + g_vh;
                HRGN probe = CreateRectRgn(0, 0, 0, 0);
                int rt = GetWindowRgn(g_toolbarWnd, probe);
                DeleteObject(probe);
                if (!IsWindowVisible(g_toolbarWnd) || off || rt == 1) {
                    // Stage B: drop the region entirely and re-show on top.
                    Log("watchdog: stage B (remove region + reshow)");
                    SetWindowRgn(g_toolbarWnd, nullptr, TRUE);
                    int w = g_toolbarExpanded ? PANEL_W : BALL_W;
                    int hh = g_toolbarExpanded ? PANEL_H : BALL_H;
                    SetWindowPos(g_toolbarWnd, HWND_TOPMOST,
                                 g_vx + (g_vw - w) / 2, g_vy + 14, w, hh, SWP_SHOWWINDOW);
                    SetTimer(h, 10, 900, nullptr);
                }
            }
        } else if (wp == 10) {
            KillTimer(h, 10);
            if (g_frozen && g_toolbarWnd && (g_dragging || g_tbVisState != 0)) {
                Log("watchdog2: skipped (drawing/fading - hidden by design)");
            } else if (g_frozen && g_toolbarWnd) {
                LogToolbarState("watchdog2");
                if (!IsWindowVisible(g_toolbarWnd)) {
                    // Stage C: rebuild as a plain no-region bar, simplest paint.
                    Log("watchdog: stage C (plain fallback rebuild)");
                    g_tbPlainFallback = true;
                    CreateToolbarWindow(false);
                    LogToolbarState("watchdog2-afterC");
                }
            }
        }
        return 0;
    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, MENU_FREEZE, g_frozen ? L"退出冻结" : L"冻结屏幕");
            AppendMenuW(menu, MF_STRING | (g_autoStart ? MF_CHECKED : 0), MENU_AUTOSTART, L"开机自启动");
            AppendMenuW(menu, MF_STRING, MENU_EXIT, L"退出程序");
            SetForegroundWindow(h);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(menu);
        } else if (lp == WM_LBUTTONDBLCLK) {
            PostMessage(h, WM_TOGGLE_FREEZE, 0, 0);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == MENU_FREEZE) PostMessage(h, WM_TOGGLE_FREEZE, 0, 0);
        if (LOWORD(wp) == MENU_AUTOSTART) SetAutoStart(!g_autoStart);
        if (LOWORD(wp) == MENU_EXIT)   DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        RemoveTray(h);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    g_inst = inst;
    // exe dir for log
    GetModuleFileNameW(nullptr, g_exeDir, MAX_PATH);
    wchar_t* slash = wcsrchr(g_exeDir, L'\\');
    if (slash) *slash = 0;
    Log("=== FreezeMark start ===");
    timeBeginPeriod(1);  // finer timer resolution for the morph animation

    // Single instance WITH TAKEOVER (v1.4): a new build must not be silently
    // blocked by a forgotten old resident process (that made earlier fixes
    // look broken). The new instance asks the old one to exit, waits for
    // the named mutex to free up, then takes its place.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"FreezeMark_SingleInstance_v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Log("start: old instance found -> requesting takeover");
        CloseHandle(mutex); mutex = nullptr;
        HWND old = FindWindowW(L"FreezeMarkMain", nullptr);
        if (old) PostMessage(old, WM_TAKEOVER_EXIT, 0, 0);
        else Log("takeover: old main window not found");
        for (int i = 0; i < 40 && !mutex; i++) {
            Sleep(50);
            if (i == 12) {
                // Old builds (< v1.4) don't know the takeover message. After a
                // 600 ms grace period, end our own old process by name - this
                // is the user's own utility being upgraded, nothing else.
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snap != INVALID_HANDLE_VALUE) {
                    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
                    if (Process32FirstW(snap, &pe)) do {
                        if (_wcsicmp(pe.szExeFile, L"FreezeMark.exe") == 0 &&
                            pe.th32ProcessID != GetCurrentProcessId()) {
                            HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                            if (hp) {
                                BOOL tk = TerminateProcess(hp, 0);
                                Log("takeover: terminated old pid=%lu ok=%d", pe.th32ProcessID, tk);
                                CloseHandle(hp);
                            }
                        }
                    } while (Process32NextW(snap, &pe));
                    CloseHandle(snap);
                }
            }
            HANDLE m2 = CreateMutexW(nullptr, TRUE, L"FreezeMark_SingleInstance_v1");
            if (GetLastError() != ERROR_ALREADY_EXISTS) mutex = m2;
            else CloseHandle(m2);
        }
        if (!mutex) {
            Log("start: takeover timed out, exiting");
            MessageBoxW(nullptr, L"旧版冻结标注未能退出，请在任务管理器里结束 FreezeMark.exe 后再打开新版。", L"冻结标注", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        Log("takeover: old instance gone, this instance takes over");
    }
    (void)mutex;

    InitAutoStart();

    // DPI awareness (best effort, so multi-monitor / scaling captures are 1:1)
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL (WINAPI* Fn)(HANDLE);
        auto fn = (Fn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn) { BOOL ok = fn((HANDLE)-4); Log("dpi: per-monitor-v2 ok=%d", ok); }
        else { SetProcessDPIAware(); Log("dpi: fallback SetProcessDPIAware"); }
    }

    GdiplusStartupInput gsi;
    ULONG_PTR gtok = 0;
    Status gst = GdiplusStartup(&gtok, &gsi, nullptr);
    Log("gdiplus: startup status=%d", (int)gst);
    if (gst != Ok) {
        MessageBoxW(nullptr, L"GDI+ 初始化失败，程序无法运行。", L"冻结标注", MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainProc; wc.hInstance = inst; wc.lpszClassName = L"FreezeMarkMain";
    RegisterClassW(&wc);
    wc.lpfnWndProc = FreezeProc; wc.lpszClassName = L"FreezeMarkFreeze";
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    RegisterClassW(&wc);
    wc.lpfnWndProc = ToolbarProc; wc.lpszClassName = L"FreezeMarkToolbar";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    g_mainWnd = CreateWindowW(L"FreezeMarkMain", L"FreezeMark", 0, 0, 0, 0, 0,
                              nullptr, nullptr, inst, nullptr);
    if (!g_mainWnd) { Log("main: window create failed gle=%lu", GetLastError()); return 1; }
    Log("main: hidden window=%p", (void*)g_mainWnd);

    AddTray(g_mainWnd);

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KbHook, inst, 0);
    Log("hook: install=%p gle=%lu", (void*)g_hook, GetLastError());
    if (!g_hook) {
        MessageBoxW(nullptr, L"键盘钩子安装失败，Enter+Backspace 将无法触发。\n看 exe 旁的 freezemark_debug.log。", L"冻结标注", MB_OK | MB_ICONERROR);
        return 1;
    }

    Log("ready: waiting for Enter+Backspace");
    Log("resident: blocking GetMessage loop, no polling, no idle timers (0%% CPU at rest)");
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (g_hook) UnhookWindowsHookEx(g_hook);
    ExitFreeze();
    GdiplusShutdown(gtok);
    timeEndPeriod(1);
    Log("=== FreezeMark exit ===");
    return 0;
}
