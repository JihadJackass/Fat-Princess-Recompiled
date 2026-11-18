/*
 * Fat Princess Recomp - launcher
 *
 * A small Win32 front end: edits fatprincess.ini (graphics, keyboard
 * bindings, performance, online, paths), checks that the game files are in
 * place, and starts build\FatPrincessRecomp.exe with its output in run.log.
 * Everything it writes is read by the game itself (fp_config.cpp), so the ini
 * stays the one source of truth and can still be edited by hand.
 */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }   /* gdiplus.h wants min/max; NOMINMAX hides them */
#include <gdiplus.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "../fp_ini_defaults.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")

/* ------------------------------------------------------------------------
 * Small string helpers
 * --------------------------------------------------------------------- */
static std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
static std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
static bool file_exists(const std::string& p)
{
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool dir_exists(const std::string& p)
{
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ------------------------------------------------------------------------
 * INI file that keeps its comments: values are edited in place.
 * --------------------------------------------------------------------- */
struct Ini {
    std::vector<std::string> lines;

    void load_text(const std::string& t)
    {
        lines.clear();
        size_t p = 0;
        while (p <= t.size()) {
            size_t e = t.find('\n', p);
            if (e == std::string::npos) e = t.size();
            std::string l = t.substr(p, e - p);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
            p = e + 1;
        }
        while (!lines.empty() && lines.back().empty()) lines.pop_back();
    }
    bool load(const std::string& path)
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        std::string t; char b[4096]; size_t n;
        while ((n = fread(b, 1, sizeof b, f)) > 0) t.append(b, n);
        fclose(f);
        load_text(t);
        return true;
    }
    bool save(const std::string& path) const
    {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return false;
        for (const auto& l : lines) { fputs(l.c_str(), f); fputs("\r\n", f); }
        fclose(f);
        return true;
    }
    static bool is_section(const std::string& l, std::string* name)
    {
        std::string t = trim(l);
        if (t.size() < 2 || t[0] != '[') return false;
        size_t r = t.find(']');
        if (name) *name = lower(trim(t.substr(1, r == std::string::npos ? std::string::npos : r - 1)));
        return true;
    }
    static bool key_of(const std::string& l, std::string* key, std::string* val)
    {
        std::string t = trim(l);
        if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') return false;
        size_t eq = t.find('=');
        if (eq == std::string::npos) return false;
        if (key) *key = lower(trim(t.substr(0, eq)));
        if (val) *val = trim(t.substr(eq + 1));
        return true;
    }
    std::string get(const char* sec, const char* key, const char* def = "") const
    {
        std::string cur, k, v, out = def;
        for (const auto& l : lines) {
            std::string s;
            if (is_section(l, &s)) { cur = s; continue; }
            if (cur == lower(sec) && key_of(l, &k, &v) && k == lower(key)) out = v;
        }
        return out;
    }
    void set(const char* sec, const char* key, const std::string& value)
    {
        std::string cur, k;
        const std::string want = lower(sec);
        int sec_end = -1, found = -1;
        bool in = false;
        for (size_t i = 0; i < lines.size(); i++) {
            std::string s;
            if (is_section(lines[i], &s)) { cur = s; in = (cur == want); if (in) sec_end = (int)i; continue; }
            if (in) {
                if (!trim(lines[i]).empty()) sec_end = (int)i;
                if (key_of(lines[i], &k, nullptr) && k == lower(key)) found = (int)i;
            }
        }
        const std::string line = std::string(key) + "=" + value;
        if (found >= 0) { lines[found] = line; return; }
        if (sec_end >= 0) { lines.insert(lines.begin() + sec_end + 1, line); return; }
        lines.push_back("");
        lines.push_back(std::string("[") + sec + "]");
        lines.push_back(line);
    }
};

/* ------------------------------------------------------------------------
 * Key names: the same spellings fp_config.cpp parses.
 * --------------------------------------------------------------------- */
static std::string vk_name(int vk)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
    if (vk >= VK_F1 && vk <= VK_F12) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Numpad" + std::to_string(vk - VK_NUMPAD0);
    switch (vk) {
    case VK_UP: return "Up"; case VK_DOWN: return "Down"; case VK_LEFT: return "Left"; case VK_RIGHT: return "Right";
    case VK_SPACE: return "Space"; case VK_RETURN: return "Enter"; case VK_TAB: return "Tab";
    case VK_ESCAPE: return "Escape"; case VK_BACK: return "Backspace";
    case VK_LSHIFT: return "LShift"; case VK_RSHIFT: return "RShift"; case VK_SHIFT: return "Shift";
    case VK_LCONTROL: return "LCtrl"; case VK_RCONTROL: return "RCtrl"; case VK_CONTROL: return "Ctrl";
    case VK_LMENU: return "LAlt"; case VK_RMENU: return "RAlt"; case VK_MENU: return "Alt";
    case VK_INSERT: return "Insert"; case VK_DELETE: return "Delete"; case VK_HOME: return "Home";
    case VK_END: return "End"; case VK_PRIOR: return "PageUp"; case VK_NEXT: return "PageDown";
    case VK_CAPITAL: return "CapsLock";
    case VK_LBUTTON: return "MouseLeft"; case VK_RBUTTON: return "MouseRight"; case VK_MBUTTON: return "MouseMiddle";
    case VK_XBUTTON1: return "Mouse4"; case VK_XBUTTON2: return "Mouse5";
    case VK_OEM_COMMA: return "Comma"; case VK_OEM_PERIOD: return "Period"; case VK_OEM_2: return "Slash";
    case VK_OEM_1: return "Semicolon"; case VK_OEM_7: return "Quote"; case VK_OEM_4: return "LBracket";
    case VK_OEM_6: return "RBracket"; case VK_OEM_MINUS: return "Minus"; case VK_OEM_PLUS: return "Equals";
    case VK_OEM_5: return "Backslash"; case VK_OEM_3: return "Grave";
    default: return std::string();
    }
}

/* ------------------------------------------------------------------------
 * Model
 * --------------------------------------------------------------------- */
struct Bind { const char* key; const char* label; std::string keys; };
static Bind g_binds[] = {
    { "LeftStickUp", "Move up (left stick)", "" },     { "LeftStickDown", "Move down (left stick)", "" },
    { "LeftStickLeft", "Move left (left stick)", "" }, { "LeftStickRight", "Move right (left stick)", "" },
    { "Cross", "Cross", "" }, { "Circle", "Circle", "" }, { "Square", "Square", "" }, { "Triangle", "Triangle", "" },
    { "L1", "L1", "" }, { "R1", "R1", "" }, { "L2", "L2", "" }, { "R2", "R2", "" },
    { "L3", "L3 (left stick click)", "" }, { "R3", "R3 (right stick click)", "" },
    { "Start", "Start", "" }, { "Select", "Select", "" },
    { "DpadUp", "D-pad up", "" }, { "DpadDown", "D-pad down", "" },
    { "DpadLeft", "D-pad left", "" }, { "DpadRight", "D-pad right", "" },
    { "RightStickUp", "Right stick up", "" }, { "RightStickDown", "Right stick down", "" },
    { "RightStickLeft", "Right stick left", "" }, { "RightStickRight", "Right stick right", "" },
};
static const int kBinds = (int)(sizeof g_binds / sizeof g_binds[0]);

/* What each button does in Fat Princess, from the game's own tips and help
 * text (data/local/english/symbols.txt in siege.qrk). Short form for the list,
 * long form for the status line when a row is selected. Same order as g_binds. */
static const char* const kBindWhat[][2] = {
    { "Move", "Moves your character." },
    { "Move", "Moves your character." },
    { "Move", "Moves your character." },
    { "Move", "Moves your character." },
    { "Jump; confirm in menus", "Jumps onto springboards and into catapults; confirms in menus." },
    { "Pick up / drop; back in menus", "Picks up and drops hats (class change), cake, resources and the Princess; escorts. Back in menus." },
    { "Attack (tap), charge (hold)", "Attacks; hold to charge. Also builds, repairs, chops, throws, gives cake, and heals as the Priest." },
    { "Switch weapon / special", "Switches weapons on upgraded classes; DLC specials: Pirate cannon, Ninja smoke bomb, Giant chomp." },
    { "Lock on (hold); Warrior shield", "Hold to lock on to enemies, trees, rocks and build sites; the Warrior holds up his shield." },
    { "Not used in play", "No use in normal play is mentioned in the game's own help." },
    { "Not used in play", "No use in normal play is mentioned in the game's own help." },
    { "Voice chat (online)", "Talk to teammates online. Not needed offline." },
    { "Hold with R3: get unstuck", "Hold L3 and R3 together if you get stuck." },
    { "Hold with L3: get unstuck", "Hold L3 and R3 together if you get stuck." },
    { "Pause menu, scoreboard; join in", "Opens the pause menu and scoreboard; joins split-screen co-op on another controller." },
    { "World map", "Shows the world map." },
    { "Bots follow you (up to 3)", "Orders up to 3 nearby AI teammates to follow you." },
    { "Send following bots away", "Releases the AI teammates following you." },
    { "Taunt", "Taunts." },
    { "Taunt", "Taunts." },
    { "Not used in play", "Only used to zoom in the screen-size menu." },
    { "Not used in play", "Only used to zoom in the screen-size menu." },
    { "Not used in play", "Only used to zoom in the screen-size menu." },
    { "Not used in play", "Only used to zoom in the screen-size menu." },
};

