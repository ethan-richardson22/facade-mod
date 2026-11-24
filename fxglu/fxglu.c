/*
 * FXGLU.dll - display fix for Facade's animEngineDLL.dll
 *
 * Loaded because animEngineDLL.dll's "GLU32.dll" import is renamed to "FXGLU.dll".
 * The three GLU functions are forwarded to the real glu32 (see fxglu.def).
 *
 * The engine always lays out a fixed virtual canvas: the client area of the original
 * 1024x768 window (1016x741), drawn through a 1016x1016 square viewport anchored at the
 * bottom-left (top part cropped). Mouse picking, text placement etc. all assume that
 * canvas, so we leave the engine's idea of it alone and only:
 *   - scale + center the real GL viewport to fill the actual window (native-res raster)
 *   - report the original viewport back through glGetIntegerv(GL_VIEWPORT)
 *   - map mouse coordinates (WM_MOUSEMOVE, SetCursorPos, ClipCursor) between the two
 *   - create a normal resizable window, with F11 / Alt+Enter borderless fullscreen
 * Quality-of-life extras (all configurable in fxglu.ini):
 *   - MSAA: pick a multisampled pixel format via wglChoosePixelFormatARB
 *   - anisotropic texture filtering
 *   - cursors scaled for high-DPI displays
 *   - optional automatic skip of the title sequence
 *   - Java stdout/stderr captured to a timestamped log, overridable java path/args
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <intrin.h>
#include <mmsystem.h>
#pragma intrinsic(_ReturnAddress)

/* the GLU functions animEngineDLL imports, forwarded straight to the system glu32.dll */
#pragma comment(linker, "/export:gluBuild1DMipmaps=glu32.gluBuild1DMipmaps")
#pragma comment(linker, "/export:gluBuild2DMipmaps=glu32.gluBuild2DMipmaps")
#pragma comment(linker, "/export:gluProject=glu32.gluProject")

#define VIRT_W 1016   /* engine's assumed client width  */
#define VIRT_H 741    /* engine's assumed client height */

static char g_dir[MAX_PATH];
static FILE *g_log;
static HWND g_hwnd;
static WNDPROC g_origProc;
static int g_fullscreen;
static WINDOWPLACEMENT g_savedPlacement = { sizeof(WINDOWPLACEMENT) };
static GLint g_virtViewport[4] = { 0, 0, VIRT_W, VIRT_W };
static int g_cfgW = 1920, g_cfgH = 1080, g_cfgFullscreen = 0, g_cfgLog = 1;
static int g_cfgMsaa = 8, g_cfgAniso = 16, g_cfgCursorScale = 0, g_cfgSkipIntro = 0;
static int g_cfgWide = 1, g_cfgSubs = 1, g_cfgFastAnswers = 1;
#define ENG_SUBTITLES 0x10979471   /* byte: subtitles on/off (only the menu toggle writes it; starts 0) */
#define ENG_MOUSE_X   0x1083f5e8   /* engine's mouse position in canvas pixels (WM_MOUSEMOVE handler) */
#define ENG_MOUSE_Y   0x1083f5ec
static HINSTANCE g_inst;
static BYTE *g_eng;            /* animEngineDLL.dll load address (preferred base 0x10000000) */
#define ENG(va) (g_eng + ((va) - 0x10000000))

