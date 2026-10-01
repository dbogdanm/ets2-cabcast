// CabCast - ReShade 6.8 addon for Euro Truck Simulator 2 (DX11)
//
// Shows a live Android phone (scrcpy window, captured with Windows.Graphics.Capture) on the truck's own
// navigation screen. The nav screen is a render target the game draws its UI into every frame; right after
// the game finishes that pass, the phone image is copied over it, so the cab's screen mesh, lighting,
// reflections and glare all apply to the phone picture like to the real nav.
//
// Setup: open the ReShade overlay -> Add-ons -> DBM CabCast -> "Pick screen" -> click the thumbnail that
// shows the truck's navigation. The pick is remembered (size + format + order). Toggle with the hotkey.
// Touch: in the overlay the phone picture is interactive (left click = tap/drag, right = back,
// middle = home, wheel = scroll) - clicks are forwarded to the scrcpy window.

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "capture.hpp"
#include "cdp.hpp"

#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <shlwapi.h>
#include <atomic>
#include <thread>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "DBM CabCast";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Your Android phone or a PC browser (YouTube, Spotify, Maps) live on the truck's navigation screen.";

namespace
{
const char *SEC = "DBM_Phone";

struct rt_info { resource_desc desc; bool srv_ok = false; };

std::recursive_mutex g_mutex; // recursive: D3D11 may destroy resources (-> on_destroy_resource) inside our own calls
std::map<uint64_t, rt_info> g_rts;            // all 2D colour render targets that could be a screen
// which candidate textures the game wrote recently (screens are not necessarily redrawn every frame, and may be
// drawn multisampled and then resolved/copied into the texture the cab actually samples)
struct seen_info { uint64_t first = 0, last = 0; uint32_t order = 0; uint32_t kind = 0; }; // kind: 1 RT, 2 resolve dst, 4 copy dst
std::map<uint64_t, seen_info> g_seen;
uint64_t g_frame_no = 1; uint32_t g_order_counter = 0;
bool g_show_all = false;
std::map<uint64_t, resource_desc> g_all_rt;   // every render target / UAV texture the game created (diagnostics)
bool g_dump_request = false, g_dumping = false;
std::vector<std::string> g_dump;
void note(uint64_t res, uint32_t kind)
{
    seen_info &i = g_seen[res];
    if (!i.first) { i.first = g_frame_no; i.order = ++g_order_counter; }
    i.last = g_frame_no; i.kind |= kind;
}
bool recent(uint64_t res, uint64_t frames)
{
    auto it = g_seen.find(res);
    return it != g_seen.end() && g_frame_no - it->second.last <= frames;
}
device *g_device = nullptr;
HWND g_game_hwnd = nullptr;

// selection
uint64_t g_target = 0;
uint32_t g_key_w = 0, g_key_h = 0, g_key_fmt = 0, g_key_idx = 0; // persisted
// distinct screen textures in the order the game draws them (this / last frame): the draw order of the cab
// screens is stable, while handles and creation order change whenever the game recreates them
std::vector<uint64_t> g_frame_binds, g_last_binds;
uint32_t same_desc_rank(const std::vector<uint64_t> &v, uint64_t res, const resource_desc &d, bool *found = nullptr);
bool g_picking = false;
std::map<uint64_t, resource_view> g_thumbs;   // SRVs for thumbnails

// phone texture (same desc as the target, CPU-filled)
resource g_phone = {}; resource_view g_phone_srv = {};
resource_desc g_phone_desc;
uint64_t g_uploaded_serial = 0;
bool g_injected_this_frame = false;

// settings
bool g_enabled = true;
bool g_only_main = true;    // phone only where the game draws the map into the 3D cab (center screen); cab-screen
                           // render targets (cluster) keep sampling the real game map with the route
float g_volume = 0.8f;     // phone / Android Auto audio volume (Windows audio session of the helper process)
command_list *g_imm = nullptr;
int g_hotkey = VK_F9;
int g_mode = 0;                 // 0 = mirror phone screen, 1 = virtual display sized like the nav screen
int g_dpi = 220;
int g_vd_w = 1600, g_vd_h = 800;            // separate (virtual) phone display, landscape like a car screen
char g_app[128] = "com.google.android.apps.maps";
bool g_flip_v = false, g_flip_h = false; // some cab screens are stored upside down / mirrored
bool g_screen_off = true;       // phone display off while it is shown in the truck (keeps running, saves battery)
bool g_fit = true;
int g_rate = 30;                 // screen updates per second (capture, conversion, upload)
int g_source = 0;                // 0 = Android phone (scrcpy), 1 = PC browser (Chrome / Edge app window)
char g_url[512] = "https://www.youtube.com";
const int CDP_PORT = 9333;
HANDLE g_job = nullptr;   // kill-on-close job holding every helper we start
std::mutex g_job_mutex;
std::atomic<int> g_auto_top { 0 }; // browser: title bar height in the captured window (cropped away)
// phone connection state from 'adb devices' (setup guide)
std::atomic<int> g_adb_state { 0 };  // 0 unknown, 1 no device, 2 unauthorized, 3 ok, 4 offline, 5 adb missing
std::string g_adb_model; std::atomic<bool> g_adb_busy { false }; ULONGLONG g_adb_last = 0;
// in-game cursor: the camera is frozen, a test pattern on the cab screen gives the screen -> phone mapping
bool g_cursor = false;
int g_cursor_key = VK_F10;
const int PROBE_HOLD = 22;           // frames per pattern (lets TAA / DLSS settle)
int g_probe = -1;                    // calibration frame counter, -1 = idle
// calibration patterns: 0 dark, 1 all markers, 2..6 the markers whose id has bit (p-2) set, 7 everything lit.
// Every marker is identified by its on/off sequence, so markers hidden by the bezel or the steering wheel
// simply drop out; the full-lit pattern shows which part of the texture is visible on the cab screen.
const int NP = 8, NBITS = 5;
int g_probe_want = -1, g_probe_shown = -1; // pattern to show / uploaded
std::vector<uint8_t> g_probe_buf[NP];
std::vector<uint8_t> g_cshot[NP];
float g_vis[4] = { 0, 0, 1, 1 };     // visible part of the screen texture (normalised display coords)
int g_res = 0;                       // texture resolution: 0 auto (from the screen size), else 1x / 2x / 4x of the game's
std::vector<uint8_t> g_shot[2]; uint32_t g_shot_w = 0, g_shot_h = 0; const uint32_t SHOT_SCALE = 2;
const int GRID = 5, NM = GRID * GRID; // 5x5 grid of markers
float g_marker[NM][2];               // marker centres in display texels, row major
float g_found[NM][2]; int g_found_id[NM]; bool g_cal_dbg = false; uint64_t g_cal_frame = 0; float g_cal_err = 0;
bool g_cal_ok = false; double g_hom[9] = {};
// screen auto-detection: each candidate texture shows a test pattern in turn; the one that lights up a compact area
// where the player looks is the screen. A green corner in the pattern gives the orientation (flip V / H).
bool g_detect_on = false; int g_detect_t = 0, g_detect_i = 0, g_detect_phase = 0; uint64_t g_detect_uploaded = 0, g_detect_prev = 0;
std::vector<uint64_t> g_detect_cands;
struct det_res { double score = 0; int area = 0, other = 0, green = 0; bool fv = false, fh = false; };
std::vector<det_res> g_detect_res;
std::vector<uint64_t> g_detect_rank; std::vector<det_res> g_detect_rank_res; int g_detect_pos = 0; // hits, best first
const int DETECT_HOLD = 10;
std::string g_cal_msg; uint64_t g_cal_msg_frame = 0;
resource g_staging = {};              // keep aspect (letterbox) vs stretch
bool g_hide_window = true;
float g_brightness = 1.0f;
char g_scrcpy[MAX_PATH] = "";

window_capture g_cap;
capture_frame g_frame;
std::vector<uint8_t> g_pixels;  // converted target pixels, all mips (owned by the converter thread)
// converter thread: capture frame -> target pixel format off the render thread
std::thread g_conv;
std::atomic<bool> g_conv_quit { false }, g_conv_dirty { true };
std::mutex g_ready_mutex;
std::vector<uint8_t> g_ready;   // last converted image, ready for upload
uint64_t g_ready_serial = 0;
resource_desc g_conv_desc;      // desc of the phone texture (guarded by g_ready_mutex)
bool g_conv_desc_ok = false;
std::string g_msg;

const wchar_t *TITLE = L"DBM Phone|Desktop Head Unit";
int g_crop[4] = { 0, 0, 0, 0 }; // left, top, right, bottom (source pixels)

// ---------------------------------------------------------------------------------------------- helpers
// view format: typed resources (e.g. the cab screens, R8G8B8A8_UNORM_SRGB) must be viewed with their own format
format view_format(format f) { return format_to_typeless(f) == f ? format_to_default_typed(f, 0) : f; }

bool is_screen_tex(const resource_desc &d)
{
    if (d.type != resource_type::texture_2d) return false;
    if (d.texture.depth_or_layers != 1) return false;
    if (d.texture.width < 128 || d.texture.height < 64 || d.texture.width > 4096 || d.texture.height > 4096) return false;
    switch (format_to_typeless(d.texture.format))
    {
    case format::r8g8b8a8_typeless: case format::b8g8r8a8_typeless: case format::b8g8r8x8_typeless:
    case format::r10g10b10a2_typeless: case format::r16g16b16a16_typeless: case format::r11g11b10_float:
        return true;
    default: return false;
    }
}

uint32_t bpp(format f)
{
    return format_to_typeless(f) == format::r16g16b16a16_typeless ? 8 : 4;
}

float srgb_to_lin(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

uint16_t f2h(float f)
{
    uint32_t x; memcpy(&x, &f, 4);
    const uint32_t s = (x >> 16) & 0x8000; int e = int((x >> 23) & 0xff) - 127 + 15; uint32_t m = x & 0x7fffff;
    if (e <= 0) return uint16_t(s);
    if (e >= 31) return uint16_t(s | 0x7c00);
    return uint16_t(s | (e << 10) | (m >> 13));
}

uint32_t f2r11(float v, int mbits) // unsigned small float (11 or 10 bit)
{
    if (v <= 0) return 0;
    uint32_t x; memcpy(&x, &v, 4);
    int e = int((x >> 23) & 0xff) - 127 + 15; uint32_t m = x & 0x7fffff;
    if (e <= 0) return 0;
    if (e >= 31) e = 30, m = 0x7fffff;
    return (uint32_t(e) << mbits) | (m >> (23 - mbits));
}

// float sRGB rgb (mip 0) -> packed texture data with a box-filtered mip chain
void pack(const std::vector<float> &rgb, const resource_desc &d, std::vector<uint8_t> &out)
{
    const uint32_t W = d.texture.width, H = d.texture.height, levels = std::max<uint32_t>(1, d.texture.levels);
    const format tl = format_to_typeless(d.texture.format);
    const bool linear_target = tl == format::r16g16b16a16_typeless || tl == format::r11g11b10_float;
    const uint32_t B = bpp(d.texture.format);
    size_t total = 0;
    for (uint32_t l = 0; l < levels; ++l) total += size_t(std::max(1u, W >> l)) * std::max(1u, H >> l) * B;
    out.assign(total, 0);
    size_t off = 0; uint32_t w = W, h = H;
    static thread_local std::vector<float> cur, nxt;
    cur = rgb;
    for (uint32_t l = 0; l < levels; ++l)
    {
        for (uint32_t i = 0; i < w * h; ++i)
        {
            float r = cur[i * 3], g = cur[i * 3 + 1], bl = cur[i * 3 + 2];
            uint8_t *p = &out[off + size_t(i) * B];
            if (linear_target) { r = srgb_to_lin(r); g = srgb_to_lin(g); bl = srgb_to_lin(bl); }
            if (tl == format::r8g8b8a8_typeless) { p[0] = uint8_t(r * 255 + .5f); p[1] = uint8_t(g * 255 + .5f); p[2] = uint8_t(bl * 255 + .5f); p[3] = 255; }
            else if (tl == format::b8g8r8a8_typeless || tl == format::b8g8r8x8_typeless) { p[2] = uint8_t(r * 255 + .5f); p[1] = uint8_t(g * 255 + .5f); p[0] = uint8_t(bl * 255 + .5f); p[3] = 255; }
            else if (tl == format::r10g10b10a2_typeless)
            {
                const uint32_t v = uint32_t(r * 1023 + .5f) | (uint32_t(g * 1023 + .5f) << 10) | (uint32_t(bl * 1023 + .5f) << 20) | (3u << 30);
                memcpy(p, &v, 4);
            }
            else if (tl == format::r16g16b16a16_typeless)
            {
                const uint16_t v[4] = { f2h(r), f2h(g), f2h(bl), f2h(1.0f) }; memcpy(p, v, 8);
            }
            else if (tl == format::r11g11b10_float)
            {
                const uint32_t v = f2r11(r, 6) | (f2r11(g, 6) << 11) | (f2r11(bl, 5) << 22); memcpy(p, &v, 4);
            }
        }
        off += size_t(w) * h * B;
        if (l + 1 < levels) // box-filter next mip
        {
            const uint32_t nw = std::max(1u, w >> 1), nh = std::max(1u, h >> 1);
            nxt.assign(size_t(nw) * nh * 3, 0);
            for (uint32_t y = 0; y < nh; ++y) for (uint32_t x = 0; x < nw; ++x) for (int k = 0; k < 3; ++k)
            {
                const uint32_t xa = std::min(2 * x, w - 1), xb = std::min(2 * x + 1, w - 1), ya = std::min(2 * y, h - 1), yb = std::min(2 * y + 1, h - 1);
                nxt[(size_t(y) * nw + x) * 3 + k] = (cur[(size_t(ya) * w + xa) * 3 + k] + cur[(size_t(ya) * w + xb) * 3 + k] +
                                                    cur[(size_t(yb) * w + xa) * 3 + k] + cur[(size_t(yb) * w + xb) * 3 + k]) * 0.25f;
            }
            cur.swap(nxt); w = nw; h = nh;
        }
    }
}

// phone frame (BGRA8, sRGB-encoded) -> target pixels (mip 0..n), letterboxed or stretched, bilinear
void set_flip(bool v, bool on)
{
    bool &f = v ? g_flip_v : g_flip_h;
    if (f == on) return;
    f = on;
    const int a = v ? 1 : 0; const float lo = g_vis[a], hi = g_vis[a + 2];
    g_vis[a] = 1 - hi; g_vis[a + 2] = 1 - lo; // the visible area is in display coords
    char b[96]; snprintf(b, sizeof(b), "%.3f %.3f %.3f %.3f", g_vis[0], g_vis[1], g_vis[2], g_vis[3]);
    reshade::set_config_value(nullptr, SEC, "Visible", static_cast<const char *>(b));
    reshade::set_config_value(nullptr, SEC, v ? "FlipV" : "FlipH", int(on));
    g_conv_dirty = true;
}

// where the source picture lands in the screen texture (display texels): inside the visible area, letterboxed
struct placement { float ox, oy, sx, sy; uint32_t x0, y0, x1, y1; };
placement place(uint32_t W, uint32_t H, uint32_t sw, uint32_t sh)
{
    const float rx = g_vis[0] * W, ry = g_vis[1] * H, rw = std::max(8.0f, (g_vis[2] - g_vis[0]) * W), rh = std::max(8.0f, (g_vis[3] - g_vis[1]) * H);
    placement p { rx, ry, sw / rw, sh / rh };
    float dw = rw, dh = rh;
    if (g_fit) { const float s2 = std::max(p.sx, p.sy); p.sx = p.sy = s2; dw = sw / s2; dh = sh / s2; p.ox = rx + (rw - dw) / 2; p.oy = ry + (rh - dh) / 2; }
    p.x0 = uint32_t(std::clamp(p.ox, 0.0f, float(W))); p.y0 = uint32_t(std::clamp(p.oy, 0.0f, float(H)));
    p.x1 = uint32_t(std::clamp(p.ox + dw, 0.0f, float(W))); p.y1 = uint32_t(std::clamp(p.oy + dh, 0.0f, float(H)));
    return p;
}

// 8-bit targets (the usual cab screens): integer bilinear straight into the texture bytes, integer box mips.
// Several times cheaper than the float path.
bool convert8(const capture_frame &src, const resource_desc &d, std::vector<uint8_t> &out)
{
    const format tl = format_to_typeless(d.texture.format);
    const bool rgba = tl == format::r8g8b8a8_typeless, bgra = tl == format::b8g8r8a8_typeless || tl == format::b8g8r8x8_typeless;
    if (!rgba && !bgra) return false;
    const uint32_t W = d.texture.width, H = d.texture.height, levels = std::max<uint32_t>(1, d.texture.levels);
    size_t total = 0;
    for (uint32_t l = 0; l < levels; ++l) total += size_t(std::max(1u, W >> l)) * std::max(1u, H >> l) * 4;
    out.resize(total);
    uint8_t lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = uint8_t(std::min(255.0f, i * g_brightness + 0.5f));
    uint32_t *dst = reinterpret_cast<uint32_t *>(out.data());
    std::fill(dst, dst + size_t(W) * H, 0xff000000u);
    const placement pl = place(W, H, src.w, src.h);
    const float sx = pl.sx, sy = pl.sy, ox = pl.ox, oy = pl.oy;
    const uint32_t x0 = pl.x0, y0 = pl.y0, x1 = pl.x1, y1 = pl.y1;
    static thread_local std::vector<uint32_t> xi; static thread_local std::vector<uint32_t> xw;
    xi.resize(W); xw.resize(W);
    for (uint32_t x = x0; x < x1; ++x)
    {
        const float fx = std::clamp((x - ox + 0.5f) * sx - 0.5f, 0.0f, float(src.w - 1));
        xi[x] = uint32_t(fx); xw[x] = uint32_t((fx - xi[x]) * 256);
    }
    const int ri = rgba ? 0 : 2, bi = rgba ? 2 : 0; // where R / B go in the target pixel
    for (uint32_t y = y0; y < y1; ++y)
    {
        const float fy = std::clamp((y - oy + 0.5f) * sy - 0.5f, 0.0f, float(src.h - 1));
        const uint32_t iy = uint32_t(fy), iy1 = std::min(iy + 1, src.h - 1), ty = uint32_t((fy - iy) * 256);
        const uint8_t *r0 = &src.bgra[size_t(iy) * src.w * 4], *r1 = &src.bgra[size_t(iy1) * src.w * 4];
        uint8_t *o = reinterpret_cast<uint8_t *>(&dst[size_t(g_flip_v ? H - 1 - y : y) * W]);
        for (uint32_t x = x0; x < x1; ++x)
        {
            const uint32_t ix = xi[x], ix1 = std::min(ix + 1, src.w - 1), tx = xw[x];
            const uint8_t *a = r0 + ix * 4, *b = r0 + ix1 * 4, *c = r1 + ix * 4, *e = r1 + ix1 * 4;
            uint8_t *p = o + size_t(g_flip_h ? W - 1 - x : x) * 4;
            for (int k = 0; k < 3; ++k) // source k: 0 = B, 1 = G, 2 = R
            {
                const uint32_t top = a[k] * (256 - tx) + b[k] * tx, bot = c[k] * (256 - tx) + e[k] * tx;
                p[k == 0 ? bi : k == 2 ? ri : 1] = lut[(top * (256 - ty) + bot * ty) >> 16];
            }
        }
    }
    // mips: 2x2 average of the encoded values (same as the float path)
    size_t off = 0; uint32_t w = W, h = H;
    for (uint32_t l = 1; l < levels; ++l)
    {
        const uint32_t nw = std::max(1u, w >> 1), nh = std::max(1u, h >> 1);
        const uint8_t *s = &out[off]; uint8_t *t = &out[off + size_t(w) * h * 4];
        for (uint32_t y = 0; y < nh; ++y)
        {
            const uint8_t *ra = s + size_t(std::min(2 * y, h - 1)) * w * 4, *rb = s + size_t(std::min(2 * y + 1, h - 1)) * w * 4;
            for (uint32_t x = 0; x < nw; ++x)
            {
                const uint32_t xa = std::min(2 * x, w - 1) * 4, xb = std::min(2 * x + 1, w - 1) * 4;
                uint8_t *q = t + (size_t(y) * nw + x) * 4;
                for (int k = 0; k < 4; ++k) q[k] = uint8_t((ra[xa + k] + ra[xb + k] + rb[xa + k] + rb[xb + k] + 2) >> 2);
            }
        }
        off += size_t(w) * h * 4; w = nw; h = nh;
    }
    return true;
}

void convert(const capture_frame &src, const resource_desc &d, std::vector<uint8_t> &out)
{
    if (convert8(src, d, out)) return;
    const uint32_t W = d.texture.width, H = d.texture.height;
    static thread_local std::vector<float> rgb; // reused: no 6 MB alloc/free per frame
    rgb.assign(size_t(W) * H * 3, 0.0f);
    const placement pl = place(W, H, src.w, src.h);
    const float sx = pl.sx, sy = pl.sy, ox = pl.ox, oy = pl.oy;
    const uint32_t x0 = pl.x0, y0 = pl.y0, x1 = pl.x1, y1 = pl.y1;
    for (uint32_t y = y0; y < y1; ++y)
    {
        const float fy = std::clamp((y - oy + 0.5f) * sy - 0.5f, 0.0f, float(src.h - 1));
        const uint32_t iy = uint32_t(fy), iy1 = std::min(iy + 1, src.h - 1); const float ty = fy - iy;
        for (uint32_t x = x0; x < x1; ++x)
        {
            const float fx = std::clamp((x - ox + 0.5f) * sx - 0.5f, 0.0f, float(src.w - 1));
            const uint32_t ix = uint32_t(fx), ix1 = std::min(ix + 1, src.w - 1); const float tx = fx - ix;
            const uint8_t *a = &src.bgra[(size_t(iy) * src.w + ix) * 4], *b = &src.bgra[(size_t(iy) * src.w + ix1) * 4];
            const uint8_t *c = &src.bgra[(size_t(iy1) * src.w + ix) * 4], *e = &src.bgra[(size_t(iy1) * src.w + ix1) * 4];
            float *o = &rgb[(size_t(g_flip_v ? H - 1 - y : y) * W + (g_flip_h ? W - 1 - x : x)) * 3];
            for (int k = 0; k < 3; ++k) // BGRA -> RGB
            {
                const int ch = 2 - k;
                const float top = a[ch] + (b[ch] - a[ch]) * tx, bot = c[ch] + (e[ch] - c[ch]) * tx;
                o[k] = std::min(1.0f, (top + (bot - top) * ty) / 255.0f * g_brightness);
            }
        }
    }
    pack(rgb, d, out);
}

capture_frame g_cropped;
const capture_frame &cropped(const capture_frame &f)
{
    const int l = g_crop[0], t = g_crop[1] + g_auto_top, r = g_crop[2], b = g_crop[3];
    if (!(l | t | r | b) || l + r >= int(f.w) - 16 || t + b >= int(f.h) - 16) return f;
    g_cropped.w = f.w - l - r; g_cropped.h = f.h - t - b; g_cropped.serial = f.serial;
    g_cropped.bgra.resize(size_t(g_cropped.w) * g_cropped.h * 4);
    for (uint32_t y = 0; y < g_cropped.h; ++y)
        memcpy(&g_cropped.bgra[size_t(y) * g_cropped.w * 4], &f.bgra[(size_t(y + t) * f.w + l) * 4], size_t(g_cropped.w) * 4);
    return g_cropped;
}

void restore_all();
bool g_in_upload = false, g_destroy_pending = false;
void destroy_phone_tex()
{
    if (g_in_upload) { g_destroy_pending = true; return; } // finished after the upload (see inject)
    restore_all();
    if (g_device && g_phone_srv.handle) g_device->destroy_resource_view(g_phone_srv);
    if (g_device && g_phone.handle) g_device->destroy_resource(g_phone);
    g_phone = {}; g_phone_srv = {}; g_uploaded_serial = 0; g_detect_uploaded = 0;
    std::lock_guard<std::mutex> rl(g_ready_mutex); g_conv_desc_ok = false; g_ready.clear(); g_ready_serial = 0;
}

void drop_thumbs()
{
    for (auto &t : g_thumbs) if (g_device && t.second.handle) g_device->destroy_resource_view(t.second);
    g_thumbs.clear();
}

void set_target(uint64_t res)
{
    if (res != g_target) { g_vis[0] = g_vis[1] = 0; g_vis[2] = g_vis[3] = 1; reshade::set_config_value(nullptr, SEC, "Visible", static_cast<const char *>("0 0 1 1")); g_cal_ok = false; }
    g_target = res;
    destroy_phone_tex();
    if (!res) return;
    const resource_desc &d = g_rts[res].desc;
    g_key_w = d.texture.width; g_key_h = d.texture.height; g_key_fmt = uint32_t(d.texture.format);
    // rank among same-desc textures in draw order
    g_key_idx = same_desc_rank(g_last_binds, res, d);
    reshade::set_config_value(nullptr, SEC, "TargetW", g_key_w);
    reshade::set_config_value(nullptr, SEC, "TargetH", g_key_h);
    reshade::set_config_value(nullptr, SEC, "TargetFormat", g_key_fmt);
    reshade::set_config_value(nullptr, SEC, "TargetIndex", g_key_idx);
}

uint32_t same_desc_rank(const std::vector<uint64_t> &v, uint64_t res, const resource_desc &d, bool *found)
{
    uint32_t n = 0;
    for (uint64_t r : v)
    {
        if (r == res) { if (found) *found = true; return n; }
        auto it = g_rts.find(r);
        if (it != g_rts.end() && it->second.desc.texture.width == d.texture.width && it->second.desc.texture.height == d.texture.height &&
            it->second.desc.texture.format == d.texture.format) n++;
    }
    if (found) *found = false;
    return n;
}

void auto_select()
{
    if (!g_key_w || g_frame_no % 30) return;
    if (g_target && recent(g_target, 120)) return; // sticky while the game keeps drawing it
    std::vector<uint64_t> c;
    for (uint64_t r : g_last_binds)
    {
        auto it = g_rts.find(r);
        if (it == g_rts.end()) continue;
        const resource_desc &d = it->second.desc;
        if (d.texture.width == g_key_w && d.texture.height == g_key_h && uint32_t(d.texture.format) == g_key_fmt) c.push_back(r);
    }
    if (g_key_idx < c.size() && c[g_key_idx] != g_target) { g_target = c[g_key_idx]; destroy_phone_tex(); }
}

// copy the phone picture into the nav render target (on the render thread)
void inject(command_list *cmd)
{
    if (!g_target || (!g_detect_on && (!g_enabled || !g_cap.window_found()))) return;
    device *dev = cmd->get_device();
    const uint64_t target = g_target;
    const resource_desc td = g_rts[target].desc; // copy: the map entry can go away below
    if (!g_phone.handle)
    {
        resource_desc d = td;
        if (g_only_main && !g_detect_on) // the 3D cab samples this texture directly: it can be sharper than the game's
        {
            const uint32_t m = std::max(td.texture.width, td.texture.height);
            uint32_t sc = g_res ? uint32_t(g_res) : 1;
            if (!g_res) while (sc < 4 && m * sc * 2 <= 1024) sc *= 2; // auto: about 1024 texels across
            while (sc > 1 && m * sc > 2048) sc /= 2;
            d.texture.width *= sc; d.texture.height *= sc;
            if (td.texture.levels > 1) { uint32_t lv = 1; while ((m * sc) >> lv) lv++; d.texture.levels = uint16_t(lv); }
        }
        d.usage = resource_usage::copy_source | resource_usage::shader_resource;
        d.flags = resource_flags::none; d.heap = memory_heap::default_;
        if (!dev->create_resource(d, nullptr, resource_usage::copy_source, &g_phone)) { g_msg = "could not create phone texture"; return; }
        g_phone_desc = d;
        dev->create_resource_view(g_phone, resource_usage::shader_resource,
                                  resource_view_desc(view_format(d.texture.format), 0, d.texture.levels, 0, 1), &g_phone_srv);
    }
    { std::lock_guard<std::mutex> rl(g_ready_mutex); if (!g_conv_desc_ok) { g_conv_desc = g_phone_desc; g_conv_desc_ok = true; g_conv_dirty = true; } }
    auto upload_buf = [&](const std::vector<uint8_t> &buf) {
        const resource phone = g_phone; const resource_desc pd = g_phone_desc;
        const uint32_t B = bpp(pd.texture.format), levels = std::max<uint32_t>(1, pd.texture.levels);
        size_t off = 0;
        g_in_upload = true;
        for (uint32_t l = 0; l < levels && off < buf.size(); ++l)
        {
            const uint32_t w = std::max(1u, pd.texture.width >> l), h = std::max(1u, pd.texture.height >> l);
            if (off + size_t(w) * h * B > buf.size()) break;
            subresource_data sd; sd.data = const_cast<uint8_t *>(&buf[off]); sd.row_pitch = w * B; sd.slice_pitch = w * h * B;
            dev->update_texture_region(sd, phone, l);
            off += size_t(w) * h * B;
        }
        g_in_upload = false;
    };
    if (g_detect_on)
    {
        if (g_detect_uploaded != target)
        {
            // magenta, with a green block in the texture's first rows/columns (shows where texel (0,0) ends up)
            static std::vector<uint8_t> buf;
            const uint32_t W = g_phone_desc.texture.width, H = g_phone_desc.texture.height;
            std::vector<float> rgb(size_t(W) * H * 3);
            for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x)
            {
                float *o = &rgb[(size_t(y) * W + x) * 3];
                const bool corner = x < W / 3 && y < H / 3;
                o[0] = corner ? 0.f : 1.f; o[1] = corner ? 1.f : 0.f; o[2] = corner ? 0.f : 1.f;
            }
            pack(rgb, g_phone_desc, buf);
            upload_buf(buf);
            if (g_destroy_pending) { g_destroy_pending = false; destroy_phone_tex(); return; }
            g_detect_uploaded = target;
        }
    }
    else if (g_probe_want >= 0)
    {
        if (g_probe_shown != g_probe_want && !g_probe_buf[g_probe_want].empty())
        {
            upload_buf(g_probe_buf[g_probe_want]);
            if (g_destroy_pending) { g_destroy_pending = false; destroy_phone_tex(); return; }
            g_probe_shown = g_probe_want;
        }
    }
    else if (g_ready_serial != g_uploaded_serial)
    {
        // copy out under the lock, upload without it: UpdateSubresource can make D3D11 destroy released game
        // resources right here (-> on_destroy_resource -> destroy_phone_tex), which must not deadlock or free g_phone
        static std::vector<uint8_t> upload; uint64_t serial;
        { std::lock_guard<std::mutex> rl(g_ready_mutex); if (g_ready.empty()) return; upload.swap(g_ready); serial = g_ready_serial; }
        const resource phone = g_phone; const resource_desc pd = g_phone_desc;
        const uint32_t B = bpp(pd.texture.format), levels = std::max<uint32_t>(1, pd.texture.levels);
        size_t off = 0;
        g_in_upload = true;
        for (uint32_t l = 0; l < levels && off < upload.size(); ++l)
        {
            const uint32_t w = std::max(1u, pd.texture.width >> l), h = std::max(1u, pd.texture.height >> l);
            if (off + size_t(w) * h * B > upload.size()) break;
            subresource_data sd; sd.data = &upload[off]; sd.row_pitch = w * B; sd.slice_pitch = w * h * B;
            dev->update_texture_region(sd, phone, l);
            off += size_t(w) * h * B;
        }
        g_in_upload = false;
        if (g_destroy_pending) { g_destroy_pending = false; destroy_phone_tex(); return; }
        g_uploaded_serial = serial;
    }
    if ((!g_uploaded_serial && g_probe_shown < 0 && !g_detect_on) || g_target != target || !g_phone.handle) return; // target/phone texture destroyed meanwhile
    if (g_only_main && !g_detect_on) { g_injected_this_frame = true; return; } // shown by swapping the map's view in 3D draws instead
    const uint32_t levels = std::max<uint32_t>(1, td.texture.levels);
    cmd->barrier(resource { g_target }, resource_usage::render_target, resource_usage::copy_dest);
    for (uint32_t l = 0; l < levels; ++l) cmd->copy_texture_region(g_phone, l, nullptr, resource { g_target }, l, nullptr);
    cmd->barrier(resource { g_target }, resource_usage::copy_dest, resource_usage::render_target);
    g_injected_this_frame = true;
}

// ---------------------------------------------------------------------------------------------- events
// the capture thread lives with the device (never started/joined under the loader lock in DllMain)
void converter()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // never compete with the game's threads
    capture_frame f; uint64_t last = 0;
    auto next = std::chrono::steady_clock::now();
    while (!g_conv_quit)
    {
        resource_desc d; bool ok;
        { std::lock_guard<std::mutex> rl(g_ready_mutex); d = g_conv_desc; ok = g_conv_desc_ok; }
        const bool fresh = g_cap.latest(f);
        if (!ok || !f.w || (!fresh && !g_conv_dirty && f.serial == last)) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
        if (!g_conv_dirty && std::chrono::steady_clock::now() < next) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
        next = std::chrono::steady_clock::now() + std::chrono::microseconds(1000000 / std::max(5, g_rate) - 2000);
        g_conv_dirty = false; last = f.serial;
        { std::lock_guard<std::recursive_mutex> l(g_mutex); g_frame.w = f.w; g_frame.h = f.h; g_frame.client_x = f.client_x; g_frame.client_y = f.client_y; g_frame.serial = f.serial; }
        try { convert(cropped(f), d, g_pixels); }
        catch (...) { g_pixels.clear(); std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; } // never take the game down
        std::lock_guard<std::mutex> rl(g_ready_mutex);
        if (g_conv_desc_ok && g_conv_desc.texture.width == d.texture.width && g_conv_desc.texture.height == d.texture.height &&
            g_conv_desc.texture.format == d.texture.format) { g_ready.swap(g_pixels); g_ready_serial++; }
    }
}

