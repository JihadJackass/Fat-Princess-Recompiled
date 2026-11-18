/*
 * fp_data_patches.cpp -- small, optional changes to the game's own data.
 *
 * The game keeps its scripts, menus and textures in siege.qrk. A patch here
 * rewrites one entry of that archive (a script line, later a texture), and
 * the game is given a patched copy of the archive instead of the original:
 *
 *   - the original is copied once to <game folder>/cache/siege_patched.qrk;
 *   - each patched entry is appended to the copy, stored uncompressed, and its
 *     table-of-contents record is pointed at it;
 *   - cache/siege_patched.key records which patches and which original the
 *     copy was made from, so it is rebuilt only when either changes.
 *
 * Nothing changes when no patch is turned on: the game opens the original.
 * Patches are chosen in fatprincess.ini (fp_config.cpp sets the variables):
 *
 *   FP_HIDE_COOP_START=1   [Controls] HideCoopStart=1: the split-screen
 *                           "START" join signs for players 2-4 stay hidden
 *                           (players can still join with Start on a pad).
 *   FP_SKIP_HOWTO_PROMPT=1 [Controls] SkipHowToPlayPrompt=1: no "Noob
 *                           Assistance Program" question before each mode.
 *   FP_BUTTON_ICONS=...    [Controls] ButtonIcons: Xbox or keyboard pictures
 *                           for the button prompts (fp_button_icons.cpp).
 *   FP_MENU_HINTS=1        [Controls] MenuHints=1: a "Move / Select / Back"
 *                           line at the bottom of the menus.
 *   FP_BOTS_SIEGE=1        [Bots] Mode=improved, Siege=1: bots build ladders
 *                           and springboards earlier and more often.
 *
 * When only the set of patches changes, the existing copy is reused: its
 * header and table of contents are restored from the original, the appended
 * entries cut off, and the new ones appended -- no second full copy.
 *
 * The archive format is described in tools/qrk.py. Entries are zlib streams
 * (flag 1) or stored (flag 0); a small inflate below reads the former.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <functional>
#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#  include <io.h>
#  define FP_MKDIR(p) _mkdir(p)
#else
#  include <unistd.h>
#  define FP_MKDIR(p) mkdir(p, 0755)
#endif

/* fp_button_icons.cpp */
std::string fp_button_icons_id();
bool fp_button_icons_patch(const std::string& entry, std::string& gtf);
const char* const* fp_button_icons_entries();

namespace {

/* ---- inflate (RFC 1951), enough for the archive's zlib streams ------------- */
struct Bits {
    const uint8_t* p; size_t n, i = 0; uint32_t buf = 0; int cnt = 0; bool bad = false;
    uint32_t get(int k)
    {
        while (cnt < k) {
            if (i >= n) { bad = true; return 0; }
            buf |= (uint32_t)p[i++] << cnt; cnt += 8;
        }
        const uint32_t v = buf & ((1u << k) - 1u);
        buf >>= k; cnt -= k;
        return v;
    }
};
struct Huff { uint16_t count[16]; uint16_t sym[320]; };
static bool huff_build(Huff* h, const uint8_t* len, int n)
{
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    uint16_t offs[16]; offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (uint16_t)i;
    return true;
}
static int huff_decode(Bits& b, const Huff* h)
{
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= (int)b.get(1);
        const int c = h->count[l];
        if (code - c < first) return h->sym[index + (code - first)];
        index += c; first += c; first <<= 1; code <<= 1;
        if (b.bad) return -1;
    }
    return -1;
}
static const uint16_t kLenBase[] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t  kLenExtra[] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t kDistBase[] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,
                                      4097,6145,8193,12289,16385,24577 };
