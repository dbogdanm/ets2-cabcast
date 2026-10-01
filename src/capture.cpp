#include "capture.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <inspectable.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <chrono>
#include <dwmapi.h>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;

namespace
{
struct find_ctx { std::wstring needle; HWND found = nullptr; HANDLE job = nullptr; };

BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lp)
{
    find_ctx &c = *reinterpret_cast<find_ctx *>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;           // never message our own (game) windows
    wchar_t t[256]; InternalGetWindowText(hwnd, t, 256);    // reads the cached title, sends no message
    if (c.needle == L"<job>")
    {
        // the browser's main window: titled, not a tool window, reasonably large, owned by a process in our job
        if (!c.job || !t[0] || (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) || GetWindow(hwnd, GW_OWNER)) return TRUE;
        RECT r; GetWindowRect(hwnd, &r);
        if (r.right - r.left < 300 || r.bottom - r.top < 200) return TRUE;
        BOOL in = FALSE;
        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) { IsProcessInJob(p, c.job, &in); CloseHandle(p); }
        if (in) { c.found = hwnd; return FALSE; }
        return TRUE;
    }
    // needle may hold several alternatives separated by '|'
    size_t st = 0;
    while (st <= c.needle.size())
    {
        size_t e = c.needle.find(L'|', st); if (e == std::wstring::npos) e = c.needle.size();
        const std::wstring part = c.needle.substr(st, e - st);
        if (!part.empty() && wcsstr(t, part.c_str())) { c.found = hwnd; return FALSE; }
        st = e + 1;
    }
    return TRUE;
}

HWND find_window(const std::wstring &needle, void *job)
{
    find_ctx c { needle, nullptr, static_cast<HANDLE>(job) };
    EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&c));
    return c.found;
}
}

void window_capture::start(const std::wstring &title)
{
    if (m_thread.joinable()) return;
    m_title = title; m_quit = false;
    m_thread = std::thread([this] { run(); });
}

void window_capture::stop()
{
    m_quit = true;
    if (m_thread.joinable()) m_thread.join();
}

bool window_capture::latest(capture_frame &out)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_frame.serial == out.serial || m_frame.w == 0) return false;
    out = m_frame;
    return true;
}