/* Recommended layouts: the key list for each g_binds entry, in order. */
struct Layout { const char* name; const char* about; const char* keys[24]; };
static const Layout kLayouts[] = {
    { "Keyboard and mouse",
      "WASD to move, left mouse attacks, right mouse locks on, Space jumps, E picks up, Q switches weapon.",
      { "W", "S", "A", "D",
        "Space", "E", "MouseLeft", "Q",
        "MouseRight", "None", "None", "None",
        "Z", "X",
        "Escape,Enter", "Tab",
        "1", "2", "3", "4",
        "None", "None", "None", "None" } },
    { "Keyboard only (WASD + JKL), fits 60% keyboards",
      "WASD to move; right hand on J attack, K pick up, L switch weapon, Space jump, hold Shift to lock on.",
      { "W", "S", "A", "D",
        "Space", "K", "J", "L",
        "LShift", "None", "None", "None",
        "Z", "X",
        "Escape,Enter", "Tab",
        "1", "2", "3", "4",
        "None", "None", "None", "None" } },
    { "Original defaults",
      "The launcher's first layout.",
      { nullptr } },   /* from fp_ini_defaults.h */
};

static std::string g_root, g_ini_path, g_exe, g_gamedir;
static Ini g_ini, g_defaults;
static std::vector<std::string> g_gpus;
static HWND g_main, g_status, g_play;
static std::vector<HWND> g_pages[6];
static HFONT g_font, g_bold;
/* Left sidebar: the Fat Princess logo (resource 2, assets/logo.bmp) on its own
 * blue, with the Play / Save / log / folder buttons under it. */
static const int kSidebar = 232;
static const COLORREF kSideColor = RGB(108, 133, 199);   /* logo_source.png background */
static HBRUSH g_side_brush;
static HBITMAP g_logo;
static HWND g_side_info;
static double g_dpi = 1.0;
static volatile LONG g_running = 0;
static HANDLE g_proc = NULL;          /* the running game, for Stop */
/* No saves until ui_from_ini has filled every control from the ini. Building
 * the window sends change notifications (the free-cores box gets its number
 * from its spin control), and a save then wrote the still-empty controls over
 * the whole file: bindings None, toggles off, DLC key blank. */
static bool g_loading = true;
static bool g_ui_ready = false;
static bool g_dlc_edited = false;     /* the DLC key field was changed by hand this session */

enum {
    ID_TAB = 100, ID_PLAY, ID_LOG, ID_FOLDER,
    ID_DISPLAY = 200, ID_VSYNC, ID_GPU, ID_FPS, ID_FRAMELIMIT, ID_FRAMELIMIT_INFO,
    ID_BINDLIST = 300, ID_BIND_SET, ID_BIND_ADD, ID_BIND_CLEAR, ID_BIND_RESET, ID_HIDECOOP, ID_BIND_LAYOUTS,
    ID_BUTTONICONS, ID_SKIPHOWTO, ID_MENUHINTS, ID_BOTMODE, ID_BOT_SIEGE,
    ID_PRIORITY = 400, ID_FREECORES, ID_FREECORES_SPIN, ID_PROFILE, ID_WAITSLEEP, ID_THREADREUSE, ID_REPORT_OPEN,
    ID_ONLINE = 500, ID_SERVER, ID_USERNAME,
    ID_DEVFLASH = 700, ID_DEVFLASH_BROWSE, ID_SIEGE_COPY, ID_PATHS_INFO, ID_DLCKEY,
    ID_TAB0 = 800,          /* kTabs tab buttons */
    ID_DARK = 820,
};
static const int kTabs = 6;   /* Graphics, Controls, Bots, Performance, Online, Game files */
#define WM_APP_GAME_EXIT (WM_APP + 1)

static int S(int v) { return (int)(v * g_dpi + 0.5); }
static HWND item(int id) { return GetDlgItem(g_main, id); }

/* ------------------------------------------------------------------------
 * Look: dark (default) and light. The tabs, buttons and on/off switches are
 * drawn here, because the stock tab control and themed checkboxes can't be
 * recoloured. Edits, lists and combo boxes take their colours from
 * WM_CTLCOLOR* / custom draw, plus Windows' own dark themes for borders and
 * scrollbars where it has them (Windows 10 1809 and later).
 * --------------------------------------------------------------------- */
struct Palette { COLORREF bg, panel, text, muted, border, btn, btn_down, field, stripe, sel; };
static const Palette kDark  = { RGB(22, 24, 31),   RGB(31, 34, 43),   RGB(229, 231, 238), RGB(148, 155, 173),
                                RGB(56, 61, 75),   RGB(45, 49, 61),   RGB(62, 67, 83),    RGB(38, 42, 53),
                                RGB(43, 47, 59),   RGB(74, 90, 140) };
static const Palette kLight = { RGB(236, 238, 244), RGB(255, 255, 255), RGB(27, 30, 39),  RGB(92, 99, 116),
                                RGB(206, 210, 221), RGB(230, 233, 240), RGB(208, 213, 225), RGB(255, 255, 255),
                                RGB(245, 246, 250), RGB(196, 210, 242) };
static const COLORREF kGold = RGB(242, 179, 61), kGoldDown = RGB(214, 153, 40), kGoldText = RGB(43, 30, 6);
static const COLORREF kSideBtn = RGB(88, 110, 176), kSideBtnDown = RGB(72, 92, 152),
                      kSideBorder = RGB(150, 170, 222), kSideOff = RGB(64, 82, 138), kSideText = RGB(255, 255, 255);
static Palette P = kDark;
static bool g_dark = true;
static HBRUSH g_br_bg, g_br_panel, g_br_field;
static int g_page = 0;

/* Geometry shared by the painter and the controls (layout units, scaled by S). */
static const int kTabY = 12, kTabH = 33, kTabW = 114, kPanelX = kSidebar + 12, kPanelW = 696, kPanelBottom = 488;
static RECT panel_rect()
{
    RECT r = { S(kPanelX), S(kTabY) + S(kTabH) - 1, S(kPanelX) + S(kPanelW), S(kPanelBottom) };
    return r;
}
enum Zone { Z_SIDE, Z_BG, Z_PANEL };
static Zone zone_of(HWND c)
{
    RECT r; GetWindowRect(c, &r);
    POINT pt = { r.left, r.top }; ScreenToClient(g_main, &pt);
    if (pt.x < S(kSidebar)) return Z_SIDE;
    const RECT p = panel_rect();
    return (pt.y >= p.top && pt.y < p.bottom) ? Z_PANEL : Z_BG;
}
static COLORREF zone_color(Zone z) { return z == Z_SIDE ? kSideColor : z == Z_PANEL ? P.panel : P.bg; }
static HBRUSH zone_brush(Zone z) { return z == Z_SIDE ? g_side_brush : z == Z_PANEL ? g_br_panel : g_br_bg; }

static bool is_toggle(int id) { return id == ID_FPS || id == ID_ONLINE || id == ID_DARK || id == ID_PROFILE ||
                                        id == ID_WAITSLEEP || id == ID_THREADREUSE || id == ID_HIDECOOP ||
                                        id == ID_SKIPHOWTO || id == ID_MENUHINTS || id == ID_BOT_SIEGE; }
static bool is_tab(int id) { return id >= ID_TAB0 && id < ID_TAB0 + kTabs; }
static bool tog_get(int id) { return GetWindowLongPtrA(item(id), GWLP_USERDATA) != 0; }
static void tog_set(int id, bool on)
{
    SetWindowLongPtrA(item(id), GWLP_USERDATA, on ? 1 : 0);
    InvalidateRect(item(id), NULL, FALSE);
}

static Gdiplus::Color gc(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }
static void rr(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, COLORREF fill, const COLORREF* edge)
{
    Gdiplus::GraphicsPath p;
    const float d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
    Gdiplus::SolidBrush b(gc(fill));
    g.FillPath(&b, &p);
    if (edge) { Gdiplus::Pen pen(gc(*edge), 1.0f); g.DrawPath(&pen, &p); }
}
static void hline(HDC dc, int x0, int x1, int y, COLORREF c)
{
    RECT r = { x0, y, x1, y + 1 };
    HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b);
}
static void vline(HDC dc, int x, int y0, int y1, COLORREF c)
{
    RECT r = { x, y0, x + 1, y1 };
    HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b);
}