static const uint8_t  kDistExtra[] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static bool inflate_block(Bits& b, std::string& out, const Huff* lit, const Huff* dist)
{
    for (;;) {
        const int s = huff_decode(b, lit);
        if (s < 0) return false;
        if (s < 256) { out.push_back((char)s); continue; }
        if (s == 256) return true;
        const int li = s - 257;
        if (li >= 29) return false;
        const size_t len = kLenBase[li] + b.get(kLenExtra[li]);
        const int di = huff_decode(b, dist);
        if (di < 0 || di >= 30) return false;
        const size_t d = kDistBase[di] + b.get(kDistExtra[di]);
        if (b.bad || d > out.size()) return false;
        const size_t from = out.size() - d;
        for (size_t k = 0; k < len; k++) out.push_back(out[from + k]);
    }
}

/* zlib (2-byte header) or raw deflate. Returns false on a damaged stream. */
static bool zinflate(const uint8_t* p, size_t n, std::string& out, size_t expect)
{
    out.clear(); out.reserve(expect);
    Bits b{ p, n };
    if (n >= 2 && (p[0] & 0x0F) == 8 && ((p[0] << 8) | p[1]) % 31 == 0) b.i = 2;
    int final = 0;
    do {
        final = (int)b.get(1);
        const int type = (int)b.get(2);
        if (b.bad) return false;
        if (type == 0) {
            b.buf = 0; b.cnt = 0;   /* byte-align */
            if (b.i + 4 > n) return false;
            const uint16_t len = (uint16_t)(p[b.i] | (p[b.i + 1] << 8));
            b.i += 4;
            if (b.i + len > n) return false;
            out.append((const char*)p + b.i, len); b.i += len;
        } else if (type == 1) {
            static Huff lit, dist; static bool made = false;
            if (!made) {
                uint8_t l[288];
                for (int i = 0; i < 144; i++) l[i] = 8;
                for (int i = 144; i < 256; i++) l[i] = 9;
                for (int i = 256; i < 280; i++) l[i] = 7;
                for (int i = 280; i < 288; i++) l[i] = 8;
                huff_build(&lit, l, 288);
                uint8_t dl[30]; for (int i = 0; i < 30; i++) dl[i] = 5;
                huff_build(&dist, dl, 30);
                made = true;
            }
            if (!inflate_block(b, out, &lit, &dist)) return false;
        } else if (type == 2) {
            const int hlit = (int)b.get(5) + 257, hdist = (int)b.get(5) + 1, hclen = (int)b.get(4) + 4;
            static const uint8_t order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            uint8_t cl[19] = { 0 };
            for (int i = 0; i < hclen; i++) cl[order[i]] = (uint8_t)b.get(3);
            Huff ch; huff_build(&ch, cl, 19);
            uint8_t lens[320] = { 0 };
            int k = 0;
            while (k < hlit + hdist) {
                const int s = huff_decode(b, &ch);
                if (s < 0) return false;
                if (s < 16) { lens[k++] = (uint8_t)s; continue; }
                int rep = 0; uint8_t v = 0;
                if (s == 16) { if (!k) return false; v = lens[k - 1]; rep = 3 + (int)b.get(2); }
                else if (s == 17) rep = 3 + (int)b.get(3);
                else rep = 11 + (int)b.get(7);
                if (k + rep > hlit + hdist) return false;
                while (rep--) lens[k++] = v;
            }
            Huff lit, dist;
            huff_build(&lit, lens, hlit);
            huff_build(&dist, lens + hlit, hdist);
            if (!inflate_block(b, out, &lit, &dist)) return false;
        } else {
            return false;
        }
    } while (!final);
    return !b.bad;
}

/* ---- the archive ------------------------------------------------------------- */
struct Entry { std::string name; uint32_t csize, off, size, hash, nameoff; uint8_t comp; size_t toc_pos; };
struct Archive { uint32_t count = 0, datastart = 0, nsz = 0; std::vector<Entry> e; };