static void dbg(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* ---- real <-> virtual mapping ------------------------------------------------ */

/* Canvas-to-window mapping. The canvas is scaled by s and centred (ox, oy; oy from the bottom).
 * wide = widescreen mode: the window is wider than the canvas, so instead of black bars the GL
 * viewport covers the whole width and glOrtho's x range is extended to match; canvas pixel
 * coordinates then simply continue past 0 and VIRT_W at the sides. */
typedef struct { double s, ox, oy; int W, H, wide; } Map;

static int get_map(Map *m)
{
    RECT rc;
    if (!g_hwnd || !GetClientRect(g_hwnd, &rc)) return 0;
    m->W = rc.right; m->H = rc.bottom;
    if (m->W <= 0 || m->H <= 0) return 0;
    {
        double sx = (double)m->W / VIRT_W, sy = (double)m->H / VIRT_H;
        m->s = sx < sy ? sx : sy;
    }
    m->ox = (m->W - VIRT_W * m->s) / 2.0;
    m->oy = (m->H - VIRT_H * m->s) / 2.0;
    m->wide = g_cfgWide && m->ox > 0.5;
    return 1;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* real client (top-left origin) -> virtual client (top-left origin); in widescreen mode x may
 * run past the canvas edges (negative / >= VIRT_W) */
static void real_to_virt(const Map *m, int rx, int ry, int *vx, int *vy)
{
    double gx = (rx - m->ox) / m->s;
    double gyb = ((m->H - ry) - m->oy) / m->s;      /* from bottom, virtual */
    int x = (int)floor(gx + 0.5);
    *vx = m->wide ? x : clampi(x, 0, VIRT_W - 1);
    *vy = clampi((int)floor(VIRT_H - gyb + 0.5), 0, VIRT_H - 1);
}

/* virtual client -> real client */
static void virt_to_real(const Map *m, int vx, int vy, int *rx, int *ry)
{
    *rx = (int)(m->ox + vx * m->s + 0.5);
    *ry = (int)(m->H - (m->oy + (VIRT_H - vy) * m->s) + 0.5);
}

/* ---- GL hooks ---------------------------------------------------------------- */

static void (WINAPI *real_glViewport)(GLint, GLint, GLsizei, GLsizei);
static void (WINAPI *real_glGetIntegerv)(GLenum, GLint *);
static void (WINAPI *real_glLineWidth)(GLfloat);

static void WINAPI hk_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    Map m;
    static int logged;
    g_virtViewport[0] = x; g_virtViewport[1] = y; g_virtViewport[2] = w; g_virtViewport[3] = h;
    if (!get_map(&m)) { real_glViewport(x, y, w, h); return; }
    if (logged < 3) {
        logged++;
        dbg("glViewport(%d,%d,%d,%d) client %dx%d -> scale %.3f off %.1f,%.1f", x, y, w, h,
             m.W, m.H, m.s, m.ox, m.oy);
    }
    if (m.wide)   /* whole width; glOrtho below widens the x range to match */
        real_glViewport(0, (GLint)(m.oy + y * m.s + 0.5), m.W, (GLsizei)(h * m.s + 0.5));
    else
        real_glViewport((GLint)(m.ox + x * m.s + 0.5), (GLint)(m.oy + y * m.s + 0.5),
                        (GLsizei)(w * m.s + 0.5), (GLsizei)(h * m.s + 0.5));
}

static void WINAPI hk_glGetIntegerv(GLenum pname, GLint *p)
{
    if (pname == GL_VIEWPORT) { memcpy(p, g_virtViewport, sizeof g_virtViewport); return; }
    real_glGetIntegerv(pname, p);
}

/* The engine's only glOrtho is in its reshape function: glOrtho(-100,100,-100,100,0,2000) for the
 * 1016x1016 canvas viewport. In widescreen mode the x range is extended by exactly the canvas
 * pixels the wider viewport adds on each side, so nothing is stretched. Matrix read-backs still
 * get the original projection, keeping the engine's own maths (gluProject etc.) unchanged. */
static void (WINAPI *real_glOrtho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
static void (WINAPI *real_glGetDoublev)(GLenum, GLdouble *);
static void (WINAPI *real_glGetFloatv)(GLenum, GLfloat *);
static GLdouble g_ortho[6] = { -100, 100, -100, 100, 0, 2000 };
static int g_orthoWidened;

static void WINAPI hk_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    Map m;
    static int logged;
    g_ortho[0] = l; g_ortho[1] = r; g_ortho[2] = b; g_ortho[3] = t; g_ortho[4] = n; g_ortho[5] = f;
    g_orthoWidened = 0;
    if (get_map(&m) && m.wide && g_virtViewport[2] > 0) {
        double per = (r - l) / g_virtViewport[2];                 /* ortho units per canvas pixel */
        double l2 = l + (-m.ox / m.s) * per, r2 = l + ((m.W - m.ox) / m.s) * per;
        if (logged < 3) { logged++; dbg("glOrtho x %.1f..%.1f -> %.1f..%.1f (widescreen)", l, r, l2, r2); }
        g_orthoWidened = 1;
        real_glOrtho(l2, r2, b, t, n, f);
        return;
    }
    real_glOrtho(l, r, b, t, n, f);
}

static void ortho_matrix(double *o)   /* column-major, as glGet returns it */
{
    double l = g_ortho[0], r = g_ortho[1], b = g_ortho[2], t = g_ortho[3], n = g_ortho[4], f = g_ortho[5];
    memset(o, 0, 16 * sizeof *o);
    o[0] = 2 / (r - l); o[5] = 2 / (t - b); o[10] = -2 / (f - n); o[15] = 1;
    o[12] = -(r + l) / (r - l); o[13] = -(t + b) / (t - b); o[14] = -(f + n) / (f - n);
}

static void WINAPI hk_glGetDoublev(GLenum pname, GLdouble *p)
{
    if (pname == GL_PROJECTION_MATRIX && g_orthoWidened) { ortho_matrix(p); return; }
    real_glGetDoublev(pname, p);
}

static void WINAPI hk_glGetFloatv(GLenum pname, GLfloat *p)
{
    if (pname == GL_PROJECTION_MATRIX && g_orthoWidened) {
        double o[16]; int i; ortho_matrix(o);
        for (i = 0; i < 16; i++) p[i] = (GLfloat)o[i];
        return;
    }
    real_glGetFloatv(pname, p);
}

static void WINAPI hk_glLineWidth(GLfloat w)
{
    Map m;
    real_glLineWidth(get_map(&m) && m.s > 1.0 ? (GLfloat)(w * m.s) : w);
}

/* anisotropic filtering: whenever the engine sets a texture's minification filter */
#define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
static void (WINAPI *real_glTexParameterf)(GLenum, GLenum, GLfloat);

static void WINAPI hk_glTexParameterf(GLenum target, GLenum pname, GLfloat v)
{
    static GLfloat maxAniso = -1.0f;
    real_glTexParameterf(target, pname, v);
    if (pname != GL_TEXTURE_MIN_FILTER || g_cfgAniso < 2) return;
    if (maxAniso < 0) {
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        maxAniso = 0;
        if (ext && strstr(ext, "GL_EXT_texture_filter_anisotropic"))
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso);
        dbg("anisotropic filtering: driver max %.0f, using %d", maxAniso,
             maxAniso > 1 ? (int)min((GLfloat)g_cfgAniso, maxAniso) : 0);
    }
    if (maxAniso > 1)
        real_glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, min((GLfloat)g_cfgAniso, maxAniso));
}

