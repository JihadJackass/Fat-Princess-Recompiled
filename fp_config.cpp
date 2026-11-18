/*
 * Fat Princess Recomp - user settings (fatprincess.ini)
 *
 * Read once at startup, before the renderer, input or network start. Settings
 * become the runtime's environment switches (set only when not already set, so
 * a variable set by hand still wins for testing) plus the keyboard bindings.
 * A missing file is written out with the defaults and comments, so there is
 * always one to edit -- by hand now, by the launcher later.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

extern "C" void cellPad_keyboard_bind(int target, const int* vks, int n);

namespace {

#include "fp_ini_defaults.h"

struct Entry { std::string section, key, value; };

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::vector<Entry> parse(const std::string& text)
{
    std::vector<Entry> out;
    std::string section;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = trim(text.substr(pos, e - pos));
        pos = e + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            size_t r = line.find(']');
            section = lower(trim(line.substr(1, r == std::string::npos ? std::string::npos : r - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out.push_back({ section, lower(trim(line.substr(0, eq))), trim(line.substr(eq + 1)) });
    }
    return out;
}

const char* get(const std::vector<Entry>& es, const char* sec, const char* key)
{
    for (auto it = es.rbegin(); it != es.rend(); ++it)
        if (it->section == sec && it->key == key) return it->value.c_str();
    return nullptr;
}

void set_default_env(const char* name, const char* value)
{
    if (!value || getenv(name)) return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}

bool truthy(const char* v)
{
    if (!v) return false;
    std::string s = lower(v);
    return s == "1" || s == "on" || s == "yes" || s == "true";
}

#ifdef _WIN32
int key_code(const std::string& name)
{
    std::string n = lower(trim(name));
    if (n.empty() || n == "none") return 0;
    if (n.size() == 1) {
        const char c = (char)toupper((unsigned char)n[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
        switch (c) {
        case ',': return VK_OEM_COMMA;  case '.': return VK_OEM_PERIOD;
        case '/': return VK_OEM_2;      case ';': return VK_OEM_1;
        case '\'': return VK_OEM_7;     case '[': return VK_OEM_4;
        case ']': return VK_OEM_6;      case '-': return VK_OEM_MINUS;
        case '=': return VK_OEM_PLUS;   case '\\': return VK_OEM_5;
        case '`': return VK_OEM_3;
        default: return 0;
        }
    }
    if (n[0] == 'f' && n.size() <= 3 && isdigit((unsigned char)n[1])) {
        const int f = atoi(n.c_str() + 1);
        if (f >= 1 && f <= 12) return VK_F1 + f - 1;
    }
    if (n.compare(0, 6, "numpad") == 0 && n.size() == 7 && isdigit((unsigned char)n[6]))
        return VK_NUMPAD0 + (n[6] - '0');
    static const struct { const char* name; int vk; } names[] = {
        { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT },
        /* Mouse buttons: read with the keys (GetAsyncKeyState), while the game
         * window has focus. */
        { "mouseleft", VK_LBUTTON }, { "mouseright", VK_RBUTTON }, { "mousemiddle", VK_MBUTTON },
        { "mouse4", VK_XBUTTON1 }, { "mouse5", VK_XBUTTON2 },
        { "space", VK_SPACE }, { "enter", VK_RETURN }, { "return", VK_RETURN },
        { "tab", VK_TAB }, { "escape", VK_ESCAPE }, { "esc", VK_ESCAPE },
        { "backspace", VK_BACK }, { "shift", VK_SHIFT }, { "ctrl", VK_CONTROL },
        { "control", VK_CONTROL }, { "alt", VK_MENU }, { "lshift", VK_LSHIFT },
        { "rshift", VK_RSHIFT }, { "lctrl", VK_LCONTROL }, { "rctrl", VK_RCONTROL },
        { "lalt", VK_LMENU }, { "ralt", VK_RMENU }, { "insert", VK_INSERT },
        { "delete", VK_DELETE }, { "home", VK_HOME }, { "end", VK_END },
        { "pageup", VK_PRIOR }, { "pagedown", VK_NEXT }, { "capslock", VK_CAPITAL },
        { "comma", VK_OEM_COMMA }, { "period", VK_OEM_PERIOD }, { "slash", VK_OEM_2 },
        { "semicolon", VK_OEM_1 }, { "quote", VK_OEM_7 }, { "lbracket", VK_OEM_4 },
        { "rbracket", VK_OEM_6 }, { "minus", VK_OEM_MINUS }, { "equals", VK_OEM_PLUS },
        { "backslash", VK_OEM_5 }, { "grave", VK_OEM_3 },
    };
    for (const auto& e : names) if (n == e.name) return e.vk;
    fprintf(stderr, "[config] unknown key name '%s' (ignored)\n", name.c_str());
    return 0;
}

