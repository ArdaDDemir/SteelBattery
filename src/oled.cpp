#include "oled.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace {

constexpr char kGame[] = "STEELBATTERY";
constexpr char kEvent[] = "BATTERY";

std::mutex g_mu;
std::condition_variable g_cv;
std::vector<OledFrame> g_frames;
bool g_enabled = true, g_dirty = false, g_quit = false;
std::thread g_thread;

// "127.0.0.1:51248" from GG's coreProps.json, or "" when GG isn't running.
std::string ReadAddress() {
    char pd[MAX_PATH];
    if (!GetEnvironmentVariableA("PROGRAMDATA", pd, MAX_PATH)) return "";
    for (const char* sub : {"\\SteelSeries\\GG\\coreProps.json", "\\SteelSeries\\SteelSeries Engine 3\\coreProps.json"}) {
        std::ifstream in(std::string(pd) + sub);
        if (!in) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        std::string s = ss.str();
        size_t k = s.find("\"address\"");
        if (k == std::string::npos) continue;
        size_t a = s.find('"', s.find(':', k) + 1);
        size_t b = a == std::string::npos ? a : s.find('"', a + 1);
        if (b != std::string::npos) return s.substr(a + 1, b - a - 1);
    }
    return "";
}

// POSTs JSON; returns the HTTP status (0 on connection failure).
int Post(const std::string& address, const wchar_t* path, const std::string& body) {
    size_t colon = address.rfind(':');
    if (colon == std::string::npos) return 0;
    std::wstring host(address.begin(), address.begin() + colon);
    INTERNET_PORT port = (INTERNET_PORT)atoi(address.c_str() + colon + 1);
    int status = 0;
    // One session for the life of the thread (the charging animation posts every 0.7 s).
    static HINTERNET ses = [] {
        HINTERNET h = WinHttpOpen(L"SteelBattery", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
        if (h) WinHttpSetTimeouts(h, 2000, 2000, 2000, 2000);
        return h;
    }();
    if (!ses) return 0;
    if (HINTERNET con = WinHttpConnect(ses, host.c_str(), port, 0)) {
        if (HINTERNET req = WinHttpOpenRequest(con, L"POST", path, nullptr, nullptr, nullptr, 0)) {
            const wchar_t* hdr = L"Content-Type: application/json\r\n";
            if (WinHttpSendRequest(req, hdr, (DWORD)-1, (void*)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD code = 0, sz = sizeof code;
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz,
                                    nullptr);
                status = (int)code;
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    }
    return status;
}

std::string Bytes(const std::vector<uint8_t>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ',';
        s += std::to_string(v[i]);
    }
    return s + "]";
}

bool Register(const std::string& addr) {
    std::string meta = std::string("{\"game\":\"") + kGame +
                       "\",\"game_display_name\":\"SteelBattery\",\"developer\":\"SteelBattery\","
                       "\"deinitialize_timer_length_ms\":60000}";
    if (Post(addr, L"/game_metadata", meta) != 200) return false;
    std::string bind = std::string("{\"game\":\"") + kGame + "\",\"event\":\"" + kEvent +
                       "\",\"value_optional\":true,\"handlers\":[{\"device-type\":\"screened-128x40\",\"zone\":\"one\","
                       "\"mode\":\"screen\",\"datas\":[{\"has-text\":false,\"image-data\":" +
                       Bytes(std::vector<uint8_t>(640, 0)) + "}]}]}";
    return Post(addr, L"/bind_game_event", bind) == 200;
}

void Run() {
    std::string addr;
    bool registered = false;
    size_t next = 0;  // animation frame index
    std::unique_lock lock(g_mu);
    while (!g_quit) {
        auto wait = g_frames.size() > 1 ? std::chrono::milliseconds(700) : std::chrono::milliseconds(10000);
        g_cv.wait_for(lock, wait, [] { return g_quit || g_dirty; });
        if (g_quit) break;
        if (g_dirty) next = 0;
        g_dirty = false;
        bool enabled = g_enabled;
        OledFrame image = g_frames.empty() ? OledFrame{} : g_frames[next % g_frames.size()];
        ++next;
        lock.unlock();

        if (!enabled) {
            if (registered) Post(addr, L"/remove_game", std::string("{\"game\":\"") + kGame + "\"}");
            registered = false;
        } else if (image.size() == 640) {
            if (!registered) {
                addr = ReadAddress();
                registered = !addr.empty() && Register(addr);
            }
            if (registered) {
                std::string ev = std::string("{\"game\":\"") + kGame + "\",\"event\":\"" + kEvent +
                                 "\",\"data\":{\"frame\":{\"image-data-128x40\":" + Bytes(image) + "}}}";
                if (Post(addr, L"/game_event", ev) != 200) registered = false;  // GG restarted: port may change
            }
        }
        lock.lock();
    }
    lock.unlock();
    if (registered) Post(addr, L"/remove_game", std::string("{\"game\":\"") + kGame + "\"}");
}

}  // namespace

void OledStart() { g_thread = std::thread(Run); }

void OledSet(std::vector<OledFrame> frames) {
    {
        std::lock_guard l(g_mu);
        if (frames == g_frames) return;
        g_frames = std::move(frames);
        g_dirty = true;
    }
    g_cv.notify_one();
}

void OledEnable(bool on) {
    {
        std::lock_guard l(g_mu);
        g_enabled = on;
        g_dirty = true;
    }
    g_cv.notify_one();
}

void OledStop() {
    {
        std::lock_guard l(g_mu);
        g_quit = true;
    }
    g_cv.notify_one();
    if (g_thread.joinable()) g_thread.join();
}