/* ---- MSAA pixel format --------------------------------------------------------- */

#define WGL_DRAW_TO_WINDOW_ARB 0x2001
#define WGL_SUPPORT_OPENGL_ARB 0x2010
#define WGL_DOUBLE_BUFFER_ARB  0x2011
#define WGL_PIXEL_TYPE_ARB     0x2013
#define WGL_TYPE_RGBA_ARB      0x202B
#define WGL_COLOR_BITS_ARB     0x2014
#define WGL_DEPTH_BITS_ARB     0x2022
#define WGL_STENCIL_BITS_ARB   0x2023
#define WGL_SAMPLE_BUFFERS_ARB 0x2041
#define WGL_SAMPLES_ARB        0x2042
typedef BOOL (WINAPI *PFNCHOOSEPF)(HDC, const int *, const FLOAT *, UINT, int *, UINT *);
static int (WINAPI *real_ChoosePixelFormat)(HDC, const PIXELFORMATDESCRIPTOR *);

static int WINAPI hk_ChoosePixelFormat(HDC hdc, const PIXELFORMATDESCRIPTOR *pfd)
{
    int fmt = real_ChoosePixelFormat(hdc, pfd), best = 0, samples;
    WNDCLASSA wc = { 0 };
    HWND dw; HDC ddc; HGLRC rc; PIXELFORMATDESCRIPTOR dp = *pfd;
    PFNCHOOSEPF choose;
    if (g_cfgMsaa < 2) return fmt;

    /* wglChoosePixelFormatARB needs a current GL context: make a throwaway one */
    wc.lpfnWndProc = DefWindowProcA; wc.hInstance = g_inst; wc.lpszClassName = "fxglu_dummy";
    RegisterClassA(&wc);
    dw = CreateWindowA("fxglu_dummy", "", WS_POPUP, 0, 0, 1, 1, NULL, NULL, g_inst, NULL);
    ddc = GetDC(dw);
    SetPixelFormat(ddc, real_ChoosePixelFormat(ddc, &dp), &dp);
    rc = wglCreateContext(ddc);
    wglMakeCurrent(ddc, rc);
    choose = (PFNCHOOSEPF)wglGetProcAddress("wglChoosePixelFormatARB");
    for (samples = g_cfgMsaa; choose && samples >= 2 && !best; samples /= 2) {
        int attr[] = {
            WGL_DRAW_TO_WINDOW_ARB, 1, WGL_SUPPORT_OPENGL_ARB, 1,
            WGL_DOUBLE_BUFFER_ARB, (pfd->dwFlags & PFD_DOUBLEBUFFER) ? 1 : 0,
            WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB, WGL_COLOR_BITS_ARB, pfd->cColorBits,
            WGL_DEPTH_BITS_ARB, min(pfd->cDepthBits, 24), WGL_STENCIL_BITS_ARB, pfd->cStencilBits,
            WGL_SAMPLE_BUFFERS_ARB, 1, WGL_SAMPLES_ARB, samples, 0 };
        int f; UINT n = 0;
        if (choose(hdc, attr, NULL, 1, &f, &n) && n) {
            best = f;
            dbg("MSAA: %dx pixel format %d (engine's default would be %d)", samples, f, fmt);
        }
    }
    if (!choose) dbg("MSAA: wglChoosePixelFormatARB not available");
    else if (!best) dbg("MSAA: no multisampled format available");
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(dw, ddc);
    DestroyWindow(dw);
    UnregisterClassA("fxglu_dummy", g_inst);
    return best ? best : fmt;
}