// phone sound (scrcpy forwards it, Android Auto head unit plays it): volume of their Windows audio sessions
std::thread g_audio; std::atomic<bool> g_audio_quit { false };
void audio_thread()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!g_audio_quit)
    {
        IMMDeviceEnumerator *en = nullptr; IMMDevice *dev = nullptr; IAudioSessionManager2 *mgr = nullptr; IAudioSessionEnumerator *se = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&en)) &&
            SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
            SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void **)&mgr)) &&
            SUCCEEDED(mgr->GetSessionEnumerator(&se)))
        {
            int n = 0; se->GetCount(&n);
            for (int i = 0; i < n; ++i)
            {
                IAudioSessionControl *c = nullptr; IAudioSessionControl2 *c2 = nullptr; ISimpleAudioVolume *v = nullptr;
                if (FAILED(se->GetSession(i, &c))) continue;
                DWORD pid = 0;
                if (SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), (void **)&c2))) { c2->GetProcessId(&pid); c2->Release(); }
                char exe[MAX_PATH] = ""; DWORD len = MAX_PATH;
                if (HANDLE h = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr) { QueryFullProcessImageNameA(h, 0, exe, &len); CloseHandle(h); }
                BOOL ours = FALSE;
                if (pid) { std::lock_guard<std::mutex> jl(g_job_mutex); if (g_job) if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) { IsProcessInJob(h, g_job, &ours); CloseHandle(h); } }
                if ((ours || StrStrIA(exe, "\\scrcpy.exe") || StrStrIA(exe, "\\desktop-head-unit.exe")) && SUCCEEDED(c->QueryInterface(__uuidof(ISimpleAudioVolume), (void **)&v)))
                { float cur = -1; v->GetMasterVolume(&cur); if (std::fabs(cur - g_volume) > 0.005f) v->SetMasterVolume(g_volume, nullptr); v->Release(); }
                c->Release();
            }
        }
        if (se) se->Release(); if (mgr) mgr->Release(); if (dev) dev->Release(); if (en) en->Release();
        for (int i = 0; i < 10 && !g_audio_quit; ++i) Sleep(100);
    }
    CoUninitialize();
}

