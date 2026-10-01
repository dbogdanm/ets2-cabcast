#include "cdp.hpp"

#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace cdp
{
namespace
{
std::thread g_worker;
std::atomic<bool> g_quit { false }, g_connected { false }, g_broken { false };
std::atomic<int> g_port { 0 };
std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<std::string> g_queue;
std::atomic<float> g_dpr { 0 };
std::atomic<int> g_iw { 0 }, g_ih { 0 };
int g_id = 1;
const int METRICS_ID = 999000;

// links that open new windows would leave the captured window: keep everything in this one
const char *KEEP_IN_WINDOW =
    "(function(){if(window.__dbm)return;window.__dbm=1;"
    "document.addEventListener('click',function(e){var a=e.target&&e.target.closest&&e.target.closest('a[target]');if(a)a.target='_self';},true);"
    "window.open=function(u){if(u)location.href=u;return null;};})()";

std::wstring widen(const std::string &s) { return std::wstring(s.begin(), s.end()); }

std::string http_get(HINTERNET session, int port, const wchar_t *path)
{
    std::string out;
    HINTERNET con = WinHttpConnect(session, L"127.0.0.1", INTERNET_PORT(port), 0);
    if (!con) return out;
    HINTERNET req = WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr))
    {
        DWORD n = 0;
        while (WinHttpQueryDataAvailable(req, &n) && n)
        {
            std::vector<char> b(n); DWORD rd = 0;
            if (!WinHttpReadData(req, b.data(), n, &rd) || !rd) break;
            out.append(b.data(), rd);
        }
    }
    if (req) WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    return out;
}

std::string field(const std::string &obj, const char *name)
{
    const std::string key = std::string("\"") + name + "\"";
    size_t p = obj.find(key); if (p == std::string::npos) return "";
    p = obj.find('"', obj.find(':', p + key.size()) + 1); if (p == std::string::npos) return "";
    const size_t e = obj.find('"', p + 1);
    return e == std::string::npos ? "" : obj.substr(p + 1, e - p - 1);
}

// first normal web page target
std::string find_page(const std::string &list)
{
    size_t p = 0;
    while ((p = list.find('{', p)) != std::string::npos)
    {
        const size_t e = list.find('}', p);
        if (e == std::string::npos) break;
        const std::string obj = list.substr(p, e - p + 1);
        p = e + 1;
        const std::string url = field(obj, "url");
        if (field(obj, "type") == "page" && url.rfind("chrome", 0) != 0 && url.rfind("devtools", 0) != 0 && url.rfind("edge", 0) != 0)
            return field(obj, "webSocketDebuggerUrl");
    }
    return "";
}

void receiver(HINTERNET ws)
{
    std::vector<char> buf(1 << 16); std::string msg;
    while (!g_quit)
    {
        DWORD n = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        if (WinHttpWebSocketReceive(ws, buf.data(), DWORD(buf.size()), &n, &type) != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
        msg.append(buf.data(), n);
        if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) continue;
        char key[32]; snprintf(key, sizeof(key), "\"id\":%d", METRICS_ID);
        if (msg.find(key) != std::string::npos)
        {
            const size_t v = msg.find("\"value\":\"[");
            float d = 0; int w = 0, h = 0;
            if (v != std::string::npos && sscanf(msg.c_str() + v + 10, "%d,%d,%f", &w, &h, &d) == 3) { g_iw = w; g_ih = h; g_dpr = d; }
        }
        msg.clear();
    }
    g_broken = true;
}

bool ws_send(HINTERNET ws, const std::string &s)
{
    return WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void *)s.data(), DWORD(s.size())) == NO_ERROR;
}

void worker()
{
    HINTERNET session = WinHttpOpen(L"dbm_phone", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return;
    WinHttpSetTimeouts(session, 2000, 2000, 2000, 0); // receive without timeout (the page may be quiet for long)
    while (!g_quit)
    {
        // ---- connect
        const std::string url = find_page(http_get(session, g_port, L"/json/list"));
        const size_t slash = url.find('/', 5 + 2); // after "ws://"
        if (url.rfind("ws://", 0) != 0 || slash == std::string::npos)
        { for (int i = 0; i < 10 && !g_quit; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
        HINTERNET con = WinHttpConnect(session, L"127.0.0.1", INTERNET_PORT(g_port.load()), 0);
        HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", widen(url.substr(slash)).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
        HINTERNET ws = nullptr;
        if (req && WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
            WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr))
            ws = WinHttpWebSocketCompleteUpgrade(req, 0);
        if (req) WinHttpCloseHandle(req);
        if (!ws) { if (con) WinHttpCloseHandle(con); std::this_thread::sleep_for(std::chrono::milliseconds(1000)); continue; }

        g_broken = false;
        std::thread rx(receiver, ws);
        char b[1024];
        snprintf(b, sizeof(b), "{\"id\":%d,\"method\":\"Page.addScriptToEvaluateOnNewDocument\",\"params\":{\"source\":%s}}", g_id++, str(KEEP_IN_WINDOW).c_str());
        ws_send(ws, b);
        snprintf(b, sizeof(b), "{\"id\":%d,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":%s}}", g_id++, str(KEEP_IN_WINDOW).c_str());
        ws_send(ws, b);
        g_connected = true;

        // ---- pump the queue, poll the viewport metrics once a second
        auto last_metrics = std::chrono::steady_clock::now() - std::chrono::seconds(2);
        while (!g_quit && !g_broken)
        {
            std::deque<std::string> q;
            {
                std::unique_lock<std::mutex> l(g_mutex);
                g_cv.wait_for(l, std::chrono::milliseconds(250), [] { return !g_queue.empty() || g_quit; });
                q.swap(g_queue);
            }
            bool ok = true;
            for (const std::string &m : q) if (!(ok = ws_send(ws, m))) break;
            if (ok && std::chrono::steady_clock::now() - last_metrics > std::chrono::seconds(1))
            {
                last_metrics = std::chrono::steady_clock::now();
                snprintf(b, sizeof(b), "{\"id\":%d,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"JSON.stringify([innerWidth,innerHeight,devicePixelRatio])\",\"returnByValue\":true}}", METRICS_ID);
                ok = ws_send(ws, b);
            }
            if (!ok) break;
        }
        g_connected = false;
        WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(ws); // unblocks the receiver
        rx.join();
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(session);
}
}

void start(int port)
{
    g_port = port;
    if (g_worker.joinable()) return;
    g_quit = false;
    g_worker = std::thread(worker);
}

void stop()
{
    g_quit = true; g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();
    g_connected = false; g_dpr = 0;
    std::lock_guard<std::mutex> l(g_mutex); g_queue.clear();
}

bool connected() { return g_connected; }

void send(const char *method, const std::string &params_json)
{
    if (!g_connected) return;
    std::lock_guard<std::mutex> l(g_mutex);
    if (g_queue.size() > 256) return; // stalled connection: drop instead of growing
    char head[96]; snprintf(head, sizeof(head), "{\"id\":%d,\"method\":\"", g_id++);
    g_queue.push_back(std::string(head) + method + "\",\"params\":" + params_json + "}");
    g_cv.notify_one();
}

void metrics(float &dpr, int &inner_w, int &inner_h) { dpr = g_dpr; inner_w = g_iw; inner_h = g_ih; }

std::string str(const std::string &s)
{
    std::string o = "\"";
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o + "\"";
}
}