static bool read_at(FILE* f, uint64_t off, void* dst, size_t n)
{
#ifdef _WIN32
    if (_fseeki64(f, (int64_t)off, SEEK_SET)) return false;
#else
    if (fseeko(f, (off_t)off, SEEK_SET)) return false;
#endif
    return fread(dst, 1, n, f) == n;
}
static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put_le32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static bool load_toc(FILE* f, Archive& a)
{
    uint8_t h[24];
    if (!read_at(f, 0, h, 24) || memcmp(h, "QRK", 3)) return false;
    a.count = le32(h + 8); a.datastart = le32(h + 12); a.nsz = le32(h + 20);
    if (a.count > 200000 || a.nsz > (64u << 20)) return false;
    std::vector<uint8_t> names(a.nsz), toc((size_t)a.count * 21);
    if (!read_at(f, 24, names.data(), a.nsz) || !read_at(f, 24 + a.nsz, toc.data(), toc.size())) return false;
    a.e.resize(a.count);
    for (uint32_t i = 0; i < a.count; i++) {
        const uint8_t* t = &toc[(size_t)i * 21];
        Entry& x = a.e[i];
        x.csize = le32(t); x.off = le32(t + 4); x.size = le32(t + 8); x.hash = le32(t + 12); x.nameoff = le32(t + 16);
        x.comp = t[20]; x.toc_pos = 24 + a.nsz + (size_t)i * 21;
        if (x.nameoff < a.nsz) x.name.assign((const char*)names.data() + x.nameoff, strnlen((const char*)names.data() + x.nameoff, a.nsz - x.nameoff));
    }
    return true;
}
static bool entry_data(FILE* f, const Entry& x, std::string& out)
{
    std::vector<uint8_t> raw(x.csize);
    if (!read_at(f, x.off, raw.data(), raw.size())) return false;
    if (!x.comp) { out.assign((const char*)raw.data(), raw.size()); return true; }
    return zinflate(raw.data(), raw.size(), out, x.size) && out.size() == x.size;
}

/* ---- the patches ----------------------------------------------------------------- */
struct Patch { std::string entry, id; std::function<bool(std::string&)> apply; };

static bool replace_all(std::string& s, const std::string& from, const std::string& to)
{
    bool any = false;
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) {
        s.replace(p, from.size(), to); any = true;
    }
    return any;
}
/* CoopShowStartP2..4 still run (they clear the respawn timer and set the
 * visibility symbol other scripts test); only the sign itself stays hidden. */
static bool hide_coop_start(std::string& t)
{
    bool ok = true;
    for (int p = 2; p <= 4; p++) {
        const std::string n = std::to_string(p);
        ok &= replace_all(t, "cmd \"gameUI show CoopStartButton" + n + "\"",
                             "cmd \"gameUI hide CoopStartButton" + n + "\"");
    }
    return ok;
}

static bool env_on(const char* name)
{
    const char* e = getenv(name);
    return e && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y' || e[0] == 'o' || e[0] == 'O');
}

/* The "Noob Assistance Program" question comes up each time a mode is chosen
 * until all seven How to Play pages have been read. The game has its own way
 * to stop asking (HowToPlaySupress, set after two refusals in one session);
 * this starts every session with it set. The How to Play pages stay in the
 * menu, and reading them all still marks them complete. */
static bool skip_howto_prompt(std::string& t)
{
    return replace_all(t, "sym init HowToPlaySupress 0", "sym init HowToPlaySupress 1");
}

/* Menu control hints: one line of text at the bottom of the front-end menus,
 * "Move / Select / Back" with the button glyphs, which fp_button_icons.cpp
 * turns into the player's keys or Xbox buttons. A new menu object (defined
 * like the game's own watermark text), placed in the master menu layout, and
 * one new string in the English string tables. */