void on_init_device(device *dev)
{
    // the game renders with D3D11; other add-ons (DLSS5) create extra D3D12 devices - ignore those
    if (dev->get_api() != device_api::d3d11 || g_device) return;
    g_device = dev; g_cap.start(TITLE);
    if (!g_conv.joinable()) { g_conv_quit = false; g_conv = std::thread(converter); }
    if (!g_audio.joinable()) { g_audio_quit = false; g_audio = std::thread(audio_thread); }
}
void stop_helpers();
void on_destroy_device(device *dev)
{
    if (dev != g_device) return;
    g_conv_quit = true; if (g_conv.joinable()) g_conv.join();
    g_audio_quit = true; if (g_audio.joinable()) g_audio.join();
    g_imm = nullptr;
    g_cap.stop(); drop_thumbs(); destroy_phone_tex();
    if (g_staging.handle) dev->destroy_resource(g_staging), g_staging = {};
    g_device = nullptr;
    stop_helpers();
}

void on_init_resource(device *dev, const resource_desc &d, const subresource_data *, resource_usage, resource res)
{
    if (dev == g_device && (d.usage & (resource_usage::render_target | resource_usage::unordered_access)) != 0 && d.type != resource_type::buffer)
    { std::lock_guard<std::recursive_mutex> l(g_mutex); g_all_rt[res.handle] = d; }
    if (dev != g_device || (d.usage & resource_usage::render_target) == 0 || !is_screen_tex(d)) return;
    std::lock_guard<std::recursive_mutex> l(g_mutex);
    g_rts[res.handle] = rt_info { d, (d.usage & resource_usage::shader_resource) != 0 };
}

void on_destroy_resource(device *dev, resource res)
{
    if (dev != g_device) return;
    std::lock_guard<std::recursive_mutex> l(g_mutex);
    g_all_rt.erase(res.handle);
    if (g_rts.erase(res.handle) == 0) return;
    auto t = g_thumbs.find(res.handle);
    if (t != g_thumbs.end()) { dev->destroy_resource_view(t->second); g_thumbs.erase(t); }
    g_seen.erase(res.handle);
    if (res.handle == g_target) { g_target = 0; destroy_phone_tex(); }
}

// shader resource slots that currently hold a view of the nav map (target), so they can be pointed at the phone
// texture while the game draws the 3D cab and back while it composes the cab screens (cluster)
struct slot_rec { shader_stage stages; pipeline_layout layout; uint32_t param, binding; resource_view orig; };
struct __declspec(uuid("a51e6a0b-5b3c-4f0e-8d2a-7c4e1f9b2d63")) cmd_state
{
    uint64_t rt0 = 0; bool pending = false;
    std::vector<slot_rec> recs; bool swapped = false;
};
thread_local bool t_pushing = false;

bool want_swap(uint64_t rt0)
{
    if (!g_only_main || !g_enabled || !g_phone_srv.handle || !g_uploaded_serial || !g_cap.window_found() || !rt0) return false;
    auto it = g_rts.find(rt0);
    // the cab screens are composed into 8-bit sRGB targets: keep the real map there
    return it == g_rts.end() || it->second.desc.texture.format != format::r8g8b8a8_unorm_srgb;
}

void apply_swap(command_list *cmd, cmd_state &s, bool swap)
{
    t_pushing = true;
    for (const slot_rec &r : s.recs)
    {
        const resource_view v = swap ? g_phone_srv : r.orig;
        cmd->push_descriptors(r.stages, r.layout, r.param, descriptor_table_update { {}, r.binding, 0, 1, descriptor_type::shader_resource_view, &v });
    }
    t_pushing = false;
    s.swapped = swap;
}

void restore_all()
{
    if (!g_imm) return;
    if (cmd_state *s = g_imm->get_private_data<cmd_state>()) { if (s->swapped) apply_swap(g_imm, *s, false); s->recs.clear(); }
}

void on_push_descriptors(command_list *cmd, shader_stage stages, pipeline_layout layout, uint32_t param, const descriptor_table_update &u)
{
    if (t_pushing || u.type != descriptor_type::shader_resource_view || cmd->get_device() != g_device) return;
    std::lock_guard<std::recursive_mutex> l(g_mutex);
    if (!g_only_main || !g_target) return;
    cmd_state *ps = cmd->get_private_data<cmd_state>();
    if (!ps) return;
    cmd_state &s = *ps;
    if (s.recs.empty() && !g_target) return;
    const resource_view *views = static_cast<const resource_view *>(u.descriptors);
    bool hit = false;
    for (uint32_t i = 0; i < u.count; ++i)
    {
        const uint32_t b = u.binding + u.array_offset + i;
        s.recs.erase(std::remove_if(s.recs.begin(), s.recs.end(), [&](const slot_rec &r) { return r.stages == stages && r.param == param && r.binding == b; }), s.recs.end());
        if (views[i].handle && g_device->get_resource_from_view(views[i]).handle == g_target)
        { s.recs.push_back({ stages, layout, param, b, views[i] }); hit = true; }
    }
    if (hit && want_swap(s.rt0)) apply_swap(cmd, s, true);
    else if (hit) s.swapped = false;
}
void on_init_cmd(command_list *cmd) { cmd->create_private_data<cmd_state>(); }
void on_destroy_cmd(command_list *cmd) { cmd->destroy_private_data<cmd_state>(); }

void on_bind_rts(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view)
{
    if (cmd->get_device() != g_device) return;
    cmd_state *ps = cmd->get_private_data<cmd_state>();
    if (!ps) return;
    cmd_state &s = *ps;
    uint64_t rt0 = 0;
    if (count && rtvs[0].handle) rt0 = cmd->get_device()->get_resource_from_view(rtvs[0]).handle;
    std::lock_guard<std::recursive_mutex> l(g_mutex);
    if (g_dumping)
    {
        std::string line = "BIND";
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint64_t r = rtvs[i].handle ? g_device->get_resource_from_view(rtvs[i]).handle : 0;
            const resource_desc d = r ? g_device->get_resource_desc(resource { r }) : resource_desc {};
            char b[96]; snprintf(b, sizeof(b), " [%llx %ux%ux%u f%u s%u m%u]", (unsigned long long)r, d.texture.width, d.texture.height,
                                 d.texture.depth_or_layers, uint32_t(d.texture.format), d.texture.samples, d.texture.levels);
            line += b;
        }
        g_dump.push_back(line);
    }
    if ((s.rt0 == g_target && g_target && rt0 != g_target) || s.pending) { s.pending = false; inject(cmd); } // game just finished the screen
    if (!s.recs.empty()) { const bool w = want_swap(rt0); if (w != s.swapped) apply_swap(cmd, s, w); }
    if (rt0 && rt0 != s.rt0 && g_rts.count(rt0))
    {
        note(rt0, 1);
        if (std::find(g_frame_binds.begin(), g_frame_binds.end(), rt0) == g_frame_binds.end()) g_frame_binds.push_back(rt0);
    }
    s.rt0 = rt0;
}

// a screen drawn with MSAA (or into a scratch texture) reaches the sampled texture by resolve/copy
void on_write_dst(command_list *cmd, resource dst, uint32_t kind)
{
    if (cmd->get_device() != g_device || !dst.handle) return;
    std::lock_guard<std::recursive_mutex> l(g_mutex);
    auto it = g_rts.find(dst.handle);
    if (it == g_rts.end())
    {
        const resource_desc d = g_device->get_resource_desc(dst);
        if (!is_screen_tex(d) || d.texture.samples > 1) return;
        it = g_rts.emplace(dst.handle, rt_info { d, (d.usage & resource_usage::shader_resource) != 0 }).first;
    }
    note(dst.handle, kind);
    if (g_dumping) { char b[96]; snprintf(b, sizeof(b), "%s -> [%llx]", kind == 2 ? "RESOLVE" : "COPY", (unsigned long long)dst.handle); g_dump.push_back(b); }
    if (dst.handle == g_target) if (cmd_state *ps = cmd->get_private_data<cmd_state>()) ps->pending = true; // inject after it
}
bool on_resolve(command_list *cmd, resource, uint32_t, const subresource_box *, resource dst, uint32_t, uint32_t, uint32_t, uint32_t, format)
{ on_write_dst(cmd, dst, 2); return false; }
bool on_copy_region(command_list *cmd, resource, uint32_t, const subresource_box *, resource dst, uint32_t, const subresource_box *, filter_mode)
{ on_write_dst(cmd, dst, 4); return false; }
bool on_copy_res(command_list *cmd, resource, resource dst) { on_write_dst(cmd, dst, 4); return false; }

void probe_step(command_queue *queue, swapchain *sc);
void detect_step(command_queue *queue, swapchain *sc);
void on_present(command_queue *queue, swapchain *sc, const rect *, const rect *, uint32_t, const rect *)
{
    std::lock_guard<std::recursive_mutex> l(g_mutex);
    if (queue->get_device() != g_device) return;
    if (!g_game_hwnd) { g_game_hwnd = static_cast<HWND>(sc->get_hwnd()); g_cap.game_hwnd = g_game_hwnd; }
    if (g_detect_on) { try { detect_step(queue, sc); } catch (...) { g_detect_on = false; g_target = 0; destroy_phone_tex(); g_msg = "screen detection failed"; } }
    if (g_probe >= 0) { try { probe_step(queue, sc); } catch (...) { g_probe = -1; g_probe_want = -1; g_probe_shown = -1; g_conv_dirty = true; } }
    g_imm = queue->get_immediate_command_list();
    if (g_source == 1)
    {
        float dpr; int iw, ih; cdp::metrics(dpr, iw, ih);
        if (dpr > 0 && ih > 0 && g_frame.h)
        {
            const int top = std::clamp(int(g_frame.h) - int(std::lround(ih * dpr)), 0, 200);
            if (top != g_auto_top) { g_auto_top = top; g_conv_dirty = true; }
        }
    }
    else if (g_auto_top) { g_auto_top = 0; g_conv_dirty = true; }
    if (g_target && !g_injected_this_frame) inject(queue->get_immediate_command_list()); // screen not redrawn this frame
    g_injected_this_frame = false;
    g_frame_no++;
    g_last_binds.swap(g_frame_binds); g_frame_binds.clear();
    if (!g_detect_on) auto_select();
    if (g_dumping)
    {
        g_dumping = false;
        char path[MAX_PATH]; GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string fn(path); fn = fn.substr(0, fn.find_last_of("\\/") + 1) + "dbm_phone_dump.txt";
        if (FILE *f = fopen(fn.c_str(), "w"))
        {
            fprintf(f, "ALL RENDER TARGET / UAV TEXTURES (handle type w h layers format samples mips usage, last seen frames ago)\n");
            for (auto &[r, d] : g_all_rt)
            {
                auto si = g_seen.find(r);
                fprintf(f, "%llx t%u %ux%u L%u f%u s%u m%u u%x seen %lld\n", (unsigned long long)r, uint32_t(d.type), d.texture.width, d.texture.height,
                        d.texture.depth_or_layers, uint32_t(d.texture.format), d.texture.samples, d.texture.levels, uint32_t(d.usage),
                        si == g_seen.end() ? -1LL : (long long)(g_frame_no - si->second.last));
            }
            fprintf(f, "\nONE FRAME\n");
            for (const std::string &l2 : g_dump) fprintf(f, "%s\n", l2.c_str());
            fclose(f);
            g_msg = "dump written: " + fn;
        }
        g_dump.clear();
    }
    if (g_dump_request) { g_dump_request = false; g_dumping = true; }
}

float half_to_float(uint16_t h)
{
    const uint32_t e = (h >> 10) & 31, m = h & 1023, sgn = (h >> 15) & 1;
    float v = e == 0 ? m / 1024.0f / 16384.0f : e == 31 ? 65504.0f : std::ldexp(1.0f + m / 1024.0f, int(e) - 15);
    return sgn ? -v : v;
}

void build_probe()
{
    const resource_desc &d = g_phone_desc;
    const uint32_t W = d.texture.width, H = d.texture.height;
    const float lo = 0.08f, hi = 0.92f, step = (hi - lo) / (GRID - 1);
    const int rx = std::max<int>(3, int(step * W * 0.2f)), ry = std::max<int>(3, int(step * H * 0.2f));
    for (int m = 0; m < NM; ++m) { g_marker[m][0] = (lo + step * (m % GRID)) * W; g_marker[m][1] = (lo + step * (m / GRID)) * H; }
    std::vector<float> rgb;
    for (int p = 0; p < NP; ++p)
    {
        rgb.assign(size_t(W) * H * 3, p == NP - 1 ? 1.0f : 0.0f);
        if (p == NP - 1) for (size_t i = 0; i < rgb.size(); i += 3) rgb[i + 1] = 0; // magenta everywhere
        if (p >= 1 && p < NP - 1)
            for (int m = 0; m < NM; ++m)
            {
                if (p >= 2 && !(((m + 1) >> (p - 2)) & 1)) continue;
                const int cx = int(g_marker[m][0]), cy = int(g_marker[m][1]);
                for (int y = cy - ry; y <= cy + ry; ++y) for (int x = cx - rx; x <= cx + rx; ++x)
                {
                    if (x < 0 || y < 0 || x >= int(W) || y >= int(H)) continue;
                    const uint32_t tx = g_flip_h ? W - 1 - x : x, ty = g_flip_v ? H - 1 - y : y; // display space -> texture
                    float *o = &rgb[(size_t(ty) * W + tx) * 3]; o[0] = 1; o[1] = 0; o[2] = 1;
                }
            }
        pack(rgb, d, g_probe_buf[p]);
    }
}