void window_capture::run()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // window rects in physical pixels, like the capture

    winrt::com_ptr<ID3D11Device> dev; winrt::com_ptr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, dev.put(), nullptr, ctx.put())))
    { set_status("D3D11 device failed"); return; }
    winrt::com_ptr<IDXGIDevice> dxgi = dev.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> insp;
    CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put());
    auto rtdev = insp.as<wdx::Direct3D11::IDirect3DDevice>();

    while (!m_quit)
    {
        std::wstring title; { std::lock_guard<std::mutex> l(m_mutex); title = m_title; }
        m_retarget = false;
        HWND hwnd = find_window(title, job.load());
        if (!hwnd) { m_found = false; m_hwnd = nullptr; m_fps = 0; set_status("waiting for window"); std::this_thread::sleep_for(std::chrono::milliseconds(700)); continue; }
        m_found = true; m_hwnd = hwnd;
        try
        {
            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            wgc::GraphicsCaptureItem item { nullptr };
            winrt::check_hresult(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
            auto size = item.Size();
            auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(rtdev, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
            auto session = pool.CreateCaptureSession(item);
            try { session.IsCursorCaptureEnabled(false); } catch (...) {}
            try { session.IsBorderRequired(false); } catch (...) {}
            session.StartCapture();
            set_status("capturing");

            winrt::com_ptr<ID3D11Texture2D> staging; UINT sw = 0, sh = 0;
            auto t0 = std::chrono::steady_clock::now(); int frames = 0;
            auto last_read = t0 - std::chrono::seconds(1);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // the game's threads come first
            auto last_hide = std::chrono::steady_clock::now() - std::chrono::seconds(5);
            while (!m_quit && !m_retarget && IsWindow(hwnd))
            {
                if (std::chrono::steady_clock::now() - last_hide > std::chrono::milliseconds(1000))
                {
                    // minimized / off-screen windows stop rendering: always keep it restored and on-screen;
                    // 'keep_behind' additionally sends it to the bottom of the z-order
                    last_hide = std::chrono::steady_clock::now();
                    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                    RECT r; GetWindowRect(hwnd, &r);
                    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                    UINT fl = SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;
                    if (r.left >= vx && r.left < vx + vw) fl |= SWP_NOMOVE;
                    if (!keep_behind) fl |= SWP_NOZORDER;
                    if (keep_behind || !(fl & SWP_NOMOVE)) SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, fl);
                    HWND game = static_cast<HWND>(game_hwnd.load());
                    if (keep_behind && game && GetForegroundWindow() == hwnd) SetForegroundWindow(game);
                }
                auto f = pool.TryGetNextFrame();
                if (!f) { std::this_thread::sleep_for(std::chrono::milliseconds(4)); continue; }
                // rate cap: skip frames that arrive too soon (no copy, no readback)
                const auto now = std::chrono::steady_clock::now();
                if (now - last_read < std::chrono::microseconds(1000000 / std::max(5, max_fps.load()) - 2000)) { f.Close(); continue; }
                last_read = now;
                auto cs = f.ContentSize();
                if (cs.Width != size.Width || cs.Height != size.Height)
                {
                    size = cs; pool.Recreate(rtdev, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
                    continue;
                }
                auto access = f.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                winrt::com_ptr<ID3D11Texture2D> tex;
                winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), tex.put_void()));
                D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
                if (!staging || sw != d.Width || sh != d.Height)
                {
                    D3D11_TEXTURE2D_DESC s = d; s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    s.MiscFlags = 0; s.MipLevels = 1; s.ArraySize = 1;
                    staging = nullptr; winrt::check_hresult(dev->CreateTexture2D(&s, nullptr, staging.put()));
                    sw = d.Width; sh = d.Height;
                }
                ctx->CopyResource(staging.get(), tex.get());
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m)))
                {
                    // crop to the client area (no title bar / borders); for the Android Auto head unit also drop
                    // the black margins it adds around the projected screen (marginheight in dbm_truck.ini)
                    uint32_t x0 = 0, y0 = 0, x1 = std::min<uint32_t>(sw, cs.Width), y1 = std::min<uint32_t>(sh, cs.Height);
                    int client_ox = 0, client_oy = 0;
                    RECT fr; POINT co { 0, 0 }; RECT cr;
                    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr))) && GetClientRect(hwnd, &cr) && ClientToScreen(hwnd, &co))
                    {
                        const int ox = co.x - fr.left, oy = co.y - fr.top;
                        if (ox >= 0 && oy >= 0 && cr.right > 16 && cr.bottom > 16)
                        { client_ox = ox; client_oy = oy; x0 = ox; y0 = oy; x1 = std::min<uint32_t>(x1, ox + cr.right); y1 = std::min<uint32_t>(y1, oy + cr.bottom); }
                    }
                    wchar_t title[128]; InternalGetWindowText(hwnd, title, 128);
                    if (wcsstr(title, L"Desktop Head Unit"))
                    {
                        const uint32_t ch = y1 - y0, m = uint32_t(ch * (80.0 / 720.0) / 2 + 0.5);
                        y0 += m; y1 -= m;
                    }
                    const uint32_t w = x1 - x0, h = y1 - y0;
                    std::lock_guard<std::mutex> l(m_mutex);
                    m_frame.w = w; m_frame.h = h; m_frame.client_x = int(x0) - client_ox; m_frame.client_y = int(y0) - client_oy; m_frame.bgra.resize(size_t(w) * h * 4);
                    for (uint32_t y = 0; y < h; ++y)
                        memcpy(&m_frame.bgra[size_t(y) * w * 4], static_cast<uint8_t *>(m.pData) + size_t(y + y0) * m.RowPitch + size_t(x0) * 4, size_t(w) * 4);
                    m_frame.serial++;
                    ctx->Unmap(staging.get(), 0);
                }
                f.Close();
                ++frames;
                auto dt = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
                if (dt > 1.0f) { m_fps = frames / dt; frames = 0; t0 = std::chrono::steady_clock::now(); }
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
            session.Close(); pool.Close();
        }
        catch (const winrt::hresult_error &e)
        {
            char b[128]; snprintf(b, sizeof(b), "capture error 0x%08x", (unsigned)e.code());
            set_status(b); std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
    }
    m_found = false; m_hwnd = nullptr;
}