static const char kNavObject[] =
    "add MenuObject\r\n"
    "set Name FPNavHelp\r\n"
    "set Coords \"0 0 1000 40\"\r\n"
    "set Layer 7\r\n"
    "set RenderCB _DrawUText\r\n"
    "set RenderCBExcludesDefaultRender 1\r\n"
    "set RenderCB_DX 0\r\n"
    "set RenderCB_DY 0\r\n"
    "set RenderCB_FontName FatFingersOutlined\r\n"
    "set RenderCB_RGBColor \"1 1 1\"\r\n"
    "set RenderCB_FontScale 0.6\r\n"
    "set RenderCB_Text FPMenuNavHelp\r\n"
    "set RenderCB_FloatArg 2000.0\r\n"
    "set RenderCB_BoolArg 1\r\n"
    "set RenderCB_TextJustify JustifyXCenter\r\n"
    "set RenderCB_TextJustifyVertical JustifyYCenter\r\n"
    "set Anchor Center\r\n";
static bool nav_object(std::string& t)
{
    if (t.find("set Name FPNavHelp") != std::string::npos) return false;
    if (!t.empty() && t.back() != '\n') t += "\r\n";
    t += kNavObject;
    return true;
}
static bool nav_layout(std::string& t)
{
    if (t.find("MO FPNavHelp") != std::string::npos) return false;
    if (!t.empty() && t.back() != '\n') t += "\r\n";
    t += "MO FPNavHelp {\r\npos -150 318\r\n}\r\n";
    return true;
}
/* The string tables are UTF-16 big-endian with a byte-order mark. */
static bool nav_string(std::string& t)
{
    if (t.size() < 2 || (uint8_t)t[0] != 0xFE || (uint8_t)t[1] != 0xFF) return false;
    const char16_t line[] = u"\r\nFPMenuNavHelp=\u25B2 \u25BC \u25C4 \u25BA Move     \u2666 Select     \u25CB Back\r\n";
    for (const char16_t* c = line; *c; c++) { t += (char)(*c >> 8); t += (char)(*c & 0xFF); }
    return true;
}

/* Improved bots, siege: the bot settings list each thing a worker can build
 * as "Build_ODn = name c1..c5 f6 f7 f8 f9 ...". Comparing the game's own modes:
 * f6 behaves as a delay in seconds and f9 as the base weight. Normal modes
 * give ladders 600 s / 200 and springboards 300 s / 240, so bots barely
 * siege; the story mode tuning uses 300 s / 375. Improved: at most 180 s and
 * at least the story-mode weight. A weight of 0 (the designers switching a
 * build off for a mode) is kept. */
static const char* const kBotFiles[] = {
    "data/gamescripts/ai/aisettings_default.txt", "data/gamescripts/ai/ai_blackforest.txt",
    "data/gamescripts/ai/ai_brownietown.txt", "data/gamescripts/ai/ai_cococliffs.txt",
    "data/gamescripts/ai/ai_deepfried.txt", "data/gamescripts/ai/ai_frostbite.txt",
    "data/gamescripts/ai/ai_greatgorge.txt", "data/gamescripts/ai/ai_rockyroad.txt",
    "data/gamescripts/ai/ai_sugarcove.txt",
};
static bool bots_siege(std::string& t)
{
    bool changed = false;
    size_t pos = 0;
    while (pos < t.size()) {
        size_t eol = t.find('\n', pos);
        if (eol == std::string::npos) eol = t.size();
        std::string line = t.substr(pos, eol - pos);
        const size_t q0 = line.find('"'), q1 = line.rfind('"');
        if (line.compare(0, 8, "Build_OD") == 0 && q0 != std::string::npos && q1 > q0) {
            std::vector<std::string> f;
            {
                const std::string in = line.substr(q0 + 1, q1 - q0 - 1);
                size_t i = 0;
                while (i < in.size()) {
                    while (i < in.size() && (in[i] == ' ' || in[i] == '\t')) i++;
                    size_t j = i;
                    while (j < in.size() && in[j] != ' ' && in[j] != '\t') j++;
                    if (j > i) f.push_back(in.substr(i, j - i));
                    i = j;
                }
            }
            const bool ladder = !f.empty() && f[0] == "ladderwall", spring = !f.empty() && f[0] == "springboard";
            if ((ladder || spring) && f.size() >= 10) {
                const double delay = atof(f[6].c_str()), weight = atof(f[9].c_str());
                const double want_w = ladder ? 375.0 : 360.0;
                if (weight > 0 && (delay > 180.0 || weight < want_w)) {
                    char b[32];
                    if (delay > 180.0) { snprintf(b, sizeof b, "%.1f", 180.0); f[6] = b; }
                    if (weight < want_w) { snprintf(b, sizeof b, "%.1f", want_w); f[9] = b; }
                    std::string out = line.substr(0, q0 + 1);
                    for (size_t k = 0; k < f.size(); k++) {
                        out += f[k];
                        if (k + 1 < f.size()) out += std::string(k == 0 ? 13 - (f[0].size() < 12 ? f[0].size() : 12) : 4, ' ');
                    }
                    out += line.substr(q1);   /* closing quote and anything after (\r) */
                    t.replace(pos, eol - pos, out);
                    eol = pos + out.size();
                    changed = true;
                }
            }
        }
        pos = eol + 1;
    }
    return changed;
}