bool grab_backbuffer(command_queue *queue, swapchain *sc, std::vector<uint8_t> &out)
{
    device *dev = queue->get_device();
    const resource bb = sc->get_current_back_buffer();
    const resource_desc bd = dev->get_resource_desc(bb);
    const resource_desc sd0 = g_staging.handle ? dev->get_resource_desc(g_staging) : resource_desc {};
    if (!g_staging.handle || sd0.texture.width != bd.texture.width || sd0.texture.height != bd.texture.height || sd0.texture.format != bd.texture.format)
    {
        if (g_staging.handle) dev->destroy_resource(g_staging), g_staging = {};
        const resource_desc sd(bd.texture.width, bd.texture.height, 1, 1, format_to_default_typed(bd.texture.format), 1, memory_heap::gpu_to_cpu, resource_usage::copy_dest);
        if (!dev->create_resource(sd, nullptr, resource_usage::copy_dest, &g_staging)) return false;
    }
    command_list *cmd = queue->get_immediate_command_list();
    cmd->barrier(bb, resource_usage::present, resource_usage::copy_source);
    cmd->copy_resource(bb, g_staging);
    cmd->barrier(bb, resource_usage::copy_source, resource_usage::present);
    queue->flush_immediate_command_list();
    subresource_data m;
    if (!dev->map_texture_region(g_staging, 0, nullptr, map_access::read_only, &m)) return false;
    const format tl = format_to_typeless(bd.texture.format);
    g_shot_w = bd.texture.width / SHOT_SCALE; g_shot_h = bd.texture.height / SHOT_SCALE;
    out.assign(size_t(g_shot_w) * g_shot_h * 3, 0);
    bool ok = true;
    for (uint32_t y = 0; y < g_shot_h && ok; ++y)
    {
        const uint8_t *row = static_cast<const uint8_t *>(m.data) + size_t(y * SHOT_SCALE) * m.row_pitch;
        for (uint32_t x = 0; x < g_shot_w; ++x)
        {
            const uint32_t sx = x * SHOT_SCALE; uint8_t *o = &out[(size_t(y) * g_shot_w + x) * 3];
            if (tl == format::r8g8b8a8_typeless) { const uint8_t *p = row + sx * 4; o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; }
            else if (tl == format::b8g8r8a8_typeless || tl == format::b8g8r8x8_typeless) { const uint8_t *p = row + sx * 4; o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; }
            else if (tl == format::r10g10b10a2_typeless)
            { uint32_t v; memcpy(&v, row + sx * 4, 4); o[0] = uint8_t((v & 1023) >> 2); o[1] = uint8_t(((v >> 10) & 1023) >> 2); o[2] = uint8_t(((v >> 20) & 1023) >> 2); }
            else if (tl == format::r16g16b16a16_typeless)
            {
                uint16_t v[4]; memcpy(v, row + sx * 8, 8);
                for (int k = 0; k < 3; ++k) o[k] = uint8_t(std::clamp(std::pow(std::max(0.0f, half_to_float(v[k])), 1 / 2.2f), 0.0f, 1.0f) * 255);
            }
            else ok = false;
        }
    }
    dev->unmap_texture_region(g_staging, 0);
    return ok;
}

bool solve_homography(const float src[][2], const float dst[][2], int n, double h[9])
{
    // least squares (normal equations) of the 8 unknowns
    double N[8][9] = {};
    for (int i = 0; i < n; ++i)
    {
        const double x = src[i][0], y = src[i][1], u = dst[i][0], v = dst[i][1];
        const double r0[9] = { x, y, 1, 0, 0, 0, -u * x, -u * y, u }, r1[9] = { 0, 0, 0, x, y, 1, -v * x, -v * y, v };
        for (const double *r : { r0, r1 })
            for (int j = 0; j < 8; ++j) for (int k = 0; k < 9; ++k) N[j][k] += r[j] * r[k];
    }
    for (int c = 0; c < 8; ++c)
    {
        int piv = c; for (int r = c + 1; r < 8; ++r) if (std::fabs(N[r][c]) > std::fabs(N[piv][c])) piv = r;
        if (std::fabs(N[piv][c]) < 1e-18) return false;
        for (int k = 0; k < 9; ++k) std::swap(N[c][k], N[piv][k]);
        for (int r = 0; r < 8; ++r) if (r != c) { const double f = N[r][c] / N[c][c]; for (int k = c; k < 9; ++k) N[r][k] -= f * N[c][k]; }
    }
    for (int i = 0; i < 8; ++i) h[i] = N[i][8] / N[i][i];
    h[8] = 1;
    return true;
}

void apply_h(const double *H, double x, double y, float &u, float &v)
{
    const double den = H[6] * x + H[7] * y + H[8];
    u = float((H[0] * x + H[1] * y + H[2]) / den); v = float((H[3] * x + H[4] * y + H[5]) / den);
}

bool invert3(const double m[9], double o[9])
{
    const double d = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::fabs(d) < 1e-18) return false;
    o[0] = (m[4] * m[8] - m[5] * m[7]) / d; o[1] = (m[2] * m[7] - m[1] * m[8]) / d; o[2] = (m[1] * m[5] - m[2] * m[4]) / d;
    o[3] = (m[5] * m[6] - m[3] * m[8]) / d; o[4] = (m[0] * m[8] - m[2] * m[6]) / d; o[5] = (m[2] * m[3] - m[0] * m[5]) / d;
    o[6] = (m[3] * m[7] - m[4] * m[6]) / d; o[7] = (m[1] * m[6] - m[0] * m[7]) / d; o[8] = (m[0] * m[4] - m[1] * m[3]) / d;
    return true;
}

// debug: last calibration screenshot with the found markers, next to the addon (dbm_phone\cal_debug.bmp / .txt)
std::string addon_dir();
void save_cal_debug(const std::vector<uint8_t> &img, uint32_t w, uint32_t h, int found, const char *result)
{
    const std::string dir = addon_dir();
    CreateDirectoryA(dir.c_str(), nullptr);
    std::vector<uint8_t> px(img);
    auto dot = [&](float fx, float fy, uint8_t r, uint8_t g, uint8_t b) {
        const int cx = int(fx / SHOT_SCALE), cy = int(fy / SHOT_SCALE);
        for (int y = cy - 3; y <= cy + 3; ++y) for (int x = cx - 3; x <= cx + 3; ++x)
            if (x >= 0 && y >= 0 && x < int(w) && y < int(h) && (x == cx || y == cy)) { uint8_t *o = &px[(size_t(y) * w + x) * 3]; o[0] = r; o[1] = g; o[2] = b; }
    };
    for (int i = 0; i < found && i < NM; ++i) dot(g_found[i][0], g_found[i][1], 0, 255, 0);
    if (FILE *f = fopen((dir + "\\cal_debug.bmp").c_str(), "wb"))
    {
        const uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
        uint8_t hd[54] = { 'B', 'M' };
        memcpy(hd + 2, &size, 4); hd[10] = 54; hd[14] = 40; memcpy(hd + 18, &w, 4); memcpy(hd + 22, &h, 4); hd[26] = 1; hd[28] = 24;
        fwrite(hd, 1, 54, f);
        std::vector<uint8_t> line(row, 0);
        for (int y = int(h) - 1; y >= 0; --y)
        {
            for (uint32_t x = 0; x < w; ++x) { const uint8_t *s0 = &px[(size_t(y) * w + x) * 3]; line[x * 3] = s0[2]; line[x * 3 + 1] = s0[1]; line[x * 3 + 2] = s0[0]; }
            fwrite(line.data(), 1, row, f);
        }
        fclose(f);
    }
    if (FILE *f = fopen((dir + "\\cal_debug.txt").c_str(), "w"))
    {
        RECT cr {}; if (g_game_hwnd) GetClientRect(g_game_hwnd, &cr);
        fprintf(f, "result: %s\nshot %ux%u (x%u)  client %ldx%ld  phone tex %ux%u  frame %ux%u  flip %d%d fit %d\n", result, w, h, SHOT_SCALE,
                cr.right, cr.bottom, g_phone_desc.texture.width, g_phone_desc.texture.height, g_frame.w, g_frame.h, int(g_flip_v), int(g_flip_h), int(g_fit));
        for (int i = 0; i < found && i < NM; ++i)
        {
            float u = 0, v = 0; if (g_cal_ok) apply_h(g_hom, g_found[i][0], g_found[i][1], u, v);
            fprintf(f, "m%d screen %.1f %.1f -> tex %.1f %.1f (want %.0f %.0f)\n", g_found_id[i], g_found[i][0], g_found[i][1], u, v, g_marker[g_found_id[i]][0], g_marker[g_found_id[i]][1]);
        }
        fprintf(f, "visible %.3f %.3f %.3f %.3f\n", g_vis[0], g_vis[1], g_vis[2], g_vis[3]);
        fprintf(f, "H %g %g %g / %g %g %g / %g %g %g\n", g_hom[0], g_hom[1], g_hom[2], g_hom[3], g_hom[4], g_hom[5], g_hom[6], g_hom[7], g_hom[8]);
        fclose(f);
    }
}

void cal_fail(const char *m) { g_cal_ok = false; g_cal_msg = m; g_cal_msg_frame = g_frame_no; }

// how much a pixel turned magenta between two shots
inline int magenta(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, size_t i)
{
    const int dr = b[i * 3] - a[i * 3], dg = b[i * 3 + 1] - a[i * 3 + 1], db = b[i * 3 + 2] - a[i * 3 + 2];
    const int m = std::min(dr, db);
    return m > 0 && dg < m * 3 / 4 ? m : 0; // bloom / tone mapping turn magenta pink: allow some green
}

void solve_probe()
{
    const std::vector<uint8_t> &dark = g_cshot[0], &all = g_cshot[1];
    const uint32_t w = g_shot_w, h = g_shot_h;
    for (int p = 0; p < NP; ++p) if (g_cshot[p].size() != dark.size() || dark.empty()) return cal_fail("calibration failed: no screenshot");
    std::vector<int> score(size_t(w) * h, 0); int best = 0;
    for (size_t i = 0; i < score.size(); ++i) { score[i] = magenta(dark, all, i); best = std::max(best, score[i]); }
    if (best < 20) return cal_fail("calibration failed: screen not visible (look at it)");
    const int T = best * 2 / 5;
    // blobs = candidate markers
    std::vector<int> label(score.size(), 0);
    struct comp { int area = 0; double sx = 0, sy = 0, sw = 0; std::vector<uint32_t> px; };
    std::vector<comp> comps; std::vector<uint32_t> stack;
    for (uint32_t i = 0; i < score.size(); ++i)
    {
        if (score[i] < T || label[i]) continue;
        comp c; stack.push_back(i); label[i] = int(comps.size()) + 1;
        while (!stack.empty())
        {
            const uint32_t j = stack.back(); stack.pop_back();
            const uint32_t x = j % w, y = j / w;
            c.area++; c.sx += double(x) * score[j]; c.sy += double(y) * score[j]; c.sw += score[j]; c.px.push_back(j);
            const uint32_t nb[4] = { x > 0 ? j - 1 : j, x + 1 < w ? j + 1 : j, y > 0 ? j - w : j, y + 1 < h ? j + w : j };
            for (uint32_t k : nb) if (k != j && !label[k] && score[k] >= T) { label[k] = label[i]; stack.push_back(k); }
        }
        if (c.area >= 2) comps.push_back(std::move(c));
        if (comps.size() > 200) break;
    }
    // identify each blob by its on/off sequence
    int found = 0; int area_of[NM] = {};
    for (const comp &c : comps)
    {
        double on = 0; for (uint32_t j : c.px) on += score[j];
        on /= c.area;
        int id = 0;
        for (int bit = 0; bit < NBITS; ++bit)
        {
            double v = 0; for (uint32_t j : c.px) v += magenta(dark, g_cshot[2 + bit], j);
            if (v / c.area > on * 0.5) id |= 1 << bit;
        }
        const int m = id - 1;
        if (m < 0 || m >= NM || c.area <= area_of[m]) continue;
        area_of[m] = c.area;
        const float fx = float((c.sx / c.sw + 0.5) * SHOT_SCALE), fy = float((c.sy / c.sw + 0.5) * SHOT_SCALE);
        int k = 0; while (k < found && g_found_id[k] != m) k++;
        if (k == found) found++;
        g_found[k][0] = fx; g_found[k][1] = fy; g_found_id[k] = m;
    }
    // homography from the identified markers: RANSAC (a misread marker can't pull the fit), then least squares
    const float tol = std::max(4.0f, g_phone_desc.texture.width * 0.015f);
    float src[NM][2], dst[NM][2];
    for (int i = 0; i < found; ++i) { src[i][0] = g_found[i][0]; src[i][1] = g_found[i][1]; dst[i][0] = g_marker[g_found_id[i]][0]; dst[i][1] = g_marker[g_found_id[i]][1]; }
    auto inliers = [&](const double *hm, std::vector<int> &in) {
        in.clear();
        for (int i = 0; i < found; ++i) { float u, v; apply_h(hm, src[i][0], src[i][1], u, v); if (std::hypot(u - dst[i][0], v - dst[i][1]) <= tol) in.push_back(i); }
    };
    std::vector<int> best_in, in;
    uint32_t rng = 12345;
    auto rnd = [&](int n) { rng = rng * 1664525u + 1013904223u; return int((rng >> 8) % uint32_t(n)); };
    for (int it = 0; it < 600 && found >= 4; ++it)
    {
        int k[4];
        for (int j = 0; j < 4; ++j) { bool dup; do { k[j] = rnd(found); dup = false; for (int q = 0; q < j; ++q) dup |= k[q] == k[j]; } while (dup); }
        float s4[4][2], d4[4][2];
        for (int j = 0; j < 4; ++j) { s4[j][0] = src[k[j]][0]; s4[j][1] = src[k[j]][1]; d4[j][0] = dst[k[j]][0]; d4[j][1] = dst[k[j]][1]; }
        double hm[9];
        if (!solve_homography(s4, d4, 4, hm)) continue;
        inliers(hm, in);
        if (in.size() > best_in.size()) best_in = in;
    }
    int n = 0; bool ok = false;
    for (int round = 0; round < 3 && best_in.size() >= 6; ++round) // refine: fit all agreeing markers, re-check
    {
        float si[NM][2], di[NM][2];
        for (size_t j = 0; j < best_in.size(); ++j) { si[j][0] = src[best_in[j]][0]; si[j][1] = src[best_in[j]][1]; di[j][0] = dst[best_in[j]][0]; di[j][1] = dst[best_in[j]][1]; }
        if (!solve_homography(si, di, int(best_in.size()), g_hom)) break;
        n = int(best_in.size()); ok = true;
        inliers(g_hom, in);
        if (in == best_in) break;
        best_in = in;
    }
    if (ok && n >= 6)
    {
        g_cal_err = 0;
        for (int i : best_in) { float u, v; apply_h(g_hom, src[i][0], src[i][1], u, v); g_cal_err = std::max(g_cal_err, std::hypot(u - dst[i][0], v - dst[i][1])); }
        // markers that disagree go to the end of the list (debug output)
        float fo[NM][2]; int io[NM]; int w2 = 0;
        for (int i : best_in) { fo[w2][0] = g_found[i][0]; fo[w2][1] = g_found[i][1]; io[w2++] = g_found_id[i]; }
        for (int i = 0; i < found; ++i) if (std::find(best_in.begin(), best_in.end(), i) == best_in.end()) { fo[w2][0] = g_found[i][0]; fo[w2][1] = g_found[i][1]; io[w2++] = g_found_id[i]; }
        memcpy(g_found, fo, sizeof(float) * 2 * found); memcpy(g_found_id, io, sizeof(int) * found);
    }
    else ok = false;
    char res[96]; snprintf(res, sizeof(res), "%s (%d of %d markers identified, %d used)", ok ? "ok" : "markers not found", found, NM, ok ? n : 0);
    if (!ok)
    {
        g_cal_ok = false; save_cal_debug(all, w, h, found, res);
        return cal_fail("calibration failed: screen not visible enough (look straight at it, camera still)");
    }
    // visible part of the texture: every lit pixel of the full pattern, through the homography
    {
        const std::vector<uint8_t> &full = g_cshot[NP - 1];
        int bf = 0; for (size_t i = 0; i < score.size(); ++i) bf = std::max(bf, magenta(dark, full, i));
        std::vector<float> us, vs;
        const float W = float(g_phone_desc.texture.width), H = float(g_phone_desc.texture.height);
        for (uint32_t y = 0; y < h; y += 2) for (uint32_t x = 0; x < w; x += 2)
        {
            if (magenta(dark, full, size_t(y) * w + x) < bf * 2 / 5) continue;
            float u, v; apply_h(g_hom, (x + 0.5) * SHOT_SCALE, (y + 0.5) * SHOT_SCALE, u, v);
            if (u < -0.05f * W || v < -0.05f * H || u > 1.05f * W || v > 1.05f * H) continue;
            us.push_back(u / W); vs.push_back(v / H);
        }
        if (us.size() > 50)
        {
            std::sort(us.begin(), us.end()); std::sort(vs.begin(), vs.end());
            const size_t lo = us.size() / 100, hi = us.size() - 1 - us.size() / 100;
            float vis[4] = { std::clamp(us[lo], 0.0f, 1.0f), std::clamp(vs[lo], 0.0f, 1.0f), std::clamp(us[hi], 0.0f, 1.0f), std::clamp(vs[hi], 0.0f, 1.0f) };
            // markers sit at 8 %..92 %: snap to the texture edge when the visible area reaches past them
            for (int k = 0; k < 2; ++k) { if (vis[k] < 0.06f) vis[k] = 0; if (vis[k + 2] > 0.94f) vis[k + 2] = 1; }
            if (vis[2] - vis[0] > 0.2f && vis[3] - vis[1] > 0.2f)
            {
                // flips: the pattern is built in display space, so these are display coords already
                memcpy(g_vis, vis, sizeof(vis)); g_conv_dirty = true;
                char b2[96]; snprintf(b2, sizeof(b2), "%.3f %.3f %.3f %.3f", g_vis[0], g_vis[1], g_vis[2], g_vis[3]);
                reshade::set_config_value(nullptr, SEC, "Visible", static_cast<const char *>(b2));
            }
        }
    }
    // orientation: display "down" must point down on the monitor, display "right" to the right
    {
        double inv[9];
        if (invert3(g_hom, inv))
        {
            const float W = float(g_phone_desc.texture.width), H = float(g_phone_desc.texture.height);
            float tx, ty, bx, by, lx, ly, rx, ry;
            apply_h(inv, W / 2, H * 0.3f, tx, ty); apply_h(inv, W / 2, H * 0.7f, bx, by);
            apply_h(inv, W * 0.3f, H / 2, lx, ly); apply_h(inv, W * 0.7f, H / 2, rx, ry);
            const bool fix_v = by < ty, fix_h = rx < lx;
            if (fix_v) { set_flip(true, !g_flip_v); for (int k = 0; k < 3; ++k) g_hom[3 + k] = g_hom[6 + k] * H - g_hom[3 + k]; }
            if (fix_h) { set_flip(false, !g_flip_h); for (int k = 0; k < 3; ++k) g_hom[k] = g_hom[6 + k] * W - g_hom[k]; }
            if (fix_v || fix_h) { g_msg = "screen orientation corrected"; for (auto &pb : g_probe_buf) pb.clear(); }
        }
    }
    save_cal_debug(all, w, h, found, res);
    g_cal_frame = g_frame_no;
    g_cal_ok = true; g_cal_msg.clear();
}