static void draw_button(const DRAWITEMSTRUCT* d)
{
    const int id = (int)d->CtlID;
    const int w = d->rcItem.right - d->rcItem.left, h = d->rcItem.bottom - d->rcItem.top;
    const Zone z = zone_of(d->hwndItem);
    const bool down = (d->itemState & ODS_SELECTED) != 0;
    const bool dis = (d->itemState & ODS_DISABLED) != 0;
    const bool focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT);
    char text[160] = { 0 }; GetWindowTextA(d->hwndItem, text, sizeof text);

    HDC mem = CreateCompatibleDC(d->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(d->hDC, w, h);
    HGDIOBJ obmp = SelectObject(mem, bmp);
    RECT r = { 0, 0, w, h };
    HBRUSH bg = CreateSolidBrush(zone_color(z)); FillRect(mem, &r, bg); DeleteObject(bg);

    COLORREF tc = P.text;
    RECT tr = r;
    UINT tf = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    if (is_tab(id)) {
        const bool sel = id - ID_TAB0 == g_page;
        if (sel) {
            RECT f = { 0, 0, w, h }; HBRUSH b = CreateSolidBrush(P.panel); FillRect(mem, &f, b); DeleteObject(b);
            hline(mem, 0, w, 0, P.border); vline(mem, 0, 0, h, P.border); vline(mem, w - 1, 0, h, P.border);
            RECT acc = { 1, 1, w - 1, 1 + S(3) }; HBRUSH a = CreateSolidBrush(kGold); FillRect(mem, &acc, a); DeleteObject(a);
        } else {
            hline(mem, 0, w, h - 1, P.border);
        }
        tc = sel ? P.text : P.muted;
        if (focus) { RECT fr = { S(6), S(7), w - S(6), h - S(5) }; DrawFocusRect(mem, &fr); }
    } else if (is_toggle(id)) {
        const bool on = tog_get(id);
        const int th = S(20), tw = S(38), ty = (h - th) / 2;
        {
            Gdiplus::Graphics g(mem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const COLORREF track = on ? kGold : (z == Z_SIDE ? kSideOff : P.btn_down);
            const COLORREF ring = kGold;
            rr(g, 1.5f, ty + 0.5f, (float)tw, (float)th, th / 2.0f, track, focus ? &ring : NULL);
            const float kd = (float)(th - S(6));
            const float kx = on ? 1.5f + tw - S(3) - kd : 1.5f + S(3);
            Gdiplus::SolidBrush knob(gc(RGB(255, 255, 255)));
            g.FillEllipse(&knob, kx, ty + 0.5f + S(3), kd, kd);
        }
        tr.left = tw + S(12);
        tf = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
        tc = z == Z_SIDE ? kSideText : (dis ? P.muted : P.text);
    } else {
        COLORREF fill, edge;
        if (id == ID_PLAY && g_running) { fill = down ? RGB(160, 44, 52) : RGB(190, 56, 64); edge = fill; tc = RGB(255, 255, 255); }
        else if (id == ID_PLAY) { fill = down ? kGoldDown : kGold; edge = fill; tc = kGoldText; }
        else if (z == Z_SIDE) { fill = down ? kSideBtnDown : kSideBtn; edge = kSideBorder; tc = kSideText; }
        else { fill = down ? P.btn_down : P.btn; edge = P.border; tc = P.text; }
        if (dis) { fill = z == Z_SIDE ? kSideBtnDown : P.btn; edge = fill; tc = z == Z_SIDE ? RGB(196, 206, 234) : P.muted; }
        {
            Gdiplus::Graphics g(mem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            rr(g, 0.5f, 0.5f, w - 1.0f, h - 1.0f, (float)S(6), fill, &edge);
            if (focus) {
                const COLORREF ring = id == ID_PLAY ? kGoldText : kGold;
                rr(g, (float)S(3), (float)S(3), w - 1.0f - 2 * S(3), h - 1.0f - 2 * S(3), (float)S(4), fill, &ring);
            }
        }
    }
    HGDIOBJ of = SelectObject(mem, (HFONT)SendMessageA(d->hwndItem, WM_GETFONT, 0, 0));
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, tc);
    DrawTextA(mem, text, -1, &tr, tf);
    SelectObject(mem, of);

    BitBlt(d->hDC, d->rcItem.left, d->rcItem.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, obmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static void draw_combo(const DRAWITEMSTRUCT* d)
{
    const bool field = (d->itemState & ODS_COMBOBOXEDIT) != 0;
    const bool sel = (d->itemState & ODS_SELECTED) && !field;
    HBRUSH b = CreateSolidBrush(sel ? P.sel : P.field);
    FillRect(d->hDC, &d->rcItem, b); DeleteObject(b);
    if (d->itemID == (UINT)-1) return;
    char t[256] = { 0 };
    if (SendMessageA(d->hwndItem, CB_GETLBTEXTLEN, d->itemID, 0) < (LRESULT)sizeof t)
        SendMessageA(d->hwndItem, CB_GETLBTEXT, d->itemID, (LPARAM)t);
    RECT r = d->rcItem; r.left += S(6); r.right -= S(4);
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, P.text);
    DrawTextA(d->hDC, t, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/* The key list's column header: its notifications go to the list view itself. */
static LRESULT CALLBACK list_subclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
{
    if (m == WM_NOTIFY) {
        NMHDR* n = (NMHDR*)l;
        if (n->hwndFrom == ListView_GetHeader(h) && n->code == NM_CUSTOMDRAW) {
            NMCUSTOMDRAW* cd = (NMCUSTOMDRAW*)l;
            if (cd->dwDrawStage == CDDS_PREPAINT) {
                RECT r; GetClientRect(n->hwndFrom, &r);
                HBRUSH b = CreateSolidBrush(P.btn); FillRect(cd->hdc, &r, b); DeleteObject(b);
                return CDRF_NOTIFYITEMDRAW;
            }
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                RECT r = cd->rc;
                HBRUSH b = CreateSolidBrush(P.btn); FillRect(cd->hdc, &r, b); DeleteObject(b);
                hline(cd->hdc, r.left, r.right, r.bottom - 1, P.border);
                vline(cd->hdc, r.right - 1, r.top + S(4), r.bottom - S(4), P.border);
                char t[64] = { 0 };
                HDITEMA hi = { 0 }; hi.mask = HDI_TEXT; hi.pszText = t; hi.cchTextMax = sizeof t;
                SendMessageA(n->hwndFrom, HDM_GETITEMA, cd->dwItemSpec, (LPARAM)&hi);
                r.left += S(8);
                HGDIOBJ of = SelectObject(cd->hdc, g_font);
                SetBkMode(cd->hdc, TRANSPARENT);
                SetTextColor(cd->hdc, P.text);
                DrawTextA(cd->hdc, t, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                SelectObject(cd->hdc, of);
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    return DefSubclassProc(h, m, w, l);
}

static void set_dark_titlebar(HWND h)
{
    typedef HRESULT (WINAPI *Fn)(HWND, DWORD, LPCVOID, DWORD);
    static Fn fn = (Fn)GetProcAddress(LoadLibraryA("dwmapi.dll"), "DwmSetWindowAttribute");
    if (!fn) return;
    BOOL v = g_dark;
    if (FAILED(fn(h, 20, &v, sizeof v))) fn(h, 19, &v, sizeof v);   /* DWMWA_USE_IMMERSIVE_DARK_MODE (19 before 20H1) */
}

/* Dark scrollbars in drop-down lists: uxtheme's SetPreferredAppMode (ordinal
 * 135, Windows 10 1903+). Skipped on older builds where 135 means something else. */
static void set_app_mode()
{
    typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW*);
    static DWORD build = 0;
    if (!build) {
        OSVERSIONINFOW v = { sizeof v };
        RtlGetVersionFn rgv = (RtlGetVersionFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
        if (rgv && rgv(&v) == 0) build = v.dwBuildNumber;
        if (!build) build = 1;
    }
    if (build < 18362) return;
    HMODULE ux = LoadLibraryExA("uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    typedef int (WINAPI *SetModeFn)(int);
    typedef void (WINAPI *FlushFn)(void);
    SetModeFn set_mode = (SetModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    FlushFn flush = (FlushFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (set_mode) set_mode(g_dark ? 2 : 3);   /* ForceDark / ForceLight */
    if (flush) flush();
}

static BOOL CALLBACK theme_child(HWND h, LPARAM)
{
    char cls[64] = { 0 }; GetClassNameA(h, cls, sizeof cls);
    if (!lstrcmpiA(cls, "Edit") || !lstrcmpiA(cls, "ComboBox")) {
        SetWindowTheme(h, g_dark ? L"DarkMode_CFD" : NULL, NULL);
        COMBOBOXINFO ci = { sizeof ci };
        if (!lstrcmpiA(cls, "ComboBox") && GetComboBoxInfo(h, &ci) && ci.hwndList)
            SetWindowTheme(ci.hwndList, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    } else if (!lstrcmpiA(cls, WC_LISTVIEWA)) {
        SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
        ListView_SetBkColor(h, P.field);
        ListView_SetTextBkColor(h, P.field);
        ListView_SetTextColor(h, P.text);
    } else if (!lstrcmpiA(cls, UPDOWN_CLASSA)) {
        SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
    }
    return TRUE;
}

static void apply_theme()
{
    P = g_dark ? kDark : kLight;
    if (g_br_bg) DeleteObject(g_br_bg);
    if (g_br_panel) DeleteObject(g_br_panel);
    if (g_br_field) DeleteObject(g_br_field);
    g_br_bg = CreateSolidBrush(P.bg);
    g_br_panel = CreateSolidBrush(P.panel);
    g_br_field = CreateSolidBrush(P.field);
    set_app_mode();
    if (!g_main) return;
    set_dark_titlebar(g_main);
    EnumChildWindows(g_main, theme_child, 0);
    RedrawWindow(g_main, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
    /* The caption only repaints in the new colour after an activation change. */
    if (GetActiveWindow() == g_main) { SendMessageA(g_main, WM_NCACTIVATE, FALSE, 0); SendMessageA(g_main, WM_NCACTIVATE, TRUE, 0); }
}

/* ------------------------------------------------------------------------
 * Environment checks
 * --------------------------------------------------------------------- */
static std::string detect_devflash()
{
    std::string v = g_ini.get("paths", "devflash");
    if (!v.empty()) return v;
    const char* bases[] = { getenv("USERPROFILE"), getenv("LOCALAPPDATA"), getenv("APPDATA"), getenv("ProgramFiles") };
    const char* subs[] = { "Desktop\\RPCS3", "Downloads\\RPCS3", "RPCS3", "rpcs3" };
    for (const char* b : bases) {
        if (!b) continue;
        for (const char* s : subs) {
            std::string c = std::string(b) + "\\" + s + "\\dev_flash";
            if (dir_exists(c + "\\sys\\external")) return c;
        }
    }
    if (dir_exists("C:\\RPCS3\\dev_flash\\sys\\external")) return "C:\\RPCS3\\dev_flash";
    return std::string();
}

static void enumerate_gpus()
{
    g_gpus.clear();
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) return;
    auto gipa = (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress(vk, "vkGetInstanceProcAddr");
    if (!gipa) return;
    auto create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "FatPrincessLauncher"; app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (create(&ci, NULL, &inst) != VK_SUCCESS) return;
    auto enumd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    auto props = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    auto destroy = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    uint32_t n = 0;
    if (enumd && props && enumd(inst, &n, NULL) == VK_SUCCESS && n) {
        std::vector<VkPhysicalDevice> d(n);
        enumd(inst, &n, d.data());
        for (uint32_t i = 0; i < n; i++) {
            VkPhysicalDeviceProperties p; props(d[i], &p);
            g_gpus.push_back(std::to_string(i) + ": " + p.deviceName);
        }
    }
    if (destroy) destroy(inst, NULL);
}

/* ------------------------------------------------------------------------
 * Controls
 * --------------------------------------------------------------------- */
static HWND mk(const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id, int page,
               DWORD ex = 0)
{
    HWND hw = CreateWindowExA(ex, cls, text, WS_CHILD | style, S(x), S(y), S(w), S(h), g_main,
                              (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(hw, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (page >= 0) g_pages[page].push_back(hw);
    else ShowWindow(hw, SW_SHOW);
    return hw;
}
static void edit_margins(HWND e) { SendMessageA(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(5), S(5))); }
static HWND label(const char* t, int x, int y, int w, int page, int h = 20)
{ return mk("STATIC", t, SS_LEFT, x, y, w, h, 0, page); }
static HWND combo(int id, int x, int y, int w, int page, const std::vector<std::string>& items)
{
    HWND c = mk("COMBOBOX", "", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
                x, y, w, 200, id, page);
    for (const auto& s : items) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    return c;
}
static void set_status(const std::string& s) { SetWindowTextA(g_status, s.c_str()); }
static std::string text_of(int id)
{
    char b[1024] = { 0 }; GetWindowTextA(item(id), b, sizeof b); return trim(b);
}

static void show_page(int p)
{
    for (int i = 0; i < kTabs; i++)
        for (HWND h : g_pages[i]) ShowWindow(h, i == p ? SW_SHOW : SW_HIDE);
}

static void update_paths_info();
static void select_page(int p)
{
    g_page = p;
    show_page(p);
    for (int i = 0; i < kTabs; i++) InvalidateRect(item(ID_TAB0 + i), NULL, FALSE);
    const RECT pr = panel_rect(); InvalidateRect(g_main, &pr, TRUE);
    if (p == 4) update_paths_info();
}

static void refresh_bind_list()
{
    HWND lv = item(ID_BINDLIST);
    const int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < kBinds; i++) {
        LVITEMA it = { 0 };
        it.mask = LVIF_TEXT; it.iItem = i; it.pszText = (char*)g_binds[i].label;
        SendMessageA(lv, LVM_INSERTITEMA, 0, (LPARAM)&it);
        LVITEMA what = { 0 };
        what.iSubItem = 1; what.pszText = (char*)kBindWhat[i][0];
        SendMessageA(lv, LVM_SETITEMTEXTA, i, (LPARAM)&what);
        std::string k = g_binds[i].keys.empty() || lower(g_binds[i].keys) == "none" ? "(unbound)" : g_binds[i].keys;
        LVITEMA sub = { 0 };
        sub.iSubItem = 2; sub.pszText = (char*)k.c_str();
        SendMessageA(lv, LVM_SETITEMTEXTA, i, (LPARAM)&sub);
    }
    if (sel >= 0) { ListView_SetItemState(lv, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); ListView_EnsureVisible(lv, sel, FALSE); }
    ListView_SetColumnWidth(lv, 2, LVSCW_AUTOSIZE_USEHEADER);   /* fill the row: no stock-coloured header filler */
}

static std::vector<std::string> split_keys(const std::string& s)
{
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= s.size()) {
        size_t c = s.find(',', p);
        if (c == std::string::npos) c = s.size();
        std::string k = trim(s.substr(p, c - p));
        if (!k.empty() && lower(k) != "none") out.push_back(k);
        p = c + 1;
    }
    return out;
}
static std::string join_keys(const std::vector<std::string>& v)
{
    if (v.empty()) return "None";
    std::string s;
    for (size_t i = 0; i < v.size(); i++) { if (i) s += ","; s += v[i]; }
    return s;
}

/* Modal "press a key" box. Returns the VK, or 0 if cancelled (Esc / close). */
static int s_cap_vk = 0, s_cap_done = 0;
static LRESULT CALLBACK cap_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        int vk = (int)w;
        const UINT sc = (UINT)((l >> 16) & 0xFF);
        const bool ext = (l >> 24) & 1;
        if (vk == VK_SHIFT) vk = (int)MapVirtualKeyA(sc, MAPVK_VSC_TO_VK_EX);
        else if (vk == VK_CONTROL) vk = ext ? VK_RCONTROL : VK_LCONTROL;
        else if (vk == VK_MENU) vk = ext ? VK_RMENU : VK_LMENU;
        if (vk == VK_ESCAPE) { s_cap_vk = 0; s_cap_done = 1; return 0; }
        if (!vk_name(vk).empty()) { s_cap_vk = vk; s_cap_done = 1; }
        return 0;
    }
    /* Mouse buttons bind too (the game reads them with the keys). */
    case WM_LBUTTONDOWN: s_cap_vk = VK_LBUTTON; s_cap_done = 1; return 0;
    case WM_RBUTTONDOWN: s_cap_vk = VK_RBUTTON; s_cap_done = 1; return 0;
    case WM_MBUTTONDOWN: s_cap_vk = VK_MBUTTON; s_cap_done = 1; return 0;
    case WM_XBUTTONDOWN: s_cap_vk = HIWORD(w) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1; s_cap_done = 1; return TRUE;
    case WM_CLOSE: s_cap_vk = 0; s_cap_done = 1; return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        FillRect(dc, &r, g_br_panel);
        HGDIOBJ of = SelectObject(dc, g_bold);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, P.text);
        DrawTextA(dc, "Press a key, or click a mouse button\nin this box, to bind it\n\n(Esc cancels)", -1, &r, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcA(h, m, w, l);
}
static int capture_key(const char* what)
{
    static bool reg = false;
    if (!reg) {
        WNDCLASSA wc = { 0 };
        wc.lpfnWndProc = cap_proc; wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = NULL;   /* painted in WM_PAINT */
        wc.lpszClassName = "fp_capture";
        RegisterClassA(&wc); reg = true;
    }
    RECT pr; GetWindowRect(g_main, &pr);
    const int w = S(340), h = S(160);
    std::string title = std::string("Bind: ") + what;
    HWND hw = CreateWindowExA(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, "fp_capture", title.c_str(),
                              WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                              pr.left + (pr.right - pr.left - w) / 2, pr.top + (pr.bottom - pr.top - h) / 2,
                              w, h, g_main, NULL, GetModuleHandleA(NULL), NULL);
    set_dark_titlebar(hw);
    EnableWindow(g_main, FALSE);
    SetFocus(hw);
    s_cap_done = 0; s_cap_vk = 0;
    MSG msg;
    while (!s_cap_done && GetMessageA(&msg, NULL, 0, 0)) {
        if (msg.hwnd == hw && (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN)) {
            cap_proc(hw, msg.message, msg.wParam, msg.lParam);   /* before Alt/F10 menu handling */
            continue;
        }
        TranslateMessage(&msg); DispatchMessageA(&msg);
    }
    EnableWindow(g_main, TRUE);
    DestroyWindow(hw);
    SetForegroundWindow(g_main);
    return s_cap_vk;
}

static bool save_ini();
/* "Recommended layouts...": a menu of whole keyboard (and mouse) layouts. */
static void apply_layout(int li)
{
    const Layout& L = kLayouts[li];
    const std::string q = std::string("Replace every key binding with the \"") + L.name + "\" layout?\n\n" + L.about;
    if (MessageBoxA(g_main, q.c_str(), "Fat Princess", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    for (int i = 0; i < kBinds; i++)
        g_binds[i].keys = L.keys[0] ? std::string(L.keys[i]) : g_defaults.get("controls", g_binds[i].key);
    refresh_bind_list();
    save_ini();
    set_status(std::string("Layout \"") + L.name + "\" applied and saved.");
}
static void layouts_menu()
{
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < (int)(sizeof kLayouts / sizeof kLayouts[0]); i++)
        AppendMenuA(m, MF_STRING, 1 + i, kLayouts[i].name);
    RECT r; GetWindowRect(item(ID_BIND_LAYOUTS), &r);
    const int pick = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY,
                                         r.left, r.bottom, 0, g_main, NULL);
    DestroyMenu(m);
    if (pick > 0) apply_layout(pick - 1);
}

static int selected_bind()
{
    return ListView_GetNextItem(item(ID_BINDLIST), -1, LVNI_SELECTED);
}

static void bind_key(bool add)
{
    const int i = selected_bind();
    if (i < 0) { set_status("Pick a button in the list first."); return; }
    const int vk = capture_key(g_binds[i].label);
    if (!vk) { set_status("Binding cancelled."); return; }
    const std::string name = vk_name(vk);
    std::vector<std::string> keys = add ? split_keys(g_binds[i].keys) : std::vector<std::string>();
    for (const auto& k : keys) if (lower(k) == lower(name)) { set_status(name + " is already on " + g_binds[i].label + "."); return; }
    if (keys.size() >= 4) keys.pop_back();
    keys.push_back(name);
    g_binds[i].keys = join_keys(keys);
    /* Say where else the key is used: allowed, but rarely meant. */
    std::string also;
    for (int j = 0; j < kBinds; j++) {
        if (j == i) continue;
        for (const auto& k : split_keys(g_binds[j].keys))
            if (lower(k) == lower(name)) { if (!also.empty()) also += ", "; also += g_binds[j].label; }
    }
    refresh_bind_list();
    save_ini();
    set_status(also.empty() ? (std::string(g_binds[i].label) + " = " + g_binds[i].keys + " (saved)")
                            : (name + " is also bound to: " + also + " (saved)"));
}

/* ------------------------------------------------------------------------
 * Load / save
 * --------------------------------------------------------------------- */
static void select_combo(int id, int idx) { SendMessageA(item(id), CB_SETCURSEL, idx, 0); }
static int combo_sel(int id) { return (int)SendMessageA(item(id), CB_GETCURSEL, 0, 0); }

/* The line under the frame rate choice: what the selected option does. */
static void update_framelimit_info()
{
    static const char* text[] = {
        "The game's own speed: 30 frames a second, as on the PS3. Recommended.",
        "Shows a new frame on every refresh. Work in progress: the game's logic is not\r\n"
        "yet adjusted for it, so the game can run faster than it should.",
        "No frame rate limit at all. Work in progress: for testing performance only;\r\n"
        "the game runs faster than it should.",
    };
    const int i = combo_sel(ID_FRAMELIMIT);
    SetWindowTextA(item(ID_FRAMELIMIT_INFO), text[i >= 0 && i < 3 ? i : 0]);
}

static std::string dlc_key_hex(bool* bad);
/* Sidebar line under the logo: version and whether Fat Roles is unlocked. */
static void update_side_info()
{
    bool bad = false;
    const bool dlc = !dlc_key_hex(&bad).empty();
    std::string s = "Fat Princess v2.00 (NPUA80139)\r\n";
    s += dlc ? "Fat Roles DLC: on" : "Fat Roles DLC: needs your key (Game files)";
    SetWindowTextA(g_side_info, s.c_str());
    RECT r; GetWindowRect(g_side_info, &r); MapWindowPoints(NULL, g_main, (POINT*)&r, 2);
    InvalidateRect(g_main, &r, TRUE);
}

static void ui_from_ini()
{
    g_loading = true;
    const std::string fs = lower(g_ini.get("graphics", "fullscreen", "0"));
    select_combo(ID_DISPLAY, (fs == "1" || fs == "on" || fs == "true") ? 1 : 0);
    const std::string vs = lower(g_ini.get("graphics", "vsync", "auto"));
    select_combo(ID_VSYNC, (vs == "on" || vs == "1") ? 1 : (vs == "off" || vs == "0") ? 2 : 0);
    const std::string gpu = lower(g_ini.get("graphics", "gpu", "auto"));
    int gsel = 0;
    if (gpu != "auto" && !gpu.empty()) { const int n = atoi(gpu.c_str()); if (n >= 0 && n < (int)g_gpus.size()) gsel = n + 1; }
    select_combo(ID_GPU, gsel);
    /* The game paces itself to 30 FPS, as on the PS3; an old "60" or "30"
     * both mean that pacing. */
    { const std::string fl = lower(trim(g_ini.get("graphics", "framelimit", "30")));
      select_combo(ID_FRAMELIMIT, fl == "60fps" ? 1 : fl == "unlocked" ? 2 : 0);
      update_framelimit_info(); }
    const std::string sf = lower(g_ini.get("graphics", "showfps", "1"));
    tog_set(ID_FPS, !(sf == "0" || sf == "off"));

    for (int i = 0; i < kBinds; i++)
        g_binds[i].keys = g_ini.get("controls", g_binds[i].key, g_defaults.get("controls", g_binds[i].key).c_str());
    refresh_bind_list();
    { const std::string hc = lower(trim(g_ini.get("controls", "hidecoopstart", "1")));
      tog_set(ID_HIDECOOP, !(hc == "0" || hc == "off")); }
    { const std::string v = lower(trim(g_ini.get("controls", "menuhints", "1")));
      tog_set(ID_MENUHINTS, !(v == "0" || v == "off")); }
    { const std::string v = lower(trim(g_ini.get("controls", "skiphowtoplayprompt", "0")));
      tog_set(ID_SKIPHOWTO, v == "1" || v == "on"); }
    { const std::string v = lower(trim(g_ini.get("bots", "mode", "classic")));
      select_combo(ID_BOTMODE, v == "improved" ? 1 : 0); }
    { const std::string v = lower(trim(g_ini.get("bots", "siege", "1")));
      tog_set(ID_BOT_SIEGE, !(v == "0" || v == "off")); }
    { const std::string v = lower(trim(g_ini.get("controls", "buttonicons", "auto")));
      select_combo(ID_BUTTONICONS, v == "keyboard" ? 1 : v == "xbox" ? 2 : v == "ps" ? 3 : 0); }

    const std::string pr = lower(g_ini.get("performance", "priority", "normal"));
    select_combo(ID_PRIORITY, pr == "belownormal" ? 1 : pr == "high" ? 2 : 0);
    { const std::string pf = lower(trim(g_ini.get("performance", "profile", "0")));
      tog_set(ID_PROFILE, pf == "1" || pf == "on"); }
    SetWindowTextA(item(ID_FREECORES), g_ini.get("performance", "freecores", "1").c_str());
    tog_set(ID_WAITSLEEP, lower(trim(g_ini.get("performance", "waitmode", "spin"))) == "sleep");
    { const std::string r = lower(trim(g_ini.get("performance", "sputhreadreuse", "0")));
      tog_set(ID_THREADREUSE, r == "1" || r == "on"); }

    const std::string on = lower(g_ini.get("online", "enabled", "0"));
    tog_set(ID_ONLINE, on == "1" || on == "on");
    tog_set(ID_DARK, g_dark);
    SetWindowTextA(item(ID_SERVER), g_ini.get("online", "server", "127.0.0.1:36100").c_str());
    SetWindowTextA(item(ID_USERNAME), g_ini.get("online", "username", "Princess").c_str());

    SetWindowTextA(item(ID_DEVFLASH), g_ini.get("paths", "devflash").c_str());
    SetWindowTextA(item(ID_DLCKEY), g_ini.get("paths", "dlckey").c_str());
    update_side_info();
    g_loading = false;
    g_ui_ready = true;
}

/* The DLC key as typed, cleaned to its hex digits; empty when it is not a
 * full 32-digit key (and *bad says whether something was typed). */
static std::string dlc_key_hex(bool* bad)
{
    std::string k = trim(text_of(ID_DLCKEY)), hex;
    for (char c : k) if (isxdigit((unsigned char)c)) hex += c;   /* spaces / dashes pasted along */
    if (bad) *bad = !hex.empty() && hex.size() != 32;
    return hex.size() == 32 ? hex : std::string();
}

/* Settings are saved as soon as they change (there is no Save button). */
static bool save_ini()
{
    if (!g_ui_ready || g_loading) return false;   /* see g_loading */
    g_ini.set("Graphics", "Renderer", "vulkan");   /* the D3D12 path draws incorrectly; no longer offered */
    g_ini.set("Graphics", "Fullscreen", combo_sel(ID_DISPLAY) == 1 ? "1" : "0");
    const int vs = combo_sel(ID_VSYNC);
    g_ini.set("Graphics", "VSync", vs == 1 ? "on" : vs == 2 ? "off" : "auto");
    const int g = combo_sel(ID_GPU);
    g_ini.set("Graphics", "GPU", g <= 0 ? "auto" : std::to_string(g - 1));
    { const int fl = combo_sel(ID_FRAMELIMIT);
      g_ini.set("Graphics", "FrameLimit", fl == 1 ? "60fps" : fl == 2 ? "unlocked" : "30"); }
    g_ini.set("Graphics", "ShowFPS", tog_get(ID_FPS) ? "1" : "0");
    for (int i = 0; i < kBinds; i++) g_ini.set("Controls", g_binds[i].key, g_binds[i].keys.empty() ? "None" : g_binds[i].keys);
    g_ini.set("Controls", "HideCoopStart", tog_get(ID_HIDECOOP) ? "1" : "0");
    g_ini.set("Controls", "MenuHints", tog_get(ID_MENUHINTS) ? "1" : "0");
    g_ini.set("Bots", "Mode", combo_sel(ID_BOTMODE) == 1 ? "improved" : "classic");
    g_ini.set("Bots", "Siege", tog_get(ID_BOT_SIEGE) ? "1" : "0");
    g_ini.set("Controls", "SkipHowToPlayPrompt", tog_get(ID_SKIPHOWTO) ? "1" : "0");
    { const int bi = combo_sel(ID_BUTTONICONS);
      g_ini.set("Controls", "ButtonIcons", bi == 1 ? "keyboard" : bi == 2 ? "xbox" : bi == 3 ? "ps" : "auto"); }
    const int pr = combo_sel(ID_PRIORITY);
    g_ini.set("Performance", "Priority", pr == 1 ? "belownormal" : pr == 2 ? "high" : "normal");
    g_ini.set("Performance", "FreeCores", std::to_string(atoi(text_of(ID_FREECORES).c_str())));
    g_ini.set("Performance", "Profile", tog_get(ID_PROFILE) ? "1" : "0");
    g_ini.set("Performance", "WaitMode", tog_get(ID_WAITSLEEP) ? "sleep" : "spin");
    g_ini.set("Performance", "SpuThreadReuse", tog_get(ID_THREADREUSE) ? "1" : "0");
    g_ini.set("Online", "Enabled", tog_get(ID_ONLINE) ? "1" : "0");
    g_ini.set("Launcher", "DarkMode", g_dark ? "1" : "0");
    g_ini.set("Online", "Server", text_of(ID_SERVER));
    g_ini.set("Online", "Username", text_of(ID_USERNAME));
    g_ini.set("Paths", "DevFlash", text_of(ID_DEVFLASH));
    /* A half-typed key is not saved, and a saved key is only blanked when the
     * field was cleared by hand -- never because the field came up empty. */
    { bool bad = false; const std::string hex = dlc_key_hex(&bad);
      if (!bad && (!hex.empty() || g_dlc_edited)) g_ini.set("Paths", "DlcKey", hex); }
    if (!g_ini.save(g_ini_path)) { set_status("Could not write " + g_ini_path); return false; }
    return true;
}

/* A setting changed: save it now. */
static void setting_changed()
{
    if (g_loading) return;
    if (save_ini()) set_status("Settings saved.");
}

static void update_paths_info()
{
    std::string df = text_of(ID_DEVFLASH);
    std::string shown = df.empty() ? detect_devflash() : df;
    std::string info;
    info += "Firmware (dev_flash): ";
    info += shown.empty() ? "NOT FOUND - audio codecs will be missing. Set it above."
                          : (dir_exists(shown + "\\sys\\external") ? shown + "  (ok)" : shown + "  (no sys\\external here?)");
    info += "\r\n\r\nGame data: ";
    info += file_exists(g_gamedir + "\\USRDIR\\GAME.elf") ? "GAME.elf ok" : "GAME.elf MISSING";
    info += file_exists(g_gamedir + "\\USRDIR\\siege.qrk") ? ", siege.qrk ok" : ", siege.qrk MISSING (use the button below)";
    info += "\r\n\r\nGame executable: ";
    info += file_exists(g_exe) ? "ok" : "not built yet - run build.bat";
    SetWindowTextA(item(ID_PATHS_INFO), info.c_str());
}

/* ------------------------------------------------------------------------
 * Launch
 * --------------------------------------------------------------------- */
static DWORD WINAPI wait_game(LPVOID p)
{
    HANDLE proc = (HANDLE)p;
    WaitForSingleObject(proc, INFINITE);
    DWORD code = 0; GetExitCodeProcess(proc, &code);
    PostMessageA(g_main, WM_APP_GAME_EXIT, code, 0);   /* the handle is closed there */
    return 0;
}

/* While the game runs, Play becomes Stop: ends a game that has hung or lost
 * its window, without a trip to Task Manager. */
static void stop_game()
{
    if (!g_running || !g_proc) return;
    if (MessageBoxA(g_main, "Stop the game now? Progress since the last save is lost.", "Fat Princess",
                    MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    TerminateProcess(g_proc, 1);
    set_status("Stopping the game...");
}

static void play()
{
    if (g_running) { stop_game(); return; }
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return;
    if (!save_ini()) { g_running = 0; return; }
    if (!file_exists(g_exe)) { MessageBoxA(g_main, "The game is not built yet. Run build.bat first.", "Fat Princess", MB_ICONWARNING); g_running = 0; return; }
    if (!file_exists(g_gamedir + "\\USRDIR\\GAME.elf")) { MessageBoxA(g_main, ("GAME.elf not found in\n" + g_gamedir + "\\USRDIR").c_str(), "Fat Princess", MB_ICONWARNING); g_running = 0; return; }
    if (!file_exists(g_gamedir + "\\USRDIR\\siege.qrk")) { MessageBoxA(g_main, "siege.qrk is missing. Use Paths > Copy siege.qrk from RPCS3.", "Fat Princess", MB_ICONWARNING); g_running = 0; return; }

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    const std::string log = g_root + "\\run.log";
    HANDLE out = CreateFileA(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOA si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out; si.hStdError = out; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = { 0 };
    std::string cmd = "\"" + g_exe + "\"";
    std::vector<char> cmdl(cmd.begin(), cmd.end()); cmdl.push_back(0);
    const BOOL ok = CreateProcessA(g_exe.c_str(), cmdl.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL,
                                   g_root.c_str(), &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) {
        set_status("Could not start the game (error " + std::to_string(GetLastError()) + ").");
        g_running = 0; return;
    }
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;
    HANDLE th = CreateThread(NULL, 0, wait_game, pi.hProcess, 0, NULL);
    if (th) CloseHandle(th);
    SetWindowTextA(g_play, "Stop game");
    set_status("Game running. F8 in the game writes a performance report; its log is run.log.");
}

static void copy_siege()
{
    std::string df = text_of(ID_DEVFLASH);
    if (df.empty()) df = detect_devflash();
    std::string src;
    if (!df.empty()) src = df + "\\..\\dev_hdd0\\game\\NPUA80139\\USRDIR\\siege.qrk";
    if (src.empty() || !file_exists(src)) {
        MessageBoxA(g_main, "Could not find siege.qrk in the RPCS3 install next to the firmware folder.\n"
                            "Copy RPCS3\\dev_hdd0\\game\\NPUA80139\\USRDIR\\siege.qrk into the game's\n"
                            "vfs\\dev_hdd0\\game\\NPUA80139\\USRDIR folder by hand.", "Fat Princess", MB_ICONWARNING);
        return;
    }
    set_status("Copying siege.qrk (357 MB)...");
    UpdateWindow(g_main);
    const std::string dst = g_gamedir + "\\USRDIR\\siege.qrk";
    if (CopyFileA(src.c_str(), dst.c_str(), FALSE)) set_status("siege.qrk copied.");
    else set_status("Copy failed (error " + std::to_string(GetLastError()) + ").");
    update_paths_info();
}

static void browse_devflash()
{
    BROWSEINFOA bi = { 0 };
    bi.hwndOwner = g_main;
    bi.lpszTitle = "Select the RPCS3 dev_flash folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return;
    char path[MAX_PATH];
    if (SHGetPathFromIDListA(pidl, path)) SetWindowTextA(item(ID_DEVFLASH), path);
    CoTaskMemFree(pidl);
    update_paths_info();
    setting_changed();
}

/* ------------------------------------------------------------------------
 * Window
 * --------------------------------------------------------------------- */
static void build_ui()
{
    const char* tabs[] = { "Graphics", "Controls", "Bots", "Performance", "Online", "Game files" };
    for (int i = 0; i < kTabs; i++)
        mk("BUTTON", tabs[i], BS_OWNERDRAW | WS_TABSTOP, kPanelX + i * (kTabW + 2), kTabY, kTabW, kTabH, ID_TAB0 + i, -1);
    const int X = kSidebar + 32, Y = 64;

    /* Graphics */
    label("Display", X, Y + 3, 150, 0);
    combo(ID_DISPLAY, X + 160, Y, 300, 0, { "Windowed", "Fullscreen (borderless)" });
    label("VSync", X, Y + 43, 150, 0);
    combo(ID_VSYNC, X + 160, Y + 40, 300, 0, { "Auto (no tearing, never waits)", "On (classic vsync)", "Off (may tear)" });
    label("Frame rate", X, Y + 83, 150, 0);
    combo(ID_FRAMELIMIT, X + 160, Y + 80, 460, 0, { "30 FPS (Original PS3 framerate - Stable)",
                                                    "60 FPS (Work-in-progress, testing - Unstable)",
                                                    "Unlocked (Work-in-progress, testing - Unstable)" });
    mk("STATIC", "", SS_LEFT, X + 160, Y + 114, 460, 36, ID_FRAMELIMIT_INFO, 0);
    label("Graphics card", X, Y + 163, 150, 0);
    std::vector<std::string> g = { "Auto" };
    for (const auto& s : g_gpus) g.push_back(s);
    combo(ID_GPU, X + 160, Y + 160, 460, 0, g);
    mk("BUTTON", "Show frame rate and performance in the window title", BS_OWNERDRAW | WS_TABSTOP,
       X + 160, Y + 198, 460, 28, ID_FPS, 0);
    label("Alt+Enter switches between windowed and full screen while playing.\r\n"
          "Resolution scaling and FSR are planned.", X, Y + 250, 620, 0, 40);

    /* Controls */
    HWND lv = mk(WC_LISTVIEWA, "", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                 X, Y, 620, 214, ID_BINDLIST, 1);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowSubclass(lv, list_subclass, 1, 0);
    LVCOLUMNA c = { 0 }; c.mask = LVCF_TEXT | LVCF_WIDTH;
    c.pszText = (char*)"PS3 button"; c.cx = S(140); SendMessageA(lv, LVM_INSERTCOLUMNA, 0, (LPARAM)&c);
    c.pszText = (char*)"What it does"; c.cx = S(250); SendMessageA(lv, LVM_INSERTCOLUMNA, 1, (LPARAM)&c);
    c.pszText = (char*)"Keys"; c.cx = S(205); SendMessageA(lv, LVM_INSERTCOLUMNA, 2, (LPARAM)&c);
    mk("BUTTON", "Set key...", BS_OWNERDRAW | WS_TABSTOP, X, Y + 222, 110, 30, ID_BIND_SET, 1);
    mk("BUTTON", "Add another...", BS_OWNERDRAW | WS_TABSTOP, X + 118, Y + 222, 120, 30, ID_BIND_ADD, 1);
    mk("BUTTON", "Clear", BS_OWNERDRAW | WS_TABSTOP, X + 246, Y + 222, 80, 30, ID_BIND_CLEAR, 1);
    mk("BUTTON", "Recommended layouts...", BS_OWNERDRAW | WS_TABSTOP, X + 334, Y + 222, 186, 30, ID_BIND_LAYOUTS, 1);
    mk("BUTTON", "Reset", BS_OWNERDRAW | WS_TABSTOP, X + 528, Y + 222, 92, 30, ID_BIND_RESET, 1);
    label("Button icons", X, Y + 267, 150, 1);
    combo(ID_BUTTONICONS, X + 160, Y + 264, 460, 1, { "Auto (your keys, or Xbox when a controller is plugged in)",
                                                      "Keyboard and mouse (shows your keys)",
                                                      "Xbox controller", "PlayStation (original)" });
    mk("BUTTON", "Show Move / Select / Back at the bottom of the menus", BS_OWNERDRAW | WS_TABSTOP,
       X, Y + 300, 620, 28, ID_MENUHINTS, 1);
    mk("BUTTON", "Skip the \"Noob Assistance Program\" how-to-play question", BS_OWNERDRAW | WS_TABSTOP,
       X, Y + 330, 620, 28, ID_SKIPHOWTO, 1);
    mk("BUTTON", "Hide the split-screen START signs for players 2-4", BS_OWNERDRAW | WS_TABSTOP,
       X, Y + 360, 620, 28, ID_HIDECOOP, 1);
    label("Double-click a row to set it: any key or mouse button, up to 4 each.", X, Y + 396, 620, 1, 20);

    /* Bots */
    label("Bot behaviour", X, Y + 3, 150, 2);
    combo(ID_BOTMODE, X + 160, Y, 460, 2, { "Classic (exactly as on the PS3)", "Improved (the options below)" });
    label("Improved bots", X, Y + 48, 300, 2);
    mk("BUTTON", "Siege: build ladders and springboards earlier and more often", BS_OWNERDRAW | WS_TABSTOP,
       X, Y + 72, 620, 28, ID_BOT_SIEGE, 2);
    label("Coming next: smarter workers, sensible hat swaps, weapon switching and specials,\r\n"
          "bombs and the chicken bomb, the catapult, the portal and the DLC orb.", X, Y + 112, 620, 2, 40);
    label("Bots only fill empty player slots. The same rules apply to both teams, and changes\r\n"
          "take effect the next time the game starts.", X, Y + 166, 620, 2, 40);

    /* Performance */
    label("Priority", X, Y + 3, 150, 3);
    combo(ID_PRIORITY, X + 160, Y, 300, 3, { "Normal", "Below normal (keeps the PC responsive)", "High" });
    label("CPU cores to leave free", X, Y + 43, 160, 3);
    mk("EDIT", "1", ES_NUMBER | WS_TABSTOP, X + 163, Y + 43, 57, 18, ID_FREECORES, 3);
    HWND spin = mk(UPDOWN_CLASSA, "", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS, 0, 0, 0, 0, ID_FREECORES_SPIN, 3);
    SendMessageA(spin, UDM_SETBUDDY, (WPARAM)item(ID_FREECORES), 0);
    SendMessageA(spin, UDM_SETRANGE32, 0, 16);
    label("0 uses every core; one or two left free keeps the desktop smooth.", X + 240, Y + 43, 400, 3);
    label("Experimental", X, Y + 86, 300, 3);
    mk("BUTTON", "Let waiting game threads sleep instead of spinning", BS_OWNERDRAW | WS_TABSTOP,
       X, Y + 110, 560, 28, ID_WAITSLEEP, 3);
    mk("BUTTON", "Reuse physics task threads", BS_OWNERDRAW | WS_TABSTOP, X, Y + 144, 560, 28, ID_THREADREUSE, 3);
    label("Both can free CPU time on a busy PC. Compare with F8 reports before keeping them on.",
          X, Y + 178, 620, 3);
    label("Measuring", X, Y + 216, 300, 3);
    label("F8 while playing records 30 seconds and writes perf_report.txt: frame rate, frame\r\n"
          "times and what each frame waited on. Send it along when reporting slowdowns.",
          X, Y + 240, 620, 3, 40);
    mk("BUTTON", "Open the last report", BS_OWNERDRAW | WS_TABSTOP, X, Y + 286, 200, 30, ID_REPORT_OPEN, 3);
    mk("BUTTON", "Detailed CPU profile (profile.txt) and F9 timeline", BS_OWNERDRAW | WS_TABSTOP,
       X, Y + 330, 560, 28, ID_PROFILE, 3);

    /* Online */
    mk("BUTTON", "Play online through a psnr matchmaking server", BS_OWNERDRAW | WS_TABSTOP, X, Y - 2, 420, 28, ID_ONLINE, 4);
    label("Server (host:port)", X, Y + 43, 150, 4);
    mk("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, X + 163, Y + 43, 294, 18, ID_SERVER, 4);
    label("Online name", X, Y + 83, 150, 4);
    mk("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, X + 163, Y + 83, 294, 18, ID_USERNAME, 4);
    label("3-16 characters, starting with a letter. Online play is still being brought up.", X, Y + 120, 620, 4, 40);

    /* Game files */
    label("Firmware dev_flash folder (empty = find it automatically)", X, Y, 620, 5);
    mk("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, X + 3, Y + 27, 514, 18, ID_DEVFLASH, 5);
    mk("BUTTON", "Browse...", BS_OWNERDRAW | WS_TABSTOP, X + 530, Y + 22, 110, 28, ID_DEVFLASH_BROWSE, 5);
    mk("STATIC", "", SS_LEFT, X, Y + 64, 620, 120, ID_PATHS_INFO, 5);
    mk("BUTTON", "Copy siege.qrk from RPCS3", BS_OWNERDRAW | WS_TABSTOP, X, Y + 200, 240, 30, ID_SIEGE_COPY, 5);
    label("Fat Roles DLC licence key (32 hex digits, from your own .rap for the expansion)", X, Y + 250, 620, 5);
    mk("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, X + 3, Y + 277, 514, 18, ID_DLCKEY, 5);

    for (int id : { ID_FREECORES, ID_SERVER, ID_USERNAME, ID_DEVFLASH, ID_DLCKEY }) edit_margins(item(id));

    /* Sidebar (the logo itself is painted in paint_background). Settings save
     * themselves as they change, so there is no Save button. */
    g_play = mk("BUTTON", "Play", BS_OWNERDRAW | WS_TABSTOP, 16, 214, 200, 46, ID_PLAY, -1);
    SendMessageA(g_play, WM_SETFONT, (WPARAM)g_bold, TRUE);
    mk("BUTTON", "Open log", BS_OWNERDRAW | WS_TABSTOP, 16, 272, 200, 32, ID_LOG, -1);
    mk("BUTTON", "Game folder", BS_OWNERDRAW | WS_TABSTOP, 16, 310, 200, 32, ID_FOLDER, -1);
    mk("BUTTON", "Dark mode", BS_OWNERDRAW | WS_TABSTOP, 18, 360, 196, 28, ID_DARK, -1);
    g_side_info = mk("STATIC", "", SS_CENTER, 8, 466, kSidebar - 16, 40, 0, -1);
    g_status = mk("STATIC", "", SS_LEFT | SS_ENDELLIPSIS, kSidebar + 14, 496, 692, 20, 0, -1);
}

static void paint_background(HWND h, HDC dc)
{
    RECT r; GetClientRect(h, &r);
    RECT side = r; side.right = S(kSidebar);
    RECT rest = r; rest.left = side.right;
    FillRect(dc, &side, g_side_brush);
    FillRect(dc, &rest, g_br_bg);
    RECT p = panel_rect();
    FillRect(dc, &p, g_br_panel);
    HBRUSH edge = CreateSolidBrush(P.border); FrameRect(dc, &p, edge);
    /* Frames for the flat fields on the visible page. */
    static const int framed[] = { ID_BINDLIST, ID_FREECORES, ID_SERVER, ID_USERNAME, ID_DEVFLASH, ID_DLCKEY };
    for (int id : framed) {
        HWND c = item(id);
        if (!c || !IsWindowVisible(c)) continue;
        RECT cr; GetWindowRect(c, &cr);
        if (id == ID_FREECORES) { RECT sr; GetWindowRect(item(ID_FREECORES_SPIN), &sr); UnionRect(&cr, &cr, &sr); }
        MapWindowPoints(NULL, h, (POINT*)&cr, 2);
        const int pad = id == ID_BINDLIST ? 1 : S(3);   /* edits: a little field colour around the text */
        InflateRect(&cr, pad, pad);
        FillRect(dc, &cr, g_br_field);
        FrameRect(dc, &cr, edge);
    }
    DeleteObject(edge);
    if (!g_logo) return;
    BITMAP bm; GetObject(g_logo, sizeof bm, &bm);
    const int dw = S(kSidebar - 16), dh = dw * bm.bmHeight / bm.bmWidth;
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, g_logo);
    if (dw == bm.bmWidth) {
        BitBlt(dc, S(8), S(12), dw, dh, mem, 0, 0, SRCCOPY);
    } else {
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchBlt(dc, S(8), S(12), dw, dh, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
    }
    SelectObject(mem, old);
    DeleteDC(mem);
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND: paint_background(h, (HDC)w); return 1;
    /* Themed buttons ask the parent to draw behind their rounded corners. */
    case WM_PRINTCLIENT: paint_background(h, (HDC)w); return 0;
    case WM_CTLCOLORBTN: return (LRESULT)zone_brush(zone_of((HWND)l));
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)w;
        SetTextColor(dc, P.text);
        SetBkColor(dc, P.field);
        return (LRESULT)g_br_field;
    }
    case WM_NOTIFY: {
        NMHDR* n = (NMHDR*)l;
        if (n->idFrom == ID_BINDLIST && n->code == NM_DBLCLK) bind_key(false);
        if (n->idFrom == ID_BINDLIST && n->code == LVN_ITEMCHANGED) {
            const NMLISTVIEW* lv = (const NMLISTVIEW*)l;
            if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED) && lv->iItem >= 0 && lv->iItem < kBinds)
                set_status(std::string(g_binds[lv->iItem].label) + ": " + kBindWhat[lv->iItem][1]);
        }
        if (n->idFrom == ID_BINDLIST && n->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)l;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                const int i = (int)cd->nmcd.dwItemSpec;
                const bool sel = ListView_GetItemState(n->hwndFrom, i, LVIS_SELECTED) != 0;
                cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_HOT);
                cd->clrTextBk = sel ? P.sel : (i & 1) ? P.stripe : P.field;
                cd->clrText = P.text;
                return CDRF_NEWFONT;
            }
            return CDRF_DODEFAULT;
        }
        return 0;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* d = (const DRAWITEMSTRUCT*)l;
        if (d->CtlType == ODT_BUTTON) draw_button(d);
        else if (d->CtlType == ODT_COMBOBOX) draw_combo(d);
        return TRUE;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mi = (MEASUREITEMSTRUCT*)l;
        if (mi->CtlType == ODT_COMBOBOX) mi->itemHeight = S(22);
        return TRUE;
    }
    case WM_COMMAND: {
        const int id = LOWORD(w), code = HIWORD(w);
        /* Owner-drawn buttons report a quick second click as BN_DOUBLECLICKED. */
        if (is_toggle(id) && (code == BN_CLICKED || code == BN_DOUBLECLICKED)) {
            tog_set(id, !tog_get(id));
            if (id != ID_DARK) setting_changed();
            if (id == ID_DARK) {
                g_dark = tog_get(id);
                apply_theme();
                g_ini.set("Launcher", "DarkMode", g_dark ? "1" : "0");
                Ini disk;   /* remember it now without saving the other, unconfirmed edits */
                if (!disk.load(g_ini_path)) disk.load_text(kDefaultIni);
                disk.set("Launcher", "DarkMode", g_dark ? "1" : "0");
                disk.save(g_ini_path);
            }
            return 0;
        }
        if (is_tab(id)) { if (code == BN_CLICKED) select_page(id - ID_TAB0); return 0; }
        if (code == CBN_SELCHANGE) {
            if (id == ID_FRAMELIMIT) update_framelimit_info();
            setting_changed();
            return 0;
        }
        if (id == ID_FREECORES && code == EN_CHANGE) { setting_changed(); return 0; }
        if (id == ID_DLCKEY && code == EN_CHANGE && !g_loading) { g_dlc_edited = true; return 0; }
        if (code == EN_KILLFOCUS) {
            if (id == ID_DEVFLASH) update_paths_info();
            if (id == ID_DLCKEY) {
                bool bad = false; dlc_key_hex(&bad);
                if (bad) MessageBoxA(h, "The DLC licence key must be 32 hex digits (0-9, A-F). It was not saved.",
                                     "Fat Princess", MB_ICONWARNING);
                update_side_info();
            }
            setting_changed();
            return 0;
        }
        if (code != BN_CLICKED) return 0;
        switch (id) {
        case ID_PLAY: play(); break;
        case ID_REPORT_OPEN: {
            const std::string r = g_root + "\\perf_report.txt";
            if (file_exists(r)) ShellExecuteA(h, "open", r.c_str(), NULL, NULL, SW_SHOW);
            else set_status("No report yet: press F8 while playing to record one.");
            break;
        }
        case ID_LOG: ShellExecuteA(h, "open", (g_root + "\\run.log").c_str(), NULL, NULL, SW_SHOW); break;
        case ID_FOLDER: ShellExecuteA(h, "open", g_root.c_str(), NULL, NULL, SW_SHOW); break;
        case ID_BIND_SET: bind_key(false); break;
        case ID_BIND_LAYOUTS: layouts_menu(); break;
        case ID_BIND_ADD: bind_key(true); break;
        case ID_BIND_CLEAR: {
            const int i = selected_bind();
            if (i >= 0) { g_binds[i].keys = "None"; refresh_bind_list(); save_ini(); set_status(std::string(g_binds[i].label) + " cleared (saved)."); }
            break;
        }
        case ID_BIND_RESET:
            if (MessageBoxA(h, "Reset every keyboard binding to the defaults?", "Fat Princess", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                for (int i = 0; i < kBinds; i++) g_binds[i].keys = g_defaults.get("controls", g_binds[i].key);
                refresh_bind_list(); save_ini(); set_status("Keyboard bindings reset to the defaults (saved).");
            }
            break;
        case ID_DEVFLASH_BROWSE: browse_devflash(); break;
        case ID_SIEGE_COPY: copy_siege(); break;
        }
        return 0;
    }
    case WM_APP_GAME_EXIT:
        if (g_proc) { CloseHandle(g_proc); g_proc = NULL; }
        g_running = 0;
        SetWindowTextA(g_play, "Play");
        set_status(w == 0 ? "Game closed." : ("Game exited with code " + std::to_string((int)w) +
                                              ". If it crashed, send run.log."));
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetBkMode(dc, TRANSPARENT);
        const Zone z = zone_of((HWND)l);
        SetTextColor(dc, z == Z_SIDE ? RGB(228, 234, 250) : (HWND)l == g_status ? P.muted : P.text);
        return (LRESULT)zone_brush(z);
    }
    case WM_CLOSE:
        save_ini();   /* a field still being edited has not lost focus yet */
        if (g_running && MessageBoxA(h, "The game is still running. Close the launcher anyway?\n(The game keeps running.)",
                                     "Fat Princess", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* Common controls v6 (themed buttons and lists) through an activation context,
 * so no manifest resource is needed in the build. */
static void enable_visual_styles()
{
    static const char manifest[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\r\n"
        "<dependency><dependentAssembly><assemblyIdentity type=\"win32\" name=\"Microsoft.Windows.Common-Controls\" "
        "version=\"6.0.0.0\" processorArchitecture=\"*\" publicKeyToken=\"6595b64144ccf1df\" language=\"*\"/>"
        "</dependentAssembly></dependency></assembly>\r\n";
    char tmp[MAX_PATH], path[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    snprintf(path, sizeof path, "%sfp_launcher.manifest", tmp);
    if (FILE* f = fopen(path, "wb")) { fwrite(manifest, 1, sizeof manifest - 1, f); fclose(f); }
    ACTCTXA ac = { sizeof ac };
    ac.lpSource = path;
    HANDLE h = CreateActCtxA(&ac);
    ULONG_PTR cookie = 0;
    if (h != INVALID_HANDLE_VALUE) ActivateActCtx(h, &cookie);
}

static void find_root()
{
    char b[MAX_PATH]; GetModuleFileNameA(NULL, b, MAX_PATH);
    std::string d = b;
    d = d.substr(0, d.find_last_of("\\/"));
    /* The launcher lives in the game folder or in its build\ subfolder. */
    if (!dir_exists(d + "\\vfs") && !file_exists(d + "\\run.bat")) {
        const std::string up = d.substr(0, d.find_last_of("\\/"));
        if (dir_exists(up + "\\vfs") || file_exists(up + "\\run.bat")) d = up;
    }
    g_root = d;
    g_ini_path = g_root + "\\fatprincess.ini";
    g_exe = g_root + "\\build\\FatPrincessRecomp.exe";
    g_gamedir = g_root + "\\vfs\\dev_hdd0\\game\\NPUA80139";
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show)
{
    SetProcessDPIAware();
    enable_visual_styles();
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ULONG_PTR gdip = 0;
    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&gdip, &gsi, NULL);

    find_root();
    g_defaults.load_text(kDefaultIni);
    if (!g_ini.load(g_ini_path)) g_ini.load_text(kDefaultIni);
    enumerate_gpus();
    g_dark = trim(g_ini.get("launcher", "darkmode", "1")) != "0";
    apply_theme();   /* palette and brushes; the controls are themed after build_ui */

    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY) / 96.0;
    ReleaseDC(NULL, dc);
    NONCLIENTMETRICSA ncm = { sizeof ncm };
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectA(&ncm.lfMessageFont);
    LOGFONTA bl = ncm.lfMessageFont; bl.lfWeight = FW_BOLD; bl.lfHeight = (LONG)(bl.lfHeight * 1.25);
    g_bold = CreateFontIndirectA(&bl);

    g_side_brush = CreateSolidBrush(kSideColor);
    g_logo = (HBITMAP)LoadImageA(inst, MAKEINTRESOURCEA(2), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    WNDCLASSEXA wc = { sizeof wc };
    wc.lpfnWndProc = main_proc; wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = NULL;   /* paint_background */
    wc.hIcon = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = "fp_launcher";
    RegisterClassExA(&wc);
    RECT r = { 0, 0, S(kSidebar + 720), S(524) };
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    AdjustWindowRect(&r, style, FALSE);
    g_main = CreateWindowExA(0, "fp_launcher", "Fat Princess Recomp - Launcher", style,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             NULL, NULL, inst, NULL);
    build_ui();
    ui_from_ini();
    { const char* t = getenv("FP_LAUNCHER_TAB");   /* screenshots / testing */
      select_page(t ? atoi(t) : 0); }
    apply_theme();
    set_status(file_exists(g_exe) ? "Ready." : "The game is not built yet - run build.bat.");
    ShowWindow(g_main, show);
    UpdateWindow(g_main);

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0)) {
        if (IsDialogMessageA(g_main, &m)) continue;
        TranslateMessage(&m); DispatchMessageA(&m);
    }
    return 0;
}