static std::vector<Patch> active_patches()
{
    std::vector<Patch> p;
    if (env_on("FP_HIDE_COOP_START"))
        p.push_back({ "data/gamescripts/menucmds.txt", "hide-coop-start-1", hide_coop_start });
    if (env_on("FP_SKIP_HOWTO_PROMPT"))
        p.push_back({ "data/gamescripts/console/autoexec.txt", "skip-howto-prompt-1", skip_howto_prompt });
    if (env_on("FP_MENU_HINTS")) {
        p.push_back({ "data/gamescripts/menufinal/common.txt", "menu-hints-2", nav_object });
        p.push_back({ "data/menus/menumaster.txt", "menu-hints-2", nav_layout });
        p.push_back({ "data/local/english/symbols.txt", "menu-hints-2", nav_string });
        p.push_back({ "data/local/uk/symbols.txt", "menu-hints-2", nav_string });
    }
    if (env_on("FP_BOTS_SIEGE"))
        for (const char* f : kBotFiles) p.push_back({ f, "bots-siege-1", bots_siege });
    const std::string icons = fp_button_icons_id();
    if (!icons.empty())
        for (const char* const* e = fp_button_icons_entries(); *e; e++) {
            const std::string name = *e;
            p.push_back({ name, icons + "@" + name.substr(name.find_last_of('/') + 1),
                          [name](std::string& d) { return fp_button_icons_patch(name, d); } });
        }
    return p;
}

static bool truncate_file(FILE* f, uint64_t size)
{
    fflush(f);
#ifdef _WIN32
    return _chsize_s(_fileno(f), (long long)size) == 0;
#else
    return ftruncate(fileno(f), (off_t)size) == 0;
#endif
}

static bool copy_file(const char* from, const char* to)
{
    FILE* a = fopen(from, "rb");
    if (!a) return false;
    FILE* b = fopen(to, "wb");
    if (!b) { fclose(a); return false; }
    std::vector<char> buf(4u << 20);
    size_t n; bool ok = true;
    while ((n = fread(buf.data(), 1, buf.size(), a)) > 0)
        if (fwrite(buf.data(), 1, n, b) != n) { ok = false; break; }
    fclose(a);
    if (fclose(b) != 0) ok = false;
    return ok;
}

static std::string s_patched;      /* the path handed out, once built */
static int s_state = 0;             /* 0 not tried, 1 built, -1 off/failed */