// called from present while calibrating
void probe_step(command_queue *queue, swapchain *sc)
{
    if (!g_phone.handle || !g_target || !g_enabled)
    {
        if (++g_probe > 200) { g_probe = -1; g_probe_want = -1; cal_fail("calibration failed: nothing on the screen (F9 on, source started?)"); }
        return;
    }
    if (g_probe_buf[0].empty()) { build_probe(); g_probe = 0; }
    const int p = g_probe / PROBE_HOLD;
    g_probe_want = std::min(p, NP - 1);
    auto finish = [] { g_probe = -1; g_probe_want = -1; g_probe_shown = -1; g_conv_dirty = true; for (auto &c : g_cshot) c.clear(); };
    if (g_probe % PROBE_HOLD == PROBE_HOLD - 1)
    {
        if (!grab_backbuffer(queue, sc, g_cshot[p])) { cal_fail("calibration failed: unsupported back buffer format"); return finish(); }
        if (p == NP - 1) { solve_probe(); return finish(); }
    }
    g_probe++;
}

// ---------------------------------------------------------------------------------------------- screen detection
void start_detect()
{
    if (g_detect_on || g_probe >= 0) return;
    std::vector<std::pair<uint64_t, uint64_t>> list;
    for (auto &[r, info] : g_rts)
    {
        const resource_desc &d = info.desc;
        auto si = g_seen.find(r);
        if (si == g_seen.end() || g_frame_no - si->second.last > 30) continue;              // not drawn right now
        if (d.texture.width >= 2560 || d.texture.width < 128 || d.texture.height < 64 || d.texture.samples > 1) continue;
        const bool likely = d.texture.format == format::r8g8b8a8_unorm_srgb;                // cab screens are drawn sRGB
        list.push_back({ (uint64_t(likely ? 0 : 1) << 32) | si->second.order, r });
    }
    std::sort(list.begin(), list.end());
    g_detect_cands.clear();
    for (auto &l : list) if (g_detect_cands.size() < 14) g_detect_cands.push_back(l.second);
    if (g_detect_cands.empty()) { g_msg = "no screens found - sit in the cab with the screens on"; return; }
    g_detect_prev = g_target; g_target = 0; destroy_phone_tex();
    g_detect_res.assign(g_detect_cands.size(), det_res {});
    g_detect_on = true; g_detect_i = 0; g_detect_phase = 0; g_detect_t = 0;
    g_cursor = false;
    g_msg = "detecting the screen... keep looking at it";
}

det_res score_detect(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, uint32_t w, uint32_t h)
{
    det_res r;
    if (a.size() != b.size() || a.empty()) return r;
    std::vector<uint8_t> lit(size_t(w) * h, 0);
    for (size_t i = 0; i < lit.size(); ++i)
    {
        const int dr = b[i * 3] - a[i * 3], dg = b[i * 3 + 1] - a[i * 3 + 1], db = b[i * 3 + 2] - a[i * 3 + 2];
        const int m = std::min(dr, db);
        if (m > 25 && dg < m * 3 / 4) lit[i] = 1;                               // turned magenta (or pink)
        else if (dg > 25 && std::max(dr, db) < dg / 2) lit[i] = 2;              // turned green
    }
    // largest connected magenta+green area = the screen; everything else lit counts against it
    std::vector<int> label(lit.size(), 0); std::vector<uint32_t> stack;
    int best = 0, best_label = 0, total = 0, n = 0;
    for (uint32_t i = 0; i < lit.size(); ++i)
    {
        if (!lit[i] || label[i]) continue;
        int area = 0; label[i] = ++n; stack.push_back(i);
        while (!stack.empty())
        {
            const uint32_t j = stack.back(); stack.pop_back(); area++;
            const uint32_t x = j % w, y = j / w;
            const uint32_t nb[4] = { x > 0 ? j - 1 : j, x + 1 < w ? j + 1 : j, y > 0 ? j - w : j, y + 1 < h ? j + w : j };
            for (uint32_t k : nb) if (k != j && lit[k] && !label[k]) { label[k] = n; stack.push_back(k); }
        }
        total += area;
        if (area > best) { best = area; best_label = n; }
    }
    if (best < 150 || best > int(w * h / 5)) return r; // tiny, or a full-screen buffer (post-processing)
    double mx = 0, my = 0, gx = 0, gy = 0; int mc = 0, gc = 0;
    for (uint32_t i = 0; i < lit.size(); ++i)
    {
        if (label[i] != best_label) continue;
        const double x = i % w, y = i / w;
        if (lit[i] == 1) { mx += x; my += y; mc++; } else { gx += x; gy += y; gc++; }
    }
    r.area = best; r.other = total - best; r.green = gc;
    if (mc) { mx /= mc; my /= mc; }
    if (gc > 20) { gx /= gc; gy /= gc; r.fv = gy > my; r.fh = gx > mx; } // texel (0,0) shows at the bottom / right
    const double cx = (mx * mc + gx * gc) / std::max(1, mc + gc) - w / 2.0, cy = (my * mc + gy * gc) / std::max(1, mc + gc) - h / 2.0;
    const double dist = std::sqrt(cx * cx + cy * cy) / std::sqrt(w * w / 4.0 + h * h / 4.0);
    r.score = (best - r.other) * (1.0 - 0.5 * dist);
    return r;
}

void finish_detect()
{
    g_detect_on = false; g_target = 0; destroy_phone_tex();
    for (size_t i = 0; i < g_detect_res.size(); ++i) // real cab screens have mip chains
        if (g_rts.count(g_detect_cands[i]) && g_rts[g_detect_cands[i]].desc.texture.levels <= 1) g_detect_res[i].score *= 0.5;
    {
        std::vector<int> order;
        for (size_t i = 0; i < g_detect_res.size(); ++i) if (g_detect_res[i].score > 0) order.push_back(int(i));
        std::sort(order.begin(), order.end(), [](int a, int b) { return g_detect_res[a].score > g_detect_res[b].score; });
        g_detect_rank.clear(); g_detect_rank_res.clear(); g_detect_pos = 0;
        for (int i : order) { g_detect_rank.push_back(g_detect_cands[i]); g_detect_rank_res.push_back(g_detect_res[i]); }
    }
    int bi = -1;
    for (size_t i = 0; i < g_detect_res.size(); ++i) if (g_detect_res[i].score > 0 && (bi < 0 || g_detect_res[i].score > g_detect_res[bi].score)) bi = int(i);
    if (FILE *f = fopen((addon_dir() + "\\detect_debug.txt").c_str(), "w"))
    {
        for (size_t i = 0; i < g_detect_res.size(); ++i)
        {
            const det_res &r = g_detect_res[i];
            const resource_desc d = g_rts.count(g_detect_cands[i]) ? g_rts[g_detect_cands[i]].desc : resource_desc {};
            fprintf(f, "%s#%zu %ux%u f%u mips %u: score %.0f area %d other %d green %d flipV %d flipH %d\n", int(i) == bi ? "* " : "  ", i,
                    d.texture.width, d.texture.height, uint32_t(d.texture.format), d.texture.levels, r.score, r.area, r.other, r.green, int(r.fv), int(r.fh));
        }
        fclose(f);
    }
    g_shot[0].clear(); g_shot[1].clear();
    if (bi < 0 || !g_rts.count(g_detect_cands[bi]))
    {
        if (g_detect_prev && g_rts.count(g_detect_prev)) g_target = g_detect_prev;
        g_msg = "no screen detected - look straight at the screen (camera still), then try again";
        return;
    }
    const det_res &r = g_detect_res[bi];
    set_target(g_detect_cands[bi]);
    if (r.green > 20) { set_flip(true, r.fv); set_flip(false, r.fh); } // else: keep (F10 calibration fixes it)
    g_conv_dirty = true; g_cal_ok = false; // the cursor recalibrates on its next use
    const resource_desc &d = g_rts[g_target].desc;
    char b[160]; snprintf(b, sizeof(b), "screen detected: %ux%u%s%s", d.texture.width, d.texture.height, g_flip_v ? ", flipped vertically" : "", g_flip_h ? ", mirrored" : "");
    g_msg = b;
}

// per candidate: DETECT_HOLD frames normal (reference shot), DETECT_HOLD frames with the pattern (test shot);
// comparing the two cancels whatever other screens still show
void detect_step(command_queue *queue, swapchain *sc)
{
    if (++g_detect_t < DETECT_HOLD) return;
    g_detect_t = 0;
    if (g_detect_phase == 0)
    {
        if (!grab_backbuffer(queue, sc, g_shot[0])) { g_detect_res.assign(g_detect_res.size(), det_res {}); return finish_detect(); }
        const uint64_t c = g_detect_cands[g_detect_i];
        if (g_rts.count(c)) { g_target = c; destroy_phone_tex(); }
        g_detect_phase = 1;
        return;
    }
    if (g_target && grab_backbuffer(queue, sc, g_shot[1])) g_detect_res[g_detect_i] = score_detect(g_shot[0], g_shot[1], g_shot_w, g_shot_h);
    g_target = 0; destroy_phone_tex();
    g_detect_phase = 0;
    char b[96]; snprintf(b, sizeof(b), "detecting the screen... %d/%zu (keep looking at it)", g_detect_i + 1, g_detect_cands.size());
    g_msg = b;
    if (++g_detect_i >= int(g_detect_cands.size())) finish_detect();
}

// display-space texel of the phone texture (as shown upright) -> phone window client pixel
bool display_to_client(float u, float v, int &px, int &py)
{
    const resource_desc &d = g_phone_desc;
    if (!d.texture.width || !g_frame.w) return false;
    int ct = g_crop[1] + g_auto_top;
    int cw = int(g_frame.w) - g_crop[0] - g_crop[2], chh = int(g_frame.h) - ct - g_crop[3];
    int cl = g_crop[0];
    if (cw <= 16 || chh <= 16) { cw = int(g_frame.w); chh = int(g_frame.h); ct = 0; cl = 0; } // same rule as cropped()
    const placement pl = place(d.texture.width, d.texture.height, cw, chh);
    const int fx = int((u - pl.ox) * pl.sx), fy = int((v - pl.oy) * pl.sy);
    px = fx + cl + g_frame.client_x; py = fy + ct + g_frame.client_y;
    return fx >= 0 && fy >= 0 && fx < cw && fy < chh;
}

void send_mouse(UINT msg, WPARAM wp, int x, int y);
HWND phone_hwnd();
float g_cur_x = 0, g_cur_y = 0; bool g_cur_on_phone = false, g_cur_down = false; uint64_t g_last_mid = 0;
ULONGLONG g_last_mid_ms = 0;

// browser page coordinates (CSS pixels) of a window client pixel
bool css_xy(int px, int py, float &x, float &y)
{
    float dpr; int iw, ih; cdp::metrics(dpr, iw, ih);
    if (dpr <= 0) dpr = 1;
    x = px / dpr; y = (py - g_auto_top) / dpr;
    return cdp::connected();
}
void cdp_mouse(const char *type, int px, int py, const char *button, int buttons, int clicks = 0)
{
    float x, y; if (!css_xy(px, py, x, y)) return;
    char b[256]; snprintf(b, sizeof(b), "{\"type\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"button\":\"%s\",\"buttons\":%d,\"clickCount\":%d}", type, x, y, button, buttons, clicks);
    cdp::send("Input.dispatchMouseEvent", b);
}
void ptr_down(int px, int py) { if (g_source == 1) { cdp_mouse("mouseMoved", px, py, "none", 0); cdp_mouse("mousePressed", px, py, "left", 1, 1); } else send_mouse(WM_LBUTTONDOWN, MK_LBUTTON, px, py); }
void ptr_drag(int px, int py) { if (g_source == 1) cdp_mouse("mouseMoved", px, py, "left", 1); else send_mouse(WM_MOUSEMOVE, MK_LBUTTON, px, py); }
void ptr_up(int px, int py) { if (g_source == 1) cdp_mouse("mouseReleased", px, py, "left", 0, 1); else send_mouse(WM_LBUTTONUP, 0, px, py); }
void ptr_hover(int px, int py) { if (g_source == 1) cdp_mouse("mouseMoved", px, py, "none", 0); } // menus / video controls on hover
void ptr_back(int px, int py)
{
    if (g_source == 1) cdp::send("Runtime.evaluate", "{\"expression\":\"history.back()\"}");
    else { send_mouse(WM_RBUTTONDOWN, MK_RBUTTON, px, py); send_mouse(WM_RBUTTONUP, 0, px, py); }
}
void ptr_wheel(int px, int py, float notches)
{
    if (g_source == 1)
    {
        float x, y; if (!css_xy(px, py, x, y)) return;
        char b[192]; snprintf(b, sizeof(b), "{\"type\":\"mouseWheel\",\"x\":%.1f,\"y\":%.1f,\"deltaX\":0,\"deltaY\":%.0f}", x, y, -notches * 120);
        cdp::send("Input.dispatchMouseEvent", b);
        return;
    }
    HWND h = phone_hwnd(); POINT p { px, py };
    if (h) { PostMessageW(h, WM_MOUSEMOVE, 0, MAKELPARAM(px, py)); ClientToScreen(h, &p); PostMessageW(h, WM_MOUSEWHEEL, MAKEWPARAM(0, short(notches * WHEEL_DELTA)), MAKELPARAM(p.x, p.y)); }
}