void bind_controls(const std::vector<Entry>& es)
{
    /* Target numbers: CELL_PAD_CTRL_* bit numbers, then 16..23 = sticks. */
    static const struct { const char* key; int target; } t[] = {
        { "select", 0 }, { "l3", 1 }, { "r3", 2 }, { "start", 3 },
        { "dpadup", 4 }, { "dpadright", 5 }, { "dpaddown", 6 }, { "dpadleft", 7 },
        { "l2", 8 }, { "r2", 9 }, { "l1", 10 }, { "r1", 11 },
        { "triangle", 12 }, { "circle", 13 }, { "cross", 14 }, { "square", 15 },
        { "leftstickup", 16 }, { "leftstickdown", 17 }, { "leftstickleft", 18 },
        { "leftstickright", 19 }, { "rightstickup", 20 }, { "rightstickdown", 21 },
        { "rightstickleft", 22 }, { "rightstickright", 23 },
    };
    int bound = 0;
    for (const auto& b : t) {
        const char* v = get(es, "controls", b.key);
        if (!v) continue;
        int vks[4] = { 0, 0, 0, 0 }, n = 0;
        std::string s = v;
        size_t p = 0;
        while (p <= s.size() && n < 4) {
            size_t c = s.find(',', p);
            if (c == std::string::npos) c = s.size();
            const int vk = key_code(s.substr(p, c - p));
            if (vk) vks[n++] = vk;
            p = c + 1;
        }
        cellPad_keyboard_bind(b.target, vks, n);
        bound++;
    }
    fprintf(stderr, "[config] %d keyboard binding(s) from fatprincess.ini\n", bound);
}
#endif

std::string read_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return std::string();
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

std::string g_elf;

bool dir_exists(const std::string& p)
{
#ifdef _WIN32
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    FILE* f = fopen((p + "/.").c_str(), "rb"); if (f) fclose(f); return f != nullptr;
#endif
}

void make_dirs(const std::string& p)
{
#ifdef _WIN32
    std::string cur;
    for (size_t i = 0; i < p.size(); i++) {
        cur += p[i];
        if ((p[i] == '\\' || p[i] == '/') && cur.size() > 3) CreateDirectoryA(cur.c_str(), NULL);
    }
    CreateDirectoryA(p.c_str(), NULL);
#else
    (void)p;
#endif
}

std::string cwd()
{
#ifdef _WIN32
    char b[MAX_PATH]; DWORD n = GetCurrentDirectoryA(MAX_PATH, b);
    return n ? std::string(b, n) : std::string(".");
#else
    return ".";
#endif
}

/* Everything run.bat used to export, derived from the game folder (the
 * current directory), so the exe runs the same from the launcher, run.bat or
 * a double-click. Each only fills what is not already set. */