/* ---- intro skip ------------------------------------------------------------------ */
/* The title-sequence state machine (engine fn 0x1009e800, reached via the jump-table thunk at
 * 0x10002176) jumps to its last state (9) whenever the "key typed" byte at 0x10978400 is non-zero.
 * We set that byte for one call once the sequence is running (state >= 1; state 0 is a one-frame
 * camera setup a real player can't interrupt), exactly as if the player had pressed a key. */

#define ENG_INTRO_THUNK 0x10002176
#define ENG_INTRO_FN    0x1009e800
#define ENG_INTRO_STATE 0x101c7614
#define ENG_KEY_TYPED   0x10978400
static void (__cdecl *real_intro)(void);

static void __cdecl hk_intro(void)
{
    static int done;
    volatile int *state = (volatile int *)ENG(ENG_INTRO_STATE);
    volatile char *key = (volatile char *)ENG(ENG_KEY_TYPED);
    if (!done && *state >= 1 && *state < 9) {
        char old = *key;
        done = 1;
        *key = ' ';
        real_intro();
        *key = old;
        dbg("title sequence skipped (state now %d)", *state);
        return;
    }
    real_intro();
}

/* redirect one of the engine's incremental-link jump thunks (E9 rel32) to our function */
static int hook_thunk(DWORD thunkVa, DWORD fnVa, void *hook, void **orig)
{
    BYTE *t = ENG(thunkVa);
    DWORD old;
    if (t[0] != 0xE9 || t + 5 + *(LONG *)(t + 1) != ENG(fnVa)) {
        dbg("FAILED to hook thunk %08lx (unexpected bytes)", thunkVa);
        return 0;
    }
    *orig = ENG(fnVa);
    VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old);
    *(LONG *)(t + 1) = (LONG)((BYTE *)hook - (t + 5));
    VirtualProtect(t, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    dbg("hooked engine fn %08lx via thunk %08lx", fnVa, thunkVa);
    return 1;
}

/* ---- cursor hooks ------------------------------------------------------------ */

static HANDLE (WINAPI *real_LoadImageA)(HINSTANCE, LPCSTR, UINT, int, int, UINT);

/* the action cursors (hand, kiss, hug, ...) are .ico files loaded at a fixed 48x48 */
static HANDLE WINAPI hk_LoadImageA(HINSTANCE inst, LPCSTR name, UINT type, int cx, int cy, UINT fl)
{
    static int pct = -1;
    if ((type == IMAGE_ICON || type == IMAGE_CURSOR) && cx > 0 && cy > 0) {
        if (pct < 0) {
            HDC dc = GetDC(NULL);
            pct = g_cfgCursorScale > 0 ? g_cfgCursorScale : GetDeviceCaps(dc, LOGPIXELSX) * 100 / 96;
            ReleaseDC(NULL, dc);
            dbg("cursor scale %d%%", pct);
        }
        cx = cx * pct / 100; cy = cy * pct / 100;
    }
    return real_LoadImageA(inst, name, type, cx, cy, fl);
}


static BOOL (WINAPI *real_SetCursorPos)(int, int);
static BOOL (WINAPI *real_ClipCursor)(const RECT *);
static HWND (WINAPI *real_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
                                           HWND, HMENU, HINSTANCE, LPVOID);

static BOOL WINAPI hk_SetCursorPos(int x, int y)
{
    Map m; POINT p;
    static int logged;
    if (!get_map(&m)) return real_SetCursorPos(x, y);
    virt_to_real(&m, x, y, (int *)&p.x, (int *)&p.y);
    ClientToScreen(g_hwnd, &p);
    if (logged < 5) { logged++; dbg("SetCursorPos(%d,%d) -> screen %ld,%ld", x, y, p.x, p.y); }
    return real_SetCursorPos(p.x, p.y);
}