static void build(const char* orig)
{
    s_state = -1;
    const std::vector<Patch> patches = active_patches();
    if (patches.empty()) return;

    struct stat st;
    if (stat(orig, &st) != 0) return;
    std::string key = "orig " + std::to_string((long long)st.st_size) + " " + std::to_string((long long)st.st_mtime) + "\n";
    const std::string orig_line = key;
    for (const Patch& p : patches) key += p.id + "\n";

    FP_MKDIR("cache");
    const std::string out = "cache/siege_patched.qrk", keyf = "cache/siege_patched.key";
    { FILE* k = fopen(keyf.c_str(), "rb");
      if (k) {
          std::string have; char b[512]; size_t n;
          while ((n = fread(b, 1, sizeof b, k)) > 0) have.append(b, n);
          fclose(k);
          struct stat so;
          if (have == key && stat(out.c_str(), &so) == 0) {
              s_patched = out; s_state = 1;
              fprintf(stderr, "[patches] using %s (%zu patch(es))\n", out.c_str(), patches.size());
              return;
          }
      } }

    /* A copy of this same original from an earlier set of patches: reuse it. */
    bool reuse = false;
    { FILE* k = fopen(keyf.c_str(), "rb");
      if (k) {
          std::string have; char b[512]; size_t n;
          while ((n = fread(b, 1, sizeof b, k)) > 0) have.append(b, n);
          fclose(k);
          struct stat so;
          reuse = have.compare(0, orig_line.size(), orig_line) == 0 && stat(out.c_str(), &so) == 0 &&
                  (uint64_t)so.st_size >= (uint64_t)st.st_size;
      } }
    if (reuse) fprintf(stderr, "[patches] updating %s (%zu patch(es))...\n", out.c_str(), patches.size());
    else fprintf(stderr, "[patches] building %s from %s (%zu patch(es), one-time copy)...\n",
                 out.c_str(), orig, patches.size());
    remove(keyf.c_str());
    FILE* f = fopen(orig, "rb");
    if (!f) return;
    Archive a;
    if (!load_toc(f, a)) { fclose(f); fprintf(stderr, "[patches] %s: not a qrk archive\n", orig); return; }
    struct Out { size_t idx; std::string data; };
    std::vector<Out> changed;
    for (const Patch& p : patches) {
        size_t idx = a.e.size();
        for (size_t i = 0; i < a.e.size(); i++) if (a.e[i].name == p.entry) { idx = i; break; }
        if (idx == a.e.size()) { fprintf(stderr, "[patches] %s: %s not in the archive, skipped\n", p.id.c_str(), p.entry.c_str()); continue; }
        std::string text;
        for (const Out& o : changed) if (o.idx == idx) text = o.data;   /* stack patches on one entry */
        if (text.empty() && !entry_data(f, a.e[idx], text)) { fprintf(stderr, "[patches] %s: cannot read %s\n", p.id.c_str(), p.entry.c_str()); continue; }
        if (!p.apply(text)) { fprintf(stderr, "[patches] %s: %s did not match, skipped\n", p.id.c_str(), p.entry.c_str()); continue; }
        bool merged = false;
        for (Out& o : changed) if (o.idx == idx) { o.data = text; merged = true; }
        if (!merged) changed.push_back({ idx, text });
        fprintf(stderr, "[patches] %s applied to %s\n", p.id.c_str(), p.entry.c_str());
    }
    fclose(f);
    if (changed.empty()) return;

    FILE* w = nullptr;
    bool ok = true;
    if (reuse) {
        /* Original header and table of contents back, appended entries cut off. */
        std::vector<uint8_t> head(a.datastart);
        FILE* o = fopen(orig, "rb");
        w = fopen(out.c_str(), "r+b");
        ok = o && w && read_at(o, 0, head.data(), head.size());
        if (o) fclose(o);
        if (ok) {
#ifdef _WIN32
            _fseeki64(w, 0, SEEK_SET);
#else
            fseeko(w, 0, SEEK_SET);
#endif
            ok = fwrite(head.data(), 1, head.size(), w) == head.size() && truncate_file(w, (uint64_t)st.st_size);
        }
        if (!ok) { if (w) fclose(w); w = nullptr; fprintf(stderr, "[patches] reusing %s failed; copying again\n", out.c_str()); }
    }
    if (!w) {
        if (!copy_file(orig, out.c_str())) { fprintf(stderr, "[patches] cannot write %s\n", out.c_str()); remove(out.c_str()); return; }
        w = fopen(out.c_str(), "r+b");
        if (!w) return;
        ok = true;
    }
    uint64_t end = (uint64_t)st.st_size;
    for (const Out& o : changed) {
        end = (end + 15) & ~(uint64_t)15;
        if (end + o.data.size() > 0xFFFFFFFFull) { ok = false; break; }   /* offsets are 32-bit */
#ifdef _WIN32
        _fseeki64(w, (int64_t)end, SEEK_SET);
#else
        fseeko(w, (off_t)end, SEEK_SET);
#endif
        ok &= fwrite(o.data.data(), 1, o.data.size(), w) == o.data.size();
        uint8_t rec[21];
        const Entry& x = a.e[o.idx];
        put_le32(rec, (uint32_t)o.data.size()); put_le32(rec + 4, (uint32_t)end); put_le32(rec + 8, (uint32_t)o.data.size());
        put_le32(rec + 12, x.hash); put_le32(rec + 16, x.nameoff); rec[20] = 0;   /* stored, not compressed */
#ifdef _WIN32
        _fseeki64(w, (int64_t)x.toc_pos, SEEK_SET);
#else
        fseeko(w, (off_t)x.toc_pos, SEEK_SET);
#endif
        ok &= fwrite(rec, 1, 21, w) == 21;
        end += o.data.size();
    }
    if (fclose(w) != 0) ok = false;
    if (!ok) { remove(out.c_str()); fprintf(stderr, "[patches] writing %s failed; using the original\n", out.c_str()); return; }
    if (FILE* k = fopen(keyf.c_str(), "wb")) { fwrite(key.data(), 1, key.size(), k); fclose(k); }
    s_patched = out; s_state = 1;
    fprintf(stderr, "[patches] %s ready\n", out.c_str());
}