std::string utf8(const wchar_t *w, int n)
{
    char b[32]; const int m = WideCharToMultiByte(CP_UTF8, 0, w, n, b, sizeof(b), nullptr, nullptr);
    return std::string(b, std::max(0, m));
}

// a key for the browser page: named keys, Ctrl shortcuts as editing commands, everything else as typed text
void cdp_key(uint32_t vk, bool down)
{
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_CAPITAL || (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == VK_LWIN || vk == VK_RWIN) return;
    static const struct { uint32_t vk; const char *key; } named[] = {
        { VK_BACK, "Backspace" }, { VK_RETURN, "Enter" }, { VK_TAB, "Tab" }, { VK_LEFT, "ArrowLeft" }, { VK_RIGHT, "ArrowRight" },
        { VK_UP, "ArrowUp" }, { VK_DOWN, "ArrowDown" }, { VK_DELETE, "Delete" }, { VK_HOME, "Home" }, { VK_END, "End" },
        { VK_PRIOR, "PageUp" }, { VK_NEXT, "PageDown" } };
    const bool ctrl = GetAsyncKeyState(VK_CONTROL) < 0, alt = GetAsyncKeyState(VK_MENU) < 0, shift = GetAsyncKeyState(VK_SHIFT) < 0;
    const int mods = (alt ? 1 : 0) | (ctrl ? 2 : 0) | (shift ? 8 : 0);
    char b[512];
    for (auto &k : named)
    {
        if (k.vk != vk) continue;
        if (vk == VK_RETURN && down)
            snprintf(b, sizeof(b), "{\"type\":\"keyDown\",\"key\":\"Enter\",\"code\":\"Enter\",\"windowsVirtualKeyCode\":13,\"text\":\"\\r\",\"modifiers\":%d}", mods);
        else
            snprintf(b, sizeof(b), "{\"type\":\"%s\",\"key\":\"%s\",\"code\":\"%s\",\"windowsVirtualKeyCode\":%u,\"modifiers\":%d}", down ? "rawKeyDown" : "keyUp", k.key, k.key, vk, mods);
        cdp::send("Input.dispatchKeyEvent", b);
        return;
    }
    if (ctrl && !alt)
    {
        if (!down) return;
        const char *cmd = vk == 'A' ? "selectAll" : vk == 'C' ? "copy" : vk == 'X' ? "cut" : vk == 'Z' ? "undo" : nullptr;
        if (vk == 'V' && OpenClipboard(nullptr))
        {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
                if (const wchar_t *w = static_cast<const wchar_t *>(GlobalLock(h)))
                {
                    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
                    std::string t(std::max(0, n - 1), '\0'); WideCharToMultiByte(CP_UTF8, 0, w, -1, t.data(), n, nullptr, nullptr);
                    GlobalUnlock(h);
                    cdp::send("Input.insertText", "{\"text\":" + cdp::str(t) + "}");
                }
            CloseClipboard();
            return;
        }
        if (!cmd) return;
        snprintf(b, sizeof(b), "{\"type\":\"rawKeyDown\",\"windowsVirtualKeyCode\":%u,\"modifiers\":2,\"commands\":[\"%s\"]}", vk, cmd);
        cdp::send("Input.dispatchKeyEvent", b);
        return;
    }
    // printable: the character this key makes with the game's keyboard layout (AltGr / Shift / Caps Lock aware)
    BYTE ks[256] = {};
    if (shift) ks[VK_SHIFT] = 0x80;
    if (ctrl) ks[VK_CONTROL] = 0x80;
    if (alt) ks[VK_MENU] = 0x80;
    if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] = 1;
    wchar_t w[8];
    const HKL layout = GetKeyboardLayout(g_game_hwnd ? GetWindowThreadProcessId(g_game_hwnd, nullptr) : 0);
    const int n = ToUnicodeEx(vk, MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, layout), ks, w, 8, 4, layout);
    if (n <= 0 || w[0] < 0x20) return;
    const std::string t = cdp::str(utf8(w, n));
    if (down) snprintf(b, sizeof(b), "{\"type\":\"keyDown\",\"key\":%s,\"text\":%s,\"unmodifiedText\":%s,\"windowsVirtualKeyCode\":%u}", t.c_str(), t.c_str(), t.c_str(), vk);
    else snprintf(b, sizeof(b), "{\"type\":\"keyUp\",\"key\":%s,\"windowsVirtualKeyCode\":%u}", t.c_str(), vk);
    cdp::send("Input.dispatchKeyEvent", b);
}

void cursor_input(effect_runtime *rt)
{
    uint32_t mx = 0, my = 0; int16_t wheel = 0;
    rt->get_mouse_cursor_position(&mx, &my, &wheel);
    uint32_t bw = 0, bh = 0; rt->get_screenshot_width_and_height(&bw, &bh);
    RECT cr {}; if (g_game_hwnd) GetClientRect(g_game_hwnd, &cr);
    const float kx = cr.right > 0 ? float(bw) / cr.right : 1.0f, ky = cr.bottom > 0 ? float(bh) / cr.bottom : 1.0f;
    g_cur_x = mx * kx; g_cur_y = my * ky;
    g_cur_on_phone = false;
    if (!g_cal_ok || g_probe >= 0) return;
    const double *H = g_hom, den = H[6] * g_cur_x + H[7] * g_cur_y + 1;
    const float u = float((H[0] * g_cur_x + H[1] * g_cur_y + H[2]) / den), v = float((H[3] * g_cur_x + H[4] * g_cur_y + H[5]) / den);
    int px = 0, py = 0;
    g_cur_on_phone = display_to_client(u, v, px, py);
    static int last_px = -1, last_py = -1;
    const bool moved = px != last_px || py != last_py;
    last_px = px; last_py = py;
    if (g_cur_on_phone)
    {
        if (rt->is_mouse_button_pressed(0)) { ptr_down(px, py); g_cur_down = true; }
        else if (g_cur_down && rt->is_mouse_button_down(0)) { if (moved) ptr_drag(px, py); }
        else if (moved) ptr_hover(px, py);
        if (rt->is_mouse_button_pressed(1)) ptr_back(px, py);
        if (wheel != 0) ptr_wheel(px, py, wheel);
    }
    if (g_cur_down && !rt->is_mouse_button_down(0)) { ptr_up(px, py); g_cur_down = false; }
}

// typing goes to the phone (scrcpy turns key messages into Android key events / text)
void forward_keys(effect_runtime *rt)
{
    HWND h = phone_hwnd();
    if (!h && g_source == 0) return;
    for (uint32_t vk = 0x08; vk < 0xFF; ++vk)
    {
        if (vk == UINT(g_cursor_key) || vk == VK_ESCAPE || vk == UINT(g_hotkey)) continue;
        if (vk >= VK_LBUTTON && vk <= VK_XBUTTON2) continue;
        const bool down = rt->is_key_pressed(vk), up = rt->is_key_released(vk);
        if (!down && !up) continue;
        if (g_source == 1) { cdp_key(vk, down); continue; }
        UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        LPARAM lp = 1 | (LPARAM(sc) << 16);
        if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_INSERT || vk == VK_RCONTROL || vk == VK_RMENU)
            lp |= LPARAM(1) << 24; // extended key
        if (up) lp |= (LPARAM(1) << 30) | (LPARAM(1) << 31);
        const bool alt = vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
        PostMessageW(h, up ? (alt ? WM_SYSKEYUP : WM_KEYUP) : (alt ? WM_SYSKEYDOWN : WM_KEYDOWN), vk, lp);
    }
}

// recalibrate: the camera may have moved since the last calibration
void start_calibration()
{
    g_cal_ok = false; g_probe = 0; for (auto &pb : g_probe_buf) pb.clear(); g_cal_msg = "calibrating..."; g_cal_msg_frame = g_frame_no;
}

void set_cursor_mode(bool on)
{
    g_cursor = on; g_cur_down = false;
    if (on && !g_cal_ok) start_calibration();
    if (!on && g_probe >= 0) { g_probe = -1; g_probe_want = -1; g_probe_shown = -1; g_conv_dirty = true; }
}

void on_cursor_overlay(effect_runtime *)
{
    const bool show_msg = !g_cal_msg.empty() && g_frame_no - g_cal_msg_frame < 240;
    if (!g_cursor && !show_msg && !(g_cal_ok && (g_cal_dbg || g_frame_no - g_cal_frame < 180))) return;
    ImDrawList *dl = ImGui::GetForegroundDrawList(nullptr);
    const ImVec2 c(g_cur_x, g_cur_y);
    if (g_cursor)
    {
        const ImU32 ring = g_cur_on_phone ? IM_COL32(255, 255, 255, 235) : IM_COL32(255, 255, 255, 110);
        dl->AddCircleFilled(c, 11.0f, IM_COL32(0, 0, 0, 60), 32);
        dl->AddCircle(c, 10.0f, ring, 32, 2.0f);
        dl->AddCircleFilled(c, g_cur_down ? 6.0f : 2.5f, ring, 24);
    }
    if (g_cal_ok && g_probe < 0 && (g_cal_dbg || g_frame_no - g_cal_frame < 180))
    {
        // outline of the phone texture as the calibration sees it
        double inv[9];
        if (invert3(g_hom, inv))
        {
            const float W = float(g_phone_desc.texture.width), H = float(g_phone_desc.texture.height);
            const float cu[4] = { g_vis[0] * W, g_vis[2] * W, g_vis[2] * W, g_vis[0] * W }, cv[4] = { g_vis[1] * H, g_vis[1] * H, g_vis[3] * H, g_vis[3] * H };
            ImVec2 q[4];
            for (int i = 0; i < 4; ++i) { float x, y; apply_h(inv, cu[i], cv[i], x, y); q[i] = ImVec2(x, y); }
            for (int i = 0; i < 4; ++i) dl->AddLine(q[i], q[(i + 1) % 4], IM_COL32(0, 255, 120, 200), 2.0f);
            for (int i = 0; i < NM; ++i) { float x, y; apply_h(inv, g_marker[i][0], g_marker[i][1], x, y); dl->AddCircle(ImVec2(x, y), 4.0f, IM_COL32(0, 255, 120, 200), 12, 1.5f); }
            char t[64]; snprintf(t, sizeof(t), "calibrated (error %.1f texels)", g_cal_err);
            dl->AddText(q[0], IM_COL32(0, 255, 120, 230), t);
        }
    }
    if (show_msg && (g_probe >= 0 || !g_cal_ok)) dl->AddText(ImVec2(c.x + 16, c.y + 10), IM_COL32(255, 255, 255, 220), g_probe >= 0 ? "calibrating..." : g_cal_msg.c_str());
}

void on_reshade_present(effect_runtime *rt)
{
    // cursor mode: hotkey or double middle click; Esc leaves it
    // F10: toggle the cursor; F10 twice quickly: recalibrate (look at the screen first)
    static ULONGLONG last_key_ms = 0;
    bool toggle = false;
    if (rt->is_key_pressed(g_cursor_key))
    {
        const ULONGLONG now = GetTickCount64();
        if (now - last_key_ms < 400) { last_key_ms = 0; g_cursor = true; g_cur_down = false; start_calibration(); }
        else { last_key_ms = now; toggle = true; }
    }
    if (rt->is_mouse_button_pressed(2))
    {
        const ULONGLONG now = GetTickCount64();
        if (now - g_last_mid_ms < 400) { toggle = true; g_last_mid_ms = 0; } else g_last_mid_ms = now;
    }
    if (toggle) set_cursor_mode(!g_cursor);
    else if (g_cursor && rt->is_key_pressed(VK_ESCAPE)) set_cursor_mode(false);
    if (g_cursor) { rt->block_input_next_frame(); cursor_input(rt); if (g_probe < 0) forward_keys(rt); }
    g_cap.keep_behind = g_hide_window;
    if (rt->is_key_pressed(g_hotkey)) { g_enabled = !g_enabled; reshade::set_config_value(nullptr, SEC, "Enabled", int(g_enabled)); }
}