static BOOL WINAPI hk_ClipCursor(const RECT *r)
{
    RECT rc; POINT a, b;
    static int logged;
    if (!r || !g_hwnd || !GetClientRect(g_hwnd, &rc)) return real_ClipCursor(r);
    a.x = rc.left; a.y = rc.top; b.x = rc.right; b.y = rc.bottom;
    ClientToScreen(g_hwnd, &a); ClientToScreen(g_hwnd, &b);
    SetRect(&rc, a.x, a.y, b.x, b.y);
    if (logged < 5) {
        logged++;
        dbg("ClipCursor(%ld,%ld,%ld,%ld) -> %ld,%ld,%ld,%ld", r->left, r->top, r->right, r->bottom,
             rc.left, rc.top, rc.right, rc.bottom);
    }
    return real_ClipCursor(&rc);
}

/* ---- window ------------------------------------------------------------------ */

static void set_fullscreen(HWND h, int on)
{
    DWORD style = GetWindowLongA(h, GWL_STYLE);
    if (on == g_fullscreen) return;
    if (on) {
        MONITORINFO mi = { sizeof mi };
        GetWindowPlacement(h, &g_savedPlacement);
        GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongA(h, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongA(h, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &g_savedPlacement);
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = on;
    dbg("fullscreen %s", on ? "on" : "off");
}

static LRESULT CALLBACK sub_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEMOVE: {
        Map m; int vx, vy;
        if (get_map(&m)) {
            LRESULT res;
            real_to_virt(&m, (short)LOWORD(lp), (short)HIWORD(lp), &vx, &vy);
            res = CallWindowProcA(g_origProc, h, msg, wp, MAKELPARAM(clampi(vx, 0, VIRT_W - 1), vy));
            /* the engine keeps the position as signed ints; a 16-bit lParam can't carry the
             * negative x of the widescreen side areas, so store the real value directly */
            *(volatile int *)ENG(ENG_MOUSE_X) = vx;
            *(volatile int *)ENG(ENG_MOUSE_Y) = vy;
            return res;
        }
        break;
    }
    case WM_KEYDOWN:
        if (wp == VK_F11) { set_fullscreen(h, !g_fullscreen); return 0; }
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) { set_fullscreen(h, !g_fullscreen); return 0; }
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0;   /* swallow the Alt+Enter beep */
        break;
    case WM_ERASEBKGND:
        return 1;                        /* GL paints everything; avoid flicker on resize */
    }
    return CallWindowProcA(g_origProc, h, msg, wp, lp);
}

static HWND WINAPI hk_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y,
                                      int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID p)
{
    HWND hw;
    if (parent || (style & WS_CHILD) || g_hwnd)
        return real_CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, p);

    {   /* main game window: normal overlapped window with the requested client size */
        RECT r = { 0, 0, g_cfgW, g_cfgH };
        RECT wa;
        DWORD st = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        AdjustWindowRectEx(&r, st, FALSE, ex);
        w = r.right - r.left; h = r.bottom - r.top;
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        x = wa.left + max(0, ((wa.right - wa.left) - w) / 2);
        y = wa.top + max(0, ((wa.bottom - wa.top) - h) / 2);
        dbg("CreateWindowExA: style %08lx %dx%d -> %08lx client %dx%d at %d,%d",
             style, 1024, 768, st, g_cfgW, g_cfgH, x, y);
        style = st;
    }
    hw = real_CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, p);
    if (g_cfgFastAnswers)   /* 1 ms Sleep granularity for the sliced frame sleep (engine process only) */
        timeBeginPeriod(1);
    if (g_cfgSubs) {   /* subtitles on by default; the game's own menu toggle still works */
        *(volatile BYTE *)ENG(ENG_SUBTITLES) = 1;
        dbg("subtitles on by default");
    }
    if (hw) {
        g_hwnd = hw;
        g_origProc = (WNDPROC)SetWindowLongPtrA(hw, GWLP_WNDPROC, (LONG_PTR)sub_proc);
        if (g_cfgFullscreen) set_fullscreen(hw, 1);
    }
    return hw;
}

/* ---- Java launch -------------------------------------------------------------- */
/* The engine starts the Java side with ShellExecuteA("..\..\j2re1.4.2_06\bin\java.exe", args),
 * so its console output is lost. We launch it ourselves with stdout/stderr piped into a
 * timestamped log, and allow overriding the java path / arguments from fxglu.ini. */

