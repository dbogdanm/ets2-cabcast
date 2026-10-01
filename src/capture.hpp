// Window capture (Windows.Graphics.Capture) on a private D3D11 device, frames read back to a CPU BGRA buffer.
// Works for occluded / off-screen windows (not minimized ones).
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct capture_frame
{
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> bgra;   // w*h*4, top-down
    uint64_t serial = 0;         // increments on every new frame
    int client_x = 0, client_y = 0; // frame origin in window client coordinates (for input)
};

class window_capture
{
public:
    void start(const std::wstring &title_substring);
    void stop();
    // copies the latest frame if newer than 'serial'; returns true if copied
    bool latest(capture_frame &out);
    bool window_found() const { return m_found; }
    float fps() const { return m_fps; }
    std::string status() { std::lock_guard<std::mutex> l(m_mutex); return m_status; }
    // title substring(s) separated by '|'; "<job>" = any titled window of a process in 'job' (e.g. the browser)
    void set_title(const std::wstring &t) { std::lock_guard<std::mutex> l(m_mutex); if (t != m_title) { m_title = t; m_retarget = true; } }
    void *hwnd() const { return m_hwnd; }
    // window management runs on the capture thread (never on the game's render thread: cross-thread window
    // messages from there can deadlock with the game's window thread, e.g. while loading)
    std::atomic<bool> keep_behind { true };
    std::atomic<void *> game_hwnd { nullptr };
    std::atomic<void *> job { nullptr };
    std::atomic<int> max_fps { 30 };     // frames read back per second (the rest are dropped on the GPU)

private:
    void run();
    void set_status(const std::string &s) { std::lock_guard<std::mutex> l(m_mutex); m_status = s; }

    std::thread m_thread;
    std::atomic<bool> m_quit { false }, m_found { false }, m_retarget { false };
    std::atomic<void *> m_hwnd { nullptr };
    std::atomic<float> m_fps { 0 };
    std::mutex m_mutex;
    std::wstring m_title;
    std::string m_status = "idle";
    capture_frame m_frame;
};