// ---------------------------------------------------------------------------------------------- scrcpy
bool run_in_job(const std::string &cmdline, const std::string &dir, DWORD flags, WORD show, bool wait, HANDLE *stdin_write = nullptr);
std::string find_scrcpy()
{
    if (g_scrcpy[0] && GetFileAttributesA(g_scrcpy) != INVALID_FILE_ATTRIBUTES) return g_scrcpy;
    const std::string bundled = addon_dir() + "\\scrcpy\\scrcpy.exe"; // shipped with the mod
    if (GetFileAttributesA(bundled.c_str()) != INVALID_FILE_ATTRIBUTES) return bundled;
    char buf[MAX_PATH];
    if (SearchPathA(nullptr, "scrcpy.exe", nullptr, MAX_PATH, buf, nullptr)) return buf;
    char la[MAX_PATH]; GetEnvironmentVariableA("LOCALAPPDATA", la, MAX_PATH);
    WIN32_FIND_DATAA fd; std::string base = std::string(la) + "\\Microsoft\\WinGet\\Packages\\";
    HANDLE h = FindFirstFileA((base + "Genymobile.scrcpy*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        std::string pkg = base + fd.cFileName + "\\"; FindClose(h);
        h = FindFirstFileA((pkg + "scrcpy-win64*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) { std::string p = pkg + fd.cFileName + "\\scrcpy.exe"; FindClose(h); return p; }
    }
    return "";
}

std::string addon_dir()
{
    char me[MAX_PATH]; HMODULE m = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&addon_dir, &m);
    GetModuleFileNameA(m, me, MAX_PATH);
    return std::string(me).substr(0, std::string(me).find_last_of("\\/")) + "\\dbm_phone";
}

std::string read_text(const std::string &fn)
{
    std::string text;
    if (FILE *f = fopen(fn.c_str(), "rb")) { char b[8192]; size_t n = fread(b, 1, sizeof(b) - 1, f); b[n] = 0; text = b; fclose(f); }
    return text;
}

void start_scrcpy()
{
    g_cap.set_title(TITLE);
    const std::string exe = find_scrcpy();
    if (exe.empty()) { g_msg = "scrcpy.exe not found (install: winget install Genymobile.scrcpy)"; return; }
    const std::string dir = exe.substr(0, exe.find_last_of('\\'));
    // one adb for everything: scrcpy's own adb is another version, and two adb versions kill each other's server
    // (= random disconnects of the phone / Android Auto)
    std::string adb = addon_dir() + "\\adb\\adb.exe";
    if (GetFileAttributesA(adb.c_str()) == INVALID_FILE_ATTRIBUTES) adb = dir + "\\adb.exe";
    SetEnvironmentVariableA("ADB", adb.c_str());
    char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
    const std::string log = std::string(tmp) + "dbm_phone_scrcpy.log";
    // the phone's hardware encoder sometimes hangs (e.g. after Android Auto): then fall back to the software one
    const char *encoders[] = { "", " --video-encoder=c2.android.avc.encoder" };
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        char args[640];
        if (g_mode == 1)
            snprintf(args, sizeof(args), "--new-display=%dx%d/%d --window-title=\"DBM Phone\" --window-borderless --stay-awake%s%s",
                     g_vd_w, g_vd_h, g_dpi, g_screen_off ? " --turn-screen-off" : "", encoders[attempt]);
        else
            snprintf(args, sizeof(args), "--window-title=\"DBM Phone\" --window-borderless --stay-awake --max-size=1600 --max-fps=%d%s%s", g_rate, g_screen_off ? " --turn-screen-off" : "", encoders[attempt]);
        DeleteFileA(log.c_str());
        stop_helpers();
        // through cmd only to capture scrcpy's log (display id, first video frame)
        if (!run_in_job("cmd /c \"\"" + exe + "\" " + args + " > \"" + log + "\" 2>&1\"", dir, CREATE_NO_WINDOW, SW_HIDE, false))
        { g_msg = "failed to start scrcpy"; return; }
        g_msg = std::string(attempt ? "retrying with software encoder: " : "starting: scrcpy ") + args;
        bool launched = false, video = false;
        for (int i = 0; i < 48 && !video; ++i) // up to 12 s
        {
            Sleep(250);
            const std::string text = read_text(log);
            if (text.find("ERROR") != std::string::npos && text.find("Texture") == std::string::npos && i > 8) break;
            video = text.find("Texture:") != std::string::npos;
            const size_t at = text.find("New display:");
            const size_t id = at == std::string::npos ? at : text.find("(id=", at);
            if (g_mode == 1 && g_app[0] && !launched && id != std::string::npos)
            {
                // the phone opens apps on a second display as small freeform windows: start the app fullscreen ourselves
                launched = true;
                const int disp = atoi(text.c_str() + id + 4);
                char cmd[512];
                snprintf(cmd, sizeof(cmd), "\"%s\" shell am start --display %d --windowingMode 1 -a android.intent.action.MAIN "
                         "-c android.intent.category.LAUNCHER -p %s -f 0x10000000", adb.c_str(), disp, g_app);
                run_in_job(cmd, dir, CREATE_NO_WINDOW, SW_HIDE, true);
            }
        }
        if (video) { g_msg = std::string("phone connected") + (attempt ? " (software encoder)" : "") + (g_mode == 1 ? std::string(", app: ") + g_app : ""); return; }
    }
    g_msg = "no picture from the phone (unlock it, check USB debugging; restart the phone if it persists)";
}

bool file_exists(const std::string &f) { return GetFileAttributesA(f.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string find_adb()
{
    const std::string a = addon_dir() + "\\adb\\adb.exe";
    if (file_exists(a)) return a;
    const std::string s = find_scrcpy();
    return s.empty() ? "" : s.substr(0, s.find_last_of('\\')) + "\\adb.exe";
}

// setup guide: is a phone connected and authorised?
void check_phone()
{
    const std::string adb = find_adb();
    if (adb.empty() || !file_exists(adb)) { g_adb_state = 5; g_adb_busy = false; return; }
    char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
    const std::string out = std::string(tmp) + "dbm_phone_adb.txt";
    DeleteFileA(out.c_str());
    run_in_job("cmd /c \"\"" + adb + "\" devices -l > \"" + out + "\" 2>&1\"", tmp, CREATE_NO_WINDOW, SW_HIDE, true);
    const std::string t = read_text(out);
    int st = 1; std::string model;
    size_t p = t.find("attached");
    while (p != std::string::npos && (p = t.find('\n', p)) != std::string::npos)
    {
        ++p;
        const size_t e = t.find('\n', p);
        const std::string line = t.substr(p, e == std::string::npos ? std::string::npos : e - p);
        char serial[128] = "", state[64] = "";
        if (sscanf(line.c_str(), "%127s %63s", serial, state) != 2) continue;
        const int s2 = !strcmp(state, "device") ? 3 : !strcmp(state, "unauthorized") ? 2 : !strcmp(state, "offline") ? 4 : 1;
        if (s2 > st || (s2 == 3)) st = s2;
        const size_t m = line.find("model:");
        if (s2 == 3 && m != std::string::npos) { model = line.substr(m + 6); model = model.substr(0, model.find(' ')); for (char &c : model) if (c == '_') c = ' '; }
        if (st == 3) break;
    }
    { std::lock_guard<std::recursive_mutex> l(g_mutex); g_adb_model = model; }
    g_adb_state = st; g_adb_busy = false;
}

std::string find_browser()
{
    const char *keys[] = { "chrome.exe", "msedge.exe" };
    for (const char *k : keys)
        for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
        {
            char b[MAX_PATH]; DWORD n = sizeof(b);
            const std::string sub = std::string("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + k;
            if (RegGetValueA(root, sub.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, b, &n) == ERROR_SUCCESS && file_exists(b)) return b;
        }
    char pf[MAX_PATH], pf86[MAX_PATH], la[MAX_PATH];
    GetEnvironmentVariableA("ProgramFiles", pf, MAX_PATH); GetEnvironmentVariableA("ProgramFiles(x86)", pf86, MAX_PATH); GetEnvironmentVariableA("LOCALAPPDATA", la, MAX_PATH);
    const std::string c[] = { std::string(pf) + "\\Google\\Chrome\\Application\\chrome.exe", std::string(pf86) + "\\Google\\Chrome\\Application\\chrome.exe",
                              std::string(la) + "\\Google\\Chrome\\Application\\chrome.exe", std::string(pf86) + "\\Microsoft\\Edge\\Application\\msedge.exe",
                              std::string(pf) + "\\Microsoft\\Edge\\Application\\msedge.exe" };
    for (const std::string &f : c) if (file_exists(f)) return f;
    return "";
}

// the browser as a frameless app window with its own profile (logins persist in <addon>\dbm_phone\browser);
// DevTools port for input, occlusion/background throttling off so it keeps playing behind the game
void start_browser()
{
    const std::string exe = find_browser();
    if (exe.empty()) { g_msg = "Chrome or Edge not found - install Google Chrome"; return; }
    stop_helpers();
    g_cap.set_title(L"<job>");
    uint32_t tw = 1024, th = 512;
    { std::lock_guard<std::recursive_mutex> l(g_mutex); if (g_target && g_rts.count(g_target)) { tw = g_rts[g_target].desc.texture.width; th = g_rts[g_target].desc.texture.height; } }
    const double va = (g_vis[3] - g_vis[1]) * th / std::max(1.0, double(g_vis[2] - g_vis[0]) * tw);
    const int ww = 1280, wh = int(1280.0 * std::clamp(va, 0.3, 1.5)) + 32;
    const std::string prof = addon_dir() + "\\browser";
    char args[1600];
    snprintf(args, sizeof(args), "\"%s\" --app=\"%s\" --user-data-dir=\"%s\" --remote-debugging-port=%d --window-size=%d,%d --window-position=0,0 "
             "--no-first-run --no-default-browser-check --disable-session-crashed-bubble --hide-crash-restore-bubble "
             "--disable-features=CalculateNativeWinOcclusion --disable-backgrounding-occluded-windows --disable-renderer-backgrounding "
             "--disable-background-timer-throttling --autoplay-policy=no-user-gesture-required",
             exe.c_str(), g_url, prof.c_str(), CDP_PORT, ww, wh);
    if (!run_in_job(args, exe.substr(0, exe.find_last_of('\\')), 0, SW_SHOWNOACTIVATE, false)) { g_msg = "failed to start the browser"; return; }
    cdp::start(CDP_PORT);
    g_msg = "browser started - sign in inside it once, it keeps its own profile";
}

void open_url()
{
    if (g_source == 1 && cdp::connected() && g_cap.window_found()) cdp::send("Page.navigate", "{\"url\":" + cdp::str(g_url) + "}");
    else std::thread(start_browser).detach();
}

// Android Auto Desktop Head Unit next to this addon's folder: <game>/bin/win_x64/dbm_phone/dhu
// Helper processes (adb server, Android Auto head unit) live in a kill-on-close job: they end together with
// the game (also on a crash), otherwise Steam keeps showing the game as "stopping".

HANDLE g_dhu_in = nullptr, g_dhu_proc = nullptr; // DHU console stdin (commands) and process

bool run_in_job(const std::string &cmdline, const std::string &dir, DWORD flags, WORD show, bool wait, HANDLE *stdin_write)
{
    std::lock_guard<std::mutex> l(g_job_mutex);
    if (!g_job)
    {
        g_job = CreateJobObjectA(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li {};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof(li));
        g_cap.job = g_job;
    }
    STARTUPINFOEXA si {}; si.StartupInfo.cb = sizeof(si); si.StartupInfo.dwFlags = STARTF_USESHOWWINDOW; si.StartupInfo.wShowWindow = show;
    PROCESS_INFORMATION pi {};
    std::string cl = cmdline;
    HANDLE rd = nullptr, wr = nullptr, nul = nullptr;
    std::vector<uint8_t> attr_buf; LPPROC_THREAD_ATTRIBUTE_LIST attrs = nullptr;
    HANDLE inherit[2] = {};
    if (stdin_write)
    {
        // only the pipe's read end (and NUL for output) are inherited, nothing else of the game's handles
        SECURITY_ATTRIBUTES sa { sizeof(sa), nullptr, TRUE };
        if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
        nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = rd; si.StartupInfo.hStdOutput = nul; si.StartupInfo.hStdError = nul;
        inherit[0] = rd; inherit[1] = nul;
        SIZE_T sz = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &sz);
        attr_buf.resize(sz); attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
        InitializeProcThreadAttributeList(attrs, 1, 0, &sz);
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);
        si.lpAttributeList = attrs;
    }
    const BOOL ok = CreateProcessA(nullptr, cl.data(), nullptr, nullptr, stdin_write ? TRUE : FALSE,
                                   flags | CREATE_SUSPENDED | (attrs ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr, dir.c_str(), &si.StartupInfo, &pi);
    if (attrs) DeleteProcThreadAttributeList(attrs);
    if (rd) CloseHandle(rd);
    if (nul) CloseHandle(nul);
    if (!ok) { if (wr) CloseHandle(wr); return false; }
    if (stdin_write) { *stdin_write = wr; if (g_dhu_proc) CloseHandle(g_dhu_proc); DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &g_dhu_proc, 0, FALSE, DUPLICATE_SAME_ACCESS); }
    AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    if (wait) WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

// send a console command to the Android Auto head unit (e.g. "day", "night", "keycode media_next")
void dhu_command(const char *cmd)
{
    std::lock_guard<std::mutex> l(g_job_mutex);
    if (!g_dhu_in) return;
    DWORD n; std::string c = std::string(cmd) + "\n";
    WriteFile(g_dhu_in, c.data(), DWORD(c.size()), &n, nullptr);
}

void stop_helpers()
{
    std::lock_guard<std::mutex> l(g_job_mutex);
    // graceful "quit" first: a killed head unit leaves the phone's head unit server stuck on the dead session
    if (g_dhu_in)
    {
        DWORD n; WriteFile(g_dhu_in, "quit\n", 5, &n, nullptr);
        if (g_dhu_proc) WaitForSingleObject(g_dhu_proc, 3000);
        CloseHandle(g_dhu_in); g_dhu_in = nullptr;
    }
    if (g_dhu_proc) { CloseHandle(g_dhu_proc); g_dhu_proc = nullptr; }
    g_cap.job = nullptr;
    if (g_job) { TerminateJobObject(g_job, 0); CloseHandle(g_job); g_job = nullptr; }
    cdp::stop();
}

// Android Auto Desktop Head Unit shipped next to this add-on: <game>/bin/win_x64/dbm_phone/{adb,dhu}
void start_dhu()
{
    char me[MAX_PATH]; HMODULE m = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&start_dhu, &m);
    GetModuleFileNameA(m, me, MAX_PATH);
    const std::string dir = std::string(me).substr(0, std::string(me).find_last_of("\\/")) + "/dbm_phone";
    SetEnvironmentVariableA("ADB", (dir + "/adb/adb.exe").c_str());
    stop_helpers(); // restart cleanly
    g_cap.set_title(TITLE);
    if (!run_in_job("\"" + dir + "/adb/adb.exe\" forward tcp:5277 tcp:5277", dir, CREATE_NO_WINDOW, SW_HIDE, true))
    { g_msg = "missing " + dir + "/adb/adb.exe"; return; }
    HANDLE in = nullptr;
    if (!run_in_job("\"" + dir + "/dhu/desktop-head-unit.exe\" -c config/dbm_truck.ini", dir + "/dhu", CREATE_NEW_CONSOLE, SW_SHOWMINNOACTIVE, false, &in))
    { g_msg = "missing " + dir + "/dhu/desktop-head-unit.exe"; return; }
    { std::lock_guard<std::mutex> l(g_job_mutex); g_dhu_in = in; }
    g_msg = "Android Auto started (phone: Android Auto -> Start head unit server)";
}

HWND phone_hwnd() { return static_cast<HWND>(g_cap.hwnd()); }

void send_mouse(UINT msg, WPARAM wp, int x, int y)
{
    HWND h = phone_hwnd();
    if (!h) return;
    if (msg != WM_MOUSEMOVE) PostMessageW(h, WM_MOUSEMOVE, msg == WM_LBUTTONUP ? MK_LBUTTON : 0, MAKELPARAM(x, y));
    PostMessageW(h, msg, wp, MAKELPARAM(x, y));
}

// ---------------------------------------------------------------------------------------------- overlay
const char *fmt_name(format f)
{
    switch (format_to_typeless(f)) {
    case format::r8g8b8a8_typeless: return "RGBA8"; case format::b8g8r8a8_typeless: return "BGRA8"; case format::b8g8r8x8_typeless: return "BGRX8";
    case format::r10g10b10a2_typeless: return "RGB10A2"; case format::r16g16b16a16_typeless: return "RGBA16F"; case format::r11g11b10_float: return "R11G11B10F";
    default: return "?"; }
}

void on_overlay(effect_runtime *)
{
    std::lock_guard<std::recursive_mutex> l(g_mutex);
    const ImVec4 ok(.35f, .9f, .45f, 1), warn(1, .75f, .25f, 1), bad(1, .45f, .4f, 1);
    if (ImGui::Checkbox("Show on the truck screen", &g_enabled)) reshade::set_config_value(nullptr, SEC, "Enabled", int(g_enabled));
    ImGui::SameLine(); ImGui::TextDisabled(g_hotkey == VK_F9 ? "(F9)" : "(toggle key 0x%02X)", g_hotkey);
    ImGui::SetNextItemWidth(300);
    if (ImGui::Combo("Source", &g_source, "Android phone (USB)\0PC browser (YouTube, Spotify, Maps...)\0"))
    { reshade::set_config_value(nullptr, SEC, "Source", g_source); std::thread(stop_helpers).detach(); }
    ImGui::Separator();

    // 1. which in-game screen
    ImGui::TextUnformatted("1. Truck screen:"); ImGui::SameLine();
    if (g_detect_on) ImGui::TextColored(warn, "detecting...");
    else if (g_target) { const resource_desc &d = g_rts[g_target].desc; ImGui::TextColored(ok, "ready (%ux%u)", d.texture.width, d.texture.height); }
    else if (g_key_w) ImGui::TextColored(warn, "remembered - sit in the cab");
    else ImGui::TextColored(bad, "not set");
    ImGui::SameLine();
    if (ImGui::Button(g_detect_on ? "Detecting..." : "Detect screen") && !g_detect_on) start_detect();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Look straight at the screen you want (camera still), then click.\nFinds the screen and its orientation automatically.");
    if (!g_detect_on && g_detect_rank.size() > 1)
    {
        ImGui::SameLine();
        char nb[48]; snprintf(nb, sizeof(nb), "Wrong one? Next (%d/%zu)", g_detect_pos + 1, g_detect_rank.size());
        if (ImGui::Button(nb))
        {
            for (size_t tries = 0; tries < g_detect_rank.size(); ++tries)
            {
                g_detect_pos = int((g_detect_pos + 1) % g_detect_rank.size());
                if (!g_rts.count(g_detect_rank[g_detect_pos])) continue;
                set_target(g_detect_rank[g_detect_pos]);
                if (g_detect_rank_res[g_detect_pos].green > 20) { set_flip(true, g_detect_rank_res[g_detect_pos].fv); set_flip(false, g_detect_rank_res[g_detect_pos].fh); }
                g_conv_dirty = true; g_cal_ok = false;
                break;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Other screens that also reacted during detection - click until the right one shows the picture.");
    }
    ImGui::TextUnformatted("   Picture:"); ImGui::SameLine();
    { bool fv = g_flip_v, fh = g_flip_h;
      if (ImGui::Checkbox("Flip vertically", &fv)) { set_flip(true, fv); g_cal_ok = false; }
      ImGui::SameLine(); if (ImGui::Checkbox("Flip horizontally", &fh)) { set_flip(false, fh); g_cal_ok = false; } }
    ImGui::SameLine(); ImGui::TextDisabled("(F10 calibration also fixes this)");

    // 2. what to show
    if (g_source == 0)
    {
        const ULONGLONG now = GetTickCount64();
        if (!g_cap.window_found() && !g_adb_busy && now - g_adb_last > 3000) { g_adb_last = now; g_adb_busy = true; std::thread(check_phone).detach(); }
        ImGui::TextUnformatted("2. Phone:"); ImGui::SameLine();
        if (g_cap.window_found()) ImGui::TextColored(ok, "live (%.0f fps)", g_cap.fps());
        else switch (g_adb_state.load())
        {
        case 3: ImGui::TextColored(ok, "%s connected", g_adb_model.empty() ? "phone" : g_adb_model.c_str()); break;
        case 2: ImGui::TextColored(warn, "unlock the phone and tap 'Allow' on the USB debugging prompt"); break;
        case 4: ImGui::TextColored(warn, "offline - unplug and plug the cable back in"); break;
        case 1: ImGui::TextColored(bad, "not found"); break;
        case 5: ImGui::TextColored(bad, "scrcpy / adb missing (install: winget install Genymobile.scrcpy)"); break;
        default: ImGui::TextDisabled("checking..."); break;
        }
        if (!g_cap.window_found() && g_adb_state == 1)
            ImGui::TextWrapped("   Plug the phone in with USB and turn on USB debugging: Settings > About phone > tap 'Build number' 7 times, "
                               "then Settings > Developer options > USB debugging.");
        ImGui::SameLine(); if (ImGui::Button("Start phone")) { g_msg = "starting the phone..."; std::thread(start_scrcpy).detach(); }
        ImGui::SameLine(); if (ImGui::Checkbox("Phone screen off", &g_screen_off)) reshade::set_config_value(nullptr, SEC, "ScreenOff", int(g_screen_off));
    }
    else
    {
        ImGui::TextUnformatted("2. Browser:"); ImGui::SameLine();
        if (g_cap.window_found() && cdp::connected()) ImGui::TextColored(ok, "live (%.0f fps)", g_cap.fps());
        else if (g_cap.window_found()) ImGui::TextColored(warn, "starting...");
        else if (find_browser().empty()) ImGui::TextColored(bad, "Chrome or Edge not found");
        else ImGui::TextDisabled("not started");
        ImGui::SetNextItemWidth(380);
        if (ImGui::InputText("##url", g_url, sizeof(g_url))) reshade::set_config_value(nullptr, SEC, "Url", static_cast<const char *>(g_url));
        ImGui::SameLine(); if (ImGui::Button(g_cap.window_found() ? "Open" : "Start browser")) { reshade::set_config_value(nullptr, SEC, "Url", static_cast<const char *>(g_url)); open_url(); }
        static const char *presets[][2] = { { "YouTube", "https://www.youtube.com" }, { "YouTube Music", "https://music.youtube.com" },
                                            { "Spotify", "https://open.spotify.com" }, { "Google Maps", "https://www.google.com/maps" } };
        for (int i = 0; i < 4; ++i)
        {
            if (i) ImGui::SameLine();
            if (ImGui::SmallButton(presets[i][0])) { snprintf(g_url, sizeof(g_url), "%s", presets[i][1]); reshade::set_config_value(nullptr, SEC, "Url", static_cast<const char *>(g_url)); open_url(); }
        }
    }
    ImGui::TextUnformatted("3. In game: F10 = cursor on the screen, F10 twice = recalibrate, Esc = exit. Typing goes to the screen.");
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderFloat("Volume", &g_volume, 0.0f, 1.0f, "%.2f")) reshade::set_config_value(nullptr, SEC, "Volume", g_volume);
    ImGui::SameLine(); ImGui::SetNextItemWidth(160);
    {
        int ri = g_rate >= 60 ? 2 : g_rate >= 30 ? 1 : 0;
        if (ImGui::Combo("Screen fps", &ri, "15 (lightest)\0" "30 (recommended)\0" "60 (smoothest)\0"))
        { g_rate = ri == 0 ? 15 : ri == 1 ? 30 : 60; g_cap.max_fps = g_rate; reshade::set_config_value(nullptr, SEC, "Rate", g_rate); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How often the truck screen updates. Lower = less load on the game. Phone: press Start phone again to apply.");
    }
    ImGui::SetNextItemWidth(220);
    int res_i = g_res == 4 ? 3 : std::clamp(g_res, 0, 2);
    if (ImGui::Combo("Sharpness", &res_i, "Auto (from the screen size)\0" "1x (game resolution)\0" "2x\0" "4x\0"))
    {
        g_res = res_i == 3 ? 4 : res_i; // stored as the factor
        reshade::set_config_value(nullptr, SEC, "Resolution", g_res); destroy_phone_tex(); g_cal_ok = false;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Resolution of the picture on the truck screen. Small screens (e.g. Mercedes) get more pixels in Auto.\nNeeds 'swap the map only in 3D draws' (Advanced).");
    if (!g_msg.empty()) ImGui::TextWrapped("%s", g_msg.c_str());

    if (ImGui::CollapsingHeader("Advanced"))
    {
        ImGui::Text("Capture: %s  %ux%u  %.0f fps", g_cap.status().c_str(), g_frame.w, g_frame.h, g_cap.fps());
        if (g_target) { const resource_desc &d = g_rts[g_target].desc; ImGui::Text("Screen texture: %ux%u %s mips %u (#%u)", d.texture.width, d.texture.height, fmt_name(d.texture.format), d.texture.levels, g_key_idx); }
        if (ImGui::Button(g_picking ? "Close picker" : "Pick screen manually")) { g_picking = !g_picking; if (!g_picking) drop_thumbs(); }
        ImGui::SameLine(); if (ImGui::Button("Forget screen")) { set_target(0); g_key_w = 0; reshade::set_config_value(nullptr, SEC, "TargetW", 0); }
        if (ImGui::Checkbox("Keep aspect", &g_fit)) { reshade::set_config_value(nullptr, SEC, "Fit", int(g_fit)); g_conv_dirty = true; }
        if (ImGui::Checkbox("Keep the source window behind the game", &g_hide_window)) reshade::set_config_value(nullptr, SEC, "HideWindow", int(g_hide_window));
        ImGui::SameLine(); ImGui::SetNextItemWidth(140);
        if (ImGui::SliderFloat("Brightness", &g_brightness, 0.3f, 1.5f)) { g_conv_dirty = true; reshade::set_config_value(nullptr, SEC, "Brightness", int(g_brightness * 100)); }
        ImGui::SetNextItemWidth(300);
        if (ImGui::InputInt4("Crop L/T/R/B", g_crop)) { g_conv_dirty = true; char b[64]; snprintf(b, 64, "%d,%d,%d,%d", g_crop[0], g_crop[1], g_crop[2], g_crop[3]); reshade::set_config_value(nullptr, SEC, "Crop", static_cast<const char *>(b)); }
        if (g_source == 0)
        {
            ImGui::SetNextItemWidth(220);
            if (ImGui::Combo("Phone mode", &g_mode, "Mirror phone screen\0Separate display (screen-sized)\0")) reshade::set_config_value(nullptr, SEC, "Mode", g_mode);
            if (g_mode == 1)
            {
                ImGui::SetNextItemWidth(160); if (ImGui::InputInt2("size", &g_vd_w)) { reshade::set_config_value(nullptr, SEC, "DisplayW", g_vd_w); reshade::set_config_value(nullptr, SEC, "DisplayH", g_vd_h); }
                ImGui::SameLine(); ImGui::SetNextItemWidth(90); if (ImGui::InputInt("dpi", &g_dpi)) reshade::set_config_value(nullptr, SEC, "Dpi", g_dpi);
                ImGui::SameLine(); ImGui::SetNextItemWidth(260); if (ImGui::InputText("app", g_app, sizeof(g_app))) reshade::set_config_value(nullptr, SEC, "App", static_cast<const char *>(g_app));
            }
            if (ImGui::Button("Start Android Auto (DHU)")) { g_msg = "starting Android Auto..."; std::thread(start_dhu).detach(); }
        }
        if (ImGui::Checkbox("Experimental: swap the map only in 3D draws", &g_only_main))
        { reshade::set_config_value(nullptr, SEC, "OnlyMain", int(g_only_main)); std::lock_guard<std::recursive_mutex> l2(g_mutex); restore_all(); destroy_phone_tex(); g_cal_ok = false; }
        if (ImGui::Button("Reset visible area")) { g_vis[0] = g_vis[1] = 0; g_vis[2] = g_vis[3] = 1; g_conv_dirty = true; reshade::set_config_value(nullptr, SEC, "Visible", static_cast<const char *>("0 0 1 1")); }
        ImGui::SameLine(); ImGui::TextDisabled("(%.2f %.2f - %.2f %.2f, measured by the F10 calibration)", g_vis[0], g_vis[1], g_vis[2], g_vis[3]);
        ImGui::Checkbox("Show calibration outline", &g_cal_dbg);
        if (g_cal_ok) { ImGui::SameLine(); ImGui::TextDisabled("[calibrated, err %.1f]", g_cal_err); }
    }

    if (g_picking && g_device)
    {
        ImGui::Separator();
        ImGui::TextWrapped("Click the thumbnail that shows the truck's navigation screen (sit in the cab, nav visible).");
        ImGui::Checkbox("Show all textures (mirrors, scene buffers, not written recently)", &g_show_all);
        ImGui::SameLine(); if (ImGui::Button("Dump for DBM")) g_dump_request = true;
        std::vector<std::pair<uint64_t, uint64_t>> list; // (~last seen, res) -> most recent first
        for (auto &[r, info] : g_rts)
        {
            auto si = g_seen.find(r);
            const uint64_t last = si == g_seen.end() ? 0 : si->second.last;
            if (!g_show_all && (last == 0 || g_frame_no - last > 300)) continue; // ordering below is stable (no jumping)
            if (!g_show_all && info.desc.texture.format != format::r8g8b8a8_unorm_srgb) continue; // mirrors, scene buffers...
            const resource_desc &d = info.desc;
            const bool likely = d.texture.format == format::r8g8b8a8_unorm_srgb; // cab screens are drawn sRGB
            const uint64_t area = uint64_t(d.texture.width) * d.texture.height;
            const uint32_t order = si == g_seen.end() ? 0xffffff : si->second.order;
            list.push_back({ (uint64_t(likely ? 0 : 1) << 63) | ((0xffffffffull - area) << 24) | (order & 0xffffff), r });
        }
        std::sort(list.begin(), list.end());
        int n = 0;
        for (auto &[neg, r] : list)
        {
            auto it = g_rts.find(r);
            if (it == g_rts.end() || !it->second.srv_ok) continue;
            const resource_desc &d = it->second.desc;
            if (d.texture.width >= 2560 || d.texture.samples > 1) continue; // full-screen / multisampled buffers
            auto &v = g_thumbs[r];
            if (!v.handle)
                g_device->create_resource_view(resource { r }, resource_usage::shader_resource,
                                               resource_view_desc(view_format(d.texture.format), 0, 1, 0, 1), &v);
            if (!v.handle) continue;
            const float tw = 220, th = tw * d.texture.height / d.texture.width;
            if (n++ % 3) ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::PushID(int(r));
            if (ImGui::ImageButton("t", (ImTextureID)v.handle, ImVec2(tw, std::min(th, 220.0f)))) { set_target(r); g_picking = false; }
            ImGui::PopID();
            const seen_info si = g_seen.count(r) ? g_seen[r] : seen_info {};
            ImGui::Text("%ux%u %s %s%s%s%s", d.texture.width, d.texture.height, fmt_name(d.texture.format),
                        si.kind & 1 ? "R" : "", si.kind & 2 ? "M" : "", si.kind & 4 ? "C" : "", r == g_target ? " [current]" : (d.texture.format == format::r8g8b8a8_unorm_srgb ? " cab screen" : ""));
            ImGui::EndGroup();
        }
        if (!n) ImGui::TextUnformatted("No candidate screens written recently.");
    }
    else if (g_phone_srv.handle && g_frame.w)
    {
        // interactive phone: clicks are forwarded to the scrcpy window
        ImGui::Separator();
        ImGui::TextDisabled("Preview (interactive): left = tap/drag, right = back, wheel = scroll");
        const float avail = ImGui::GetContentRegionAvail().x;
        const float dw = std::min(avail, 900.0f);
        const resource_desc &d = g_phone_desc;
        const float dh = dw * d.texture.height / d.texture.width;
        // shown un-flipped, so the touch mapping below works in phone orientation
        ImGui::Image((ImTextureID)g_phone_srv.handle, ImVec2(dw, dh), ImVec2(g_flip_h ? 1.f : 0.f, g_flip_v ? 1.f : 0.f), ImVec2(g_flip_h ? 0.f : 1.f, g_flip_v ? 0.f : 1.f));
        if (ImGui::IsItemHovered())
        {
            const ImVec2 mn = ImGui::GetItemRectMin(), mp = ImGui::GetIO().MousePos;
            const float u = (mp.x - mn.x) / dw * d.texture.width, v = (mp.y - mn.y) / dh * d.texture.height;
            int px = 0, py = 0;
            if (display_to_client(u, v, px, py))
            {
                static bool down = false;
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { ptr_down(px, py); down = true; }
                else if (down && ImGui::IsMouseDown(ImGuiMouseButton_Left)) ptr_drag(px, py);
                if (down && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) { ptr_up(px, py); down = false; }
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ptr_back(px, py);
                if (g_source == 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) { send_mouse(WM_MBUTTONDOWN, MK_MBUTTON, px, py); send_mouse(WM_MBUTTONUP, 0, px, py); }
                const float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0) ptr_wheel(px, py, wheel);
            }
        }
    }
}

void load_config()
{
    reshade::get_config_value(nullptr, SEC, "Enabled", g_enabled);
    reshade::get_config_value(nullptr, SEC, "Hotkey", g_hotkey);
    reshade::get_config_value(nullptr, SEC, "Mode", g_mode);
    reshade::get_config_value(nullptr, SEC, "Dpi", g_dpi);
    { int v = int(g_only_main); reshade::get_config_value(nullptr, SEC, "OnlyMain", v); g_only_main = v != 0; }
    reshade::get_config_value(nullptr, SEC, "Volume", g_volume);
    reshade::get_config_value(nullptr, SEC, "CursorKey", g_cursor_key);
    reshade::get_config_value(nullptr, SEC, "Source", g_source);
    reshade::get_config_value(nullptr, SEC, "Rate", g_rate);
    reshade::get_config_value(nullptr, SEC, "Resolution", g_res);
    { char vb[96] = ""; size_t n = sizeof(vb);
      if (reshade::get_config_value(nullptr, SEC, "Visible", vb, &n)) { float v[4]; if (sscanf(vb, "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]) == 4 && v[2] > v[0] && v[3] > v[1]) memcpy(g_vis, v, sizeof(v)); } }
    g_cap.max_fps = g_rate;
    { size_t n = sizeof(g_url); reshade::get_config_value(nullptr, SEC, "Url", g_url, &n); }
    { int v = int(g_screen_off); reshade::get_config_value(nullptr, SEC, "ScreenOff", v); g_screen_off = v != 0; }
    { int v = 0; reshade::get_config_value(nullptr, SEC, "FlipV", v); g_flip_v = v != 0; v = 0; reshade::get_config_value(nullptr, SEC, "FlipH", v); g_flip_h = v != 0; }
    reshade::get_config_value(nullptr, SEC, "DisplayW", g_vd_w);
    reshade::get_config_value(nullptr, SEC, "DisplayH", g_vd_h);
    { size_t n = sizeof(g_app); reshade::get_config_value(nullptr, SEC, "App", g_app, &n); }
    reshade::get_config_value(nullptr, SEC, "Fit", g_fit);
    reshade::get_config_value(nullptr, SEC, "HideWindow", g_hide_window);
    int b = 100; if (reshade::get_config_value(nullptr, SEC, "Brightness", b)) g_brightness = b / 100.0f;
    reshade::get_config_value(nullptr, SEC, "TargetW", g_key_w);
    reshade::get_config_value(nullptr, SEC, "TargetH", g_key_h);
    reshade::get_config_value(nullptr, SEC, "TargetFormat", g_key_fmt);
    reshade::get_config_value(nullptr, SEC, "TargetIndex", g_key_idx);
    { char b[64]; size_t m = sizeof(b); if (reshade::get_config_value(nullptr, SEC, "Crop", b, &m)) sscanf(b, "%d,%d,%d,%d", &g_crop[0], &g_crop[1], &g_crop[2], &g_crop[3]); }
    size_t n = sizeof(g_scrcpy); reshade::get_config_value(nullptr, SEC, "ScrcpyPath", g_scrcpy, &n);
    reshade::set_config_value(nullptr, SEC, "Hotkey", g_hotkey);
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module)) return FALSE;
        load_config();
        reshade::register_event<reshade::addon_event::init_device>(on_init_device);
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
        reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
        reshade::register_event<reshade::addon_event::init_command_list>(on_init_cmd);
        reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_cmd);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_rts);
        reshade::register_event<reshade::addon_event::present>(on_present);
        reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
        reshade::register_event<reshade::addon_event::resolve_texture_region>(on_resolve);
        reshade::register_event<reshade::addon_event::copy_texture_region>(on_copy_region);
        reshade::register_event<reshade::addon_event::copy_resource>(on_copy_res);
        reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
        reshade::register_event<reshade::addon_event::reshade_overlay>(on_cursor_overlay);
        reshade::register_overlay(nullptr, on_overlay);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_addon(module);
        break;
    }
    return TRUE;
}