void runtime_defaults(const std::vector<Entry>& es)
{
    const std::string root = cwd();
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    auto J = [&](const std::string& a, const char* b) { std::string r = a; r += sep; r += b; return r; };
    const std::string hdd0 = J(J(root, "vfs"), "dev_hdd0");
    const std::string game = J(J(hdd0, "game"), "NPUA80139");
    g_elf = J(J(game, "USRDIR"), "GAME.elf");
    set_default_env("PS3_HDD0_ROOT", hdd0.c_str());
    set_default_env("PS3_HDD1_ROOT", J(J(root, "vfs"), "dev_hdd1").c_str());
    set_default_env("PS3_VFS_ROOT", J(game, "USRDIR").c_str());
    set_default_env("PS3_TITLE", "Fat Princess");
    set_default_env("SPU_INTERP_UNLIFTED", "1");   /* any runtime-loaded SPU code not lifted */
    set_default_env("PS3_VERBOSE", "0");
    set_default_env("PS3RECOMP_VK_GUEST_PROGRAMS", "1");
    set_default_env("VK_DRAW_BATCH", "1");
    const std::string cache = J(root, "shadercache");
    make_dirs(cache);
    set_default_env("PS3RECOMP_SPIRV_CACHE", cache.c_str());
    make_dirs(J(J(root, "vfs"), "dev_hdd1"));
    make_dirs(J(J(J(J(hdd0, "home"), "00000001"), "savedata"), ""));

    /* Firmware dev_flash: [Paths] DevFlash, else the usual RPCS3 places. */
    std::string df;
    if (const char* v = get(es, "paths", "devflash")) df = v;
    if (const char* v = get(es, "paths", "dlckey"))
        if (strlen(v) == 32) set_default_env("PS3_RIF_KEY", v);
#ifdef _WIN32
    if (df.empty()) {
        const char* bases[] = { getenv("USERPROFILE"), getenv("LOCALAPPDATA"), getenv("APPDATA"),
                                getenv("ProgramFiles") };
        const char* subs[][2] = { { "Desktop\\RPCS3", "" }, { "Downloads\\RPCS3", "" },
                                  { "RPCS3", "" }, { "rpcs3", "" } };
        for (const char* b : bases) {
            if (!b || !df.empty()) continue;
            for (auto& sub : subs) {
                std::string c = std::string(b) + "\\" + sub[0] + "\\dev_flash";
                if (dir_exists(c + "\\sys\\external")) { df = c; break; }
            }
        }
        if (df.empty() && dir_exists("C:\\RPCS3\\dev_flash\\sys\\external")) df = "C:\\RPCS3\\dev_flash";
    }
#endif
    if (!df.empty()) set_default_env("PS3_DEV_FLASH", df.c_str());
    else fprintf(stderr, "[config] WARNING: no PS3 firmware dev_flash found; set [Paths] DevFlash. "
                         "Audio codecs will be missing.\n");

#ifdef _WIN32
    /* [Performance] */
    if (const char* pr = get(es, "performance", "priority")) {
        const std::string p = lower(pr);
        DWORD cls = p == "belownormal" ? BELOW_NORMAL_PRIORITY_CLASS
                  : p == "high" ? HIGH_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS;
        SetPriorityClass(GetCurrentProcess(), cls);
    }
    if (const char* fc = get(es, "performance", "freecores")) {
        const int free_n = atoi(fc);
        DWORD_PTR proc = 0, sys = 0;
        if (free_n > 0 && GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) {
            int n = 0; for (DWORD_PTR m = proc; m; m >>= 1) n += (int)(m & 1);
            int keep = n - free_n; if (keep < 2) keep = 2;
            DWORD_PTR mask = 0; int k = 0;
            for (int bit = 0; bit < (int)(sizeof(DWORD_PTR) * 8) && k < keep; bit++)
                if (proc & ((DWORD_PTR)1 << bit)) { mask |= (DWORD_PTR)1 << bit; k++; }
            if (mask) SetProcessAffinityMask(GetCurrentProcess(), mask);
            fprintf(stderr, "[config] using %d of %d logical cores\n", k, n);
        }
    }
#endif
}

} // namespace

/* The game ELF under the game folder, for a launch with no argument. */
extern "C" const char* fp_config_default_elf(void) { return g_elf.c_str(); }