static HINSTANCE (WINAPI *real_ShellExecuteA)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
static char g_javaPath[MAX_PATH], g_javaArgs[1024];
static DWORD g_launchTick;

static DWORD WINAPI java_pipe_thread(LPVOID pipe)
{
    char path[MAX_PATH], buf[4096], line[8192];
    int len = 0;
    DWORD n, i;
    FILE *f;
    wsprintfA(path, "%s\\fxglu_java_out.log", g_dir);
    if (!(f = fopen(path, "w"))) return 0;
    while (ReadFile((HANDLE)pipe, buf, sizeof buf, &n, NULL) && n) {
        for (i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\r') continue;
            if (ch != '\n' && len < (int)sizeof line - 1) { line[len++] = ch; continue; }
            line[len] = 0;
            fprintf(f, "[%7.2fs] %s\n", (GetTickCount() - g_launchTick) / 1000.0, line);
            fflush(f);
            len = 0;
        }
    }
    fclose(f);
    CloseHandle((HANDLE)pipe);
    return 0;
}

static HINSTANCE WINAPI hk_ShellExecuteA(HWND hw, LPCSTR op, LPCSTR file, LPCSTR params,
                                         LPCSTR dir, INT show)
{
    char cmd[4096], exe[MAX_PATH];
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    size_t fl = file ? strlen(file) : 0;

    if (fl < 8 || lstrcmpiA(file + fl - 8, "java.exe"))
        return real_ShellExecuteA(hw, op, file, params, dir, show);   /* e.g. the tip-jar URL */

    lstrcpynA(exe, g_javaPath[0] ? g_javaPath : file, MAX_PATH);
    wsprintfA(cmd, "\"%s\" %s", exe, g_javaArgs[0] ? g_javaArgs : (params ? params : ""));
    dbg("java launch: %s  (cwd %s)", cmd, dir ? dir : "(current)");

    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        dbg("CreatePipe failed (%lu), falling back to ShellExecute", GetLastError());
        return real_ShellExecuteA(hw, op, file, params, dir, show);
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;
    g_launchTick = GetTickCount();
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) {
        dbg("CreateProcess failed (%lu), falling back to ShellExecute", GetLastError());
        CloseHandle(rd); CloseHandle(wr);
        return real_ShellExecuteA(hw, op, file, params, dir, show);
    }
    CloseHandle(wr);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(CreateThread(NULL, 0, java_pipe_thread, rd, 0, NULL));
    dbg("java started, pid %lu", pi.dwProcessId);
    return (HINSTANCE)42;   /* > 32 means success to ShellExecute callers */
}

/* ---- performance statistics ([debug] perfstats=1) ------------------------------- */
/* Java-side AnimAPI queries post a request and wait (WaitForSingleObject at 0x10003c40) for an
 * "answer event" that the engine only sets while processing requests once per frame; the engine
 * sleeps ~20 ms per frame (Sleep at 0x100b2b5c). These counters show how much that costs. */

#define ENG_RET_ANSWER_WAIT 0x10003c46   /* return address of the answer-event wait */
#define ENG_RET_LOCK_WAIT   0x10003b16   /* return address of the request-buffer mutex wait */
#define ENG_FRAME_COUNTER   0x1083f62c
enum { ST_ANSWER, ST_LOCK, ST_OTHER, ST_SLEEP, ST_N };
static const char *g_stName[ST_N] = { "answer-wait", "mutex-wait", "other-wait", "engine-sleep" };
static volatile LONG64 g_stUs[ST_N], g_stMax[ST_N];
static volatile LONG g_stCount[ST_N];
static LARGE_INTEGER g_qpf;
static int g_cfgPerf;

static void st_add(int k, LONG64 us)
{
    LONG64 m;
    InterlockedIncrement(&g_stCount[k]);
    InterlockedAdd64(&g_stUs[k], us);
    while ((m = g_stMax[k]) < us && InterlockedCompareExchange64(&g_stMax[k], us, m) != m) {}
}

static LONG64 qpc_us(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * 1000000 / g_qpf.QuadPart; }

static DWORD (WINAPI *real_WaitForSingleObject)(HANDLE, DWORD);
static VOID (WINAPI *real_Sleep)(DWORD);

static DWORD WINAPI hk_WaitForSingleObject(HANDLE h, DWORD ms)
{
    BYTE *ret = (BYTE *)_ReturnAddress();
    LONG64 t0 = qpc_us();
    DWORD r = real_WaitForSingleObject(h, ms);
    st_add(ret == ENG(ENG_RET_ANSWER_WAIT) ? ST_ANSWER : ret == ENG(ENG_RET_LOCK_WAIT) ? ST_LOCK : ST_OTHER,
           qpc_us() - t0);
    return r;
}