/* The fs layer's open hook: the patched copy for siege.qrk, else the path. */
static const char* redirect(const char* host_path, char* out, size_t n)
{
    const char* base = strrchr(host_path, '/');
    const char* b2 = strrchr(host_path, '\\');
    if (!base || (b2 && b2 > base)) base = b2;
    base = base ? base + 1 : host_path;
#ifdef _WIN32
    if (_stricmp(base, "siege.qrk") != 0) return host_path;
#else
    if (strcasecmp(base, "siege.qrk") != 0) return host_path;
#endif
    static bool once = false;
    if (!once) { once = true; build(host_path); }   /* the first open is on one thread, at boot */
    if (s_state != 1) return host_path;
    snprintf(out, n, "%s", s_patched.c_str());
    return out;
}

} // namespace

typedef const char* (*ps3_fs_redirect_fn)(const char*, char*, size_t);
extern "C" ps3_fs_redirect_fn g_ps3_fs_redirect;

/* Called from main before the guest starts. */
extern "C" void fp_data_patches_install(void)
{
    g_ps3_fs_redirect = redirect;
}

#ifdef FP_DATA_PATCHES_TEST
/* Self-test: inflate every compressed entry of an archive and compare sizes. */
extern "C" ps3_fs_redirect_fn g_ps3_fs_redirect = nullptr;
int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    FILE* f = fopen(argv[1], "rb");
    Archive a;
    if (!f || !load_toc(f, a)) { printf("cannot read\n"); return 1; }
    size_t bad = 0, n = 0;
    for (const Entry& x : a.e) {
        if (argc > 2 && x.name.find(argv[2]) == std::string::npos) continue;
        std::string d;
        if (!entry_data(f, x, d)) { bad++; printf("FAIL %s\n", x.name.c_str()); }
        n++;
    }
    printf("%zu entries checked, %zu failed\n", n, bad);
    if (argc > 3) { build(argv[1]); printf("state %d %s\n", s_state, s_patched.c_str()); }   /* patches from the environment */
    return bad ? 1 : 0;
}
#endif