/* Call first thing in main(). */
extern "C" void fp_config_load(void)
{
    const char* path = getenv("FP_CONFIG");
    if (!path || !*path) path = "fatprincess.ini";
    std::string text = read_file(path);
    if (text.empty()) {
        if (FILE* f = fopen(path, "wb")) {
            fwrite(kDefaultIni, 1, sizeof kDefaultIni - 1, f);
            fclose(f);
            fprintf(stderr, "[config] wrote default %s\n", path);
        }
        text = kDefaultIni;
    }
    const std::vector<Entry> es = parse(text);

    /* [Graphics] */
    if (const char* r = get(es, "graphics", "renderer"))
        if (lower(r) == "d3d12") set_default_env("PS3_RENDERER", "d3d12");
    if (truthy(get(es, "graphics", "fullscreen"))) set_default_env("PS3RECOMP_VK_FULLSCREEN", "1");
    if (const char* v = get(es, "graphics", "vsync")) {
        const std::string s = lower(v);
        if (s == "on" || s == "1") set_default_env("PS3RECOMP_VK_VSYNC", "1");
        else if (s == "off" || s == "0") set_default_env("PS3RECOMP_VK_VSYNC", "0");
    }
    /* FrameLimit: 30 (default; also auto and older values) = the original
     * 30 FPS. 60fps and unlocked are experimental. A number below 59 caps at
     * that many FPS. */
    {
        const char* v = get(es, "graphics", "framelimit");
        if (!v) v = "30";
        if (lower(v) == "unlocked") {
            /* Unlocked: no refresh pacing of the present, and the game's own
             * wait for two refreshes is answered at once (cellGcmSys.c). */
            set_default_env("FP_FRAME_PACING", "unlocked");
            set_default_env("FP_FPS_MODE", "unlocked");
        }
        /* 60fps: release a frame every refresh (cellGcmSys.c, FP_FPS_MODE).
         * Experimental: the game steps its logic a fixed amount per frame, so
         * it runs at double speed until that step is halved too. */
        else if (lower(v) == "60fps") set_default_env("FP_FPS_MODE", "60");
        else if (atoi(v) > 0 && atoi(v) < 59) set_default_env("FP_FRAME_LIMIT", v);
        /* 30 (the launcher's "30 FPS"), auto, or anything older: the original
         * 30 FPS, held by a cap on the console's 29.97 beat as well as by the
         * game's own vblank wait, which alone lets some screens run at 60. */
        else set_default_env("FP_FRAME_LIMIT", "30");
    }
    if (const char* v = get(es, "performance", "profile"))
        if (truthy(v)) {
            set_default_env("FP_PROFILE", "1");
            /* The frame timeline, armed with a start far off; F9 in the game
             * window moves it to now (main.cpp ftrace_hotkey). */
            set_default_env("PS3_FTRACE", "1000000:10:ftrace.txt");
        }
    /* WaitMode: spin (default) or sleep -- how the game's short polling
     * sleeps wait (sys_timer.c). SpuThreadReuse: reuse idle SPU task threads
     * (spu_workload.c). Both are here to compare with F8's perf_report.txt. */
    if (const char* v = get(es, "performance", "waitmode"))
        if (lower(v) == "sleep") set_default_env("FP_USLEEP_MODE", "sleep");
    if (truthy(get(es, "performance", "sputhreadreuse"))) set_default_env("SPU_THREAD_REUSE", "1");
    /* [Controls] HideCoopStart (default on): hide the split-screen "START"
     * join signs for players 2-4 (fp_data_patches.cpp). */
    { const char* v = get(es, "controls", "hidecoopstart");
      if (!v || truthy(v)) set_default_env("FP_HIDE_COOP_START", "1"); }
    /* SkipHowToPlayPrompt (default off, as on the PS3): no "Noob Assistance
     * Program" question before each mode. MenuHints (default on): a "Move /
     * Select / Back" line in the menus. ButtonIcons: auto (keyboard keys, or
     * Xbox buttons when a controller is plugged in), keyboard, xbox or ps --
     * all in fp_data_patches.cpp / fp_button_icons.cpp. */
    if (truthy(get(es, "controls", "skiphowtoplayprompt"))) set_default_env("FP_SKIP_HOWTO_PROMPT", "1");
    /* [Bots] Mode = classic (default, the PS3 behaviour) or improved; under
     * improved each change has its own switch (fp_data_patches.cpp). */
    { const char* m = get(es, "bots", "mode");
      if (m && lower(trim(m)) == "improved") {
          const char* v = get(es, "bots", "siege");
          if (!v || truthy(v)) set_default_env("FP_BOTS_SIEGE", "1");
      } }
    { const char* v = get(es, "controls", "menuhints");
      if (!v || truthy(v)) set_default_env("FP_MENU_HINTS", "1"); }
    { const char* v = get(es, "controls", "buttonicons");
      set_default_env("FP_BUTTON_ICONS", v && *v ? v : "auto"); }
    {
        /* The first key of each button, for the keyboard pictures. Bindings
         * missing from the file are the defaults (as cellPad has them). */
        const std::vector<Entry> defs = parse(kDefaultIni);
        static const struct { const char* ini; const char* id; } b[] = {
            { "cross", "cross" }, { "circle", "circle" }, { "square", "square" }, { "triangle", "triangle" },
            { "l1", "l1" }, { "r1", "r1" }, { "l2", "l2" }, { "r2", "r2" }, { "l3", "l3" }, { "r3", "r3" },
            { "dpadup", "dpadup" }, { "dpadright", "dpadright" }, { "dpaddown", "dpaddown" }, { "dpadleft", "dpadleft" },
            { "leftstickup", "lsup" }, { "leftstickright", "lsright" }, { "leftstickdown", "lsdown" },
            { "leftstickleft", "lsleft" },
        };
        std::string keys;
        for (const auto& x : b) {
            const char* v = get(es, "controls", x.ini);
            if (!v) v = get(defs, "controls", x.ini);
            std::string first = v ? trim(std::string(v).substr(0, std::string(v).find(','))) : "None";
            if (first.empty()) first = "None";
            keys += std::string(keys.empty() ? "" : ";") + x.id + "=" + first;
        }
        set_default_env("FP_BUTTON_KEYS", keys.c_str());
    }
    if (const char* v = get(es, "graphics", "showfps"))
        if (!truthy(v)) set_default_env("FP_SHOW_FPS", "0");
    if (const char* v = get(es, "graphics", "gpu"))
        if (lower(v) != "auto" && *v) set_default_env("PS3RECOMP_VK_DEVICE", v);

    /* [Online] */
    if (truthy(get(es, "online", "enabled"))) {
        set_default_env("PS3_NET_ONLINE", "1");
        set_default_env("PSNR_SERVER", get(es, "online", "server"));
        set_default_env("PS3_NP_ONLINE_ID", get(es, "online", "username"));
    }

    runtime_defaults(es);
#ifdef _WIN32
    bind_controls(es);
#endif
    fprintf(stderr, "[config] loaded %s (%zu setting(s))\n", path, es.size());
}