/* Fast answers ([game] fast_answers=1): the engine's frame loop calls ProcessAblRequests()
 * (0x100b32c0, cdecl, one byte arg) and right after it Sleep(~20 ms) (call at 0x100b2b5c). Every
 * query the Java side makes during that sleep waits for the next frame (~18 ms measured). Instead
 * we sleep to the same deadline in 1 ms slices and service the request buffer between slices:
 * frame pacing is unchanged, Java's queries are answered within ~1 ms. */
#define ENG_RET_FRAME_SLEEP 0x100b2b62
#define ENG_PROCESS_ABL_REQ 0x100b32c0
static void (__cdecl *g_processAblRequests)(int);

static VOID WINAPI hk_Sleep(DWORD ms)
{
    LONG64 t0 = qpc_us();
    if (g_cfgFastAnswers && ms > 1 && (BYTE *)_ReturnAddress() == ENG(ENG_RET_FRAME_SLEEP)) {
        LONG64 deadline = t0 + (LONG64)ms * 1000;
        for (;;) {
            LONG64 left = deadline - qpc_us();
            if (left <= 0) break;
            real_Sleep(left >= 1500 ? 1 : 0);
            g_processAblRequests(0);
        }
    } else
        real_Sleep(ms);
    if (g_cfgPerf) st_add(ST_SLEEP, qpc_us() - t0);
}

static DWORD WINAPI perf_thread(LPVOID unused)
{
    int k;
    LONG lastFrame = *(volatile LONG *)ENG(ENG_FRAME_COUNTER);
    (void)unused;
    for (;;) {
        Sleep(10000);   /* our own Sleep import, not the hooked engine one */
        {
            LONG fr = *(volatile LONG *)ENG(ENG_FRAME_COUNTER);
            dbg("[perf] ---- last 10 s: engine frames %ld", fr - lastFrame);
            lastFrame = fr;
        }
        for (k = 0; k < ST_N; k++) {
            LONG n = InterlockedExchange(&g_stCount[k], 0);
            LONG64 us = InterlockedExchange64(&g_stUs[k], 0), mx = InterlockedExchange64(&g_stMax[k], 0);
            if (n)
                dbg("[perf] %-12s n=%6ld  total %7.1f ms (%5.1f%% of 10 s)  avg %6.2f ms  max %7.2f ms",
                    g_stName[k], n, us / 1000.0, us / 100000.0, us / 1000.0 / n, mx / 1000.0);
        }
    }
}

/* ---- IAT patching ------------------------------------------------------------ */

static int hook_iat(HMODULE mod, const char *dll, const char *fn, void *hook, void **orig)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd.VirtualAddress);
    for (; d->Name; d++) {
        IMAGE_THUNK_DATA *names, *iat;
        if (lstrcmpiA((char *)(base + d->Name), dll)) continue;
        names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
        iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, fn))
                continue;
            *orig = (void *)iat->u1.Function;
            VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            iat->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
            dbg("hooked %s!%s (was %p)", dll, fn, *orig);
            return 1;
        }
    }
    dbg("FAILED to hook %s!%s", dll, fn);
    return 0;
}

