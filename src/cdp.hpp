// Minimal Chrome DevTools Protocol client (WinHTTP WebSocket): sends input to the browser page shown on the
// truck screen. Chromium ignores posted window messages, but CDP input works even when the window is unfocused
// or hidden behind the game.
#pragma once
#include <string>

namespace cdp
{
void start(int port);          // connects to the first normal page on 127.0.0.1:port, reconnects in the background
void stop();
bool connected();
void send(const char *method, const std::string &params_json = "{}");
// page viewport in CSS pixels and device pixel ratio (0 until known)
void metrics(float &dpr, int &inner_w, int &inner_h);
std::string str(const std::string &s); // JSON string literal
}