static void load_config(void)
{
    char ini[MAX_PATH];
    wsprintfA(ini, "%s\\fxglu.ini", g_dir);
    g_cfgW = GetPrivateProfileIntA("display", "width", g_cfgW, ini);
    g_cfgH = GetPrivateProfileIntA("display", "height", g_cfgH, ini);
    g_cfgFullscreen = GetPrivateProfileIntA("display", "fullscreen", g_cfgFullscreen, ini);
    g_cfgLog = GetPrivateProfileIntA("debug", "log", g_cfgLog, ini);
    g_cfgPerf = GetPrivateProfileIntA("debug", "perfstats", 0, ini);
    g_cfgFastAnswers = GetPrivateProfileIntA("game", "fast_answers", g_cfgFastAnswers, ini);
    g_cfgMsaa = GetPrivateProfileIntA("display", "msaa", g_cfgMsaa, ini);
    g_cfgAniso = GetPrivateProfileIntA("display", "anisotropy", g_cfgAniso, ini);
    g_cfgCursorScale = GetPrivateProfileIntA("display", "cursor_scale", g_cfgCursorScale, ini);
    g_cfgSkipIntro = GetPrivateProfileIntA("game", "skip_intro", g_cfgSkipIntro, ini);
    g_cfgSubs = GetPrivateProfileIntA("game", "subtitles", g_cfgSubs, ini);
    g_cfgWide = GetPrivateProfileIntA("display", "widescreen", g_cfgWide, ini);
    GetPrivateProfileStringA("java", "path", "", g_javaPath, sizeof g_javaPath, ini);
    GetPrivateProfileStringA("java", "args", "", g_javaArgs, sizeof g_javaArgs, ini);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    HMODULE eng;
    char *slash;
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    g_inst = inst;

    GetModuleFileNameA(inst, g_dir, MAX_PATH);
    if ((slash = strrchr(g_dir, '\\'))) *slash = 0;
    load_config();
    if (g_cfgLog) {
        /* one log per host process: the engine DLL (and so we) can be loaded by more than one */
        char path[MAX_PATH], exe[MAX_PATH], *base, *dot;
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        base = strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe;
        if ((dot = strrchr(base, '.'))) *dot = 0;
        wsprintfA(path, "%s\\fxglu_%s.log", g_dir, base);
        g_log = fopen(path, "w");
    }
    dbg("FXGLU loaded in pid %lu; window %dx%d fullscreen=%d msaa=%d aniso=%d cursor=%d skip_intro=%d",
         GetCurrentProcessId(), g_cfgW, g_cfgH, g_cfgFullscreen, g_cfgMsaa, g_cfgAniso,
         g_cfgCursorScale, g_cfgSkipIntro);

    eng = GetModuleHandleA("animEngineDLL.dll");
    if (!eng) { dbg("animEngineDLL.dll not found"); return TRUE; }
    g_eng = (BYTE *)eng;
    if (g_eng != (BYTE *)0x10000000) dbg("note: engine relocated to %p", g_eng);
    hook_iat(eng, "OPENGL32.dll", "glViewport", hk_glViewport, (void **)&real_glViewport);
    hook_iat(eng, "OPENGL32.dll", "glGetIntegerv", hk_glGetIntegerv, (void **)&real_glGetIntegerv);
    hook_iat(eng, "OPENGL32.dll", "glLineWidth", hk_glLineWidth, (void **)&real_glLineWidth);
    hook_iat(eng, "USER32.dll", "SetCursorPos", hk_SetCursorPos, (void **)&real_SetCursorPos);
    hook_iat(eng, "USER32.dll", "ClipCursor", hk_ClipCursor, (void **)&real_ClipCursor);
    hook_iat(eng, "USER32.dll", "CreateWindowExA", hk_CreateWindowExA, (void **)&real_CreateWindowExA);
    hook_iat(eng, "SHELL32.dll", "ShellExecuteA", hk_ShellExecuteA, (void **)&real_ShellExecuteA);
    hook_iat(eng, "GDI32.dll", "ChoosePixelFormat", hk_ChoosePixelFormat, (void **)&real_ChoosePixelFormat);
    hook_iat(eng, "OPENGL32.dll", "glTexParameterf", hk_glTexParameterf, (void **)&real_glTexParameterf);
    hook_iat(eng, "OPENGL32.dll", "glOrtho", hk_glOrtho, (void **)&real_glOrtho);
    hook_iat(eng, "OPENGL32.dll", "glGetDoublev", hk_glGetDoublev, (void **)&real_glGetDoublev);
    hook_iat(eng, "OPENGL32.dll", "glGetFloatv", hk_glGetFloatv, (void **)&real_glGetFloatv);
    hook_iat(eng, "USER32.dll", "LoadImageA", hk_LoadImageA, (void **)&real_LoadImageA);
    if (g_cfgSkipIntro)
        hook_thunk(ENG_INTRO_THUNK, ENG_INTRO_FN, hk_intro, (void **)&real_intro);
    QueryPerformanceFrequency(&g_qpf);
    if (g_cfgPerf || g_cfgFastAnswers) {
        g_processAblRequests = (void (__cdecl *)(int))ENG(ENG_PROCESS_ABL_REQ);
        hook_iat(eng, "KERNEL32.dll", "Sleep", hk_Sleep, (void **)&real_Sleep);
    }
    if (g_cfgFastAnswers && g_cfgLog)
        dbg("fast answers: Java queries serviced during the engine's frame sleep");
    if (g_cfgPerf) {
        hook_iat(eng, "KERNEL32.dll", "WaitForSingleObject", hk_WaitForSingleObject,
                 (void **)&real_WaitForSingleObject);
        CloseHandle(CreateThread(NULL, 0, perf_thread, NULL, 0, NULL));
    }
    return TRUE;
}
