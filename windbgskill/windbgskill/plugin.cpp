// plugin.cpp - WinDbg HTTP Bridge Extension
//
// Exposes a local HTTP REST API so AI tools (Cursor, etc.) can control
// WinDbg via curl regardless of debug mode (kernel / user / dump analysis).
//
// WinDbg usage:
//   .load C:\path\to\windbgskill.dll
//   !windbgskill start 9090      <- start HTTP server on port 9090
//   !windbgskill stop
//   !windbgskill status
//
// AI communicates via curl:
//   curl http://127.0.0.1:9090/api/status
//   curl -X POST http://127.0.0.1:9090/api/exec ^
//        -d "{\"cmd\":\"!analyze -v\"}"
//   curl -X POST http://127.0.0.1:9090/api/break
//   curl -X POST http://127.0.0.1:9090/api/go
//
// httplib.h (cpp-httplib, single header) must be placed next to this file.
// Download: https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h

#include "pch.h"

// httplib must be included AFTER windows.h (already pulled in by pch.h)
#pragma warning(push)
#pragma warning(disable: 4267 4244 4100)  // suppress httplib size_t narrowing warnings
#include "httplib.h"
#pragma warning(pop)

#pragma comment(lib, "ws2_32.lib")

// ---------------------------------------------------------------
// IDebugOutputCallbacks - captures kd command output to a string
// ---------------------------------------------------------------

class OutputCapture : public IDebugOutputCallbacks
{
public:
    std::string text;

    STDMETHOD_(ULONG, AddRef)()  override { return 1; }
    STDMETHOD_(ULONG, Release)() override { return 1; }

    STDMETHOD(QueryInterface)(REFIID riid, PVOID* ppv) override
    {
        if (riid == __uuidof(IDebugOutputCallbacks) || riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IDebugOutputCallbacks*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHOD(Output)(ULONG /*Mask*/, PCSTR Text) override
    {
        if (Text) text += Text;
        return S_OK;
    }

    void Clear() { text.clear(); }
};

// ---------------------------------------------------------------
// Global state
// ---------------------------------------------------------------

// g_WinDbgClient  : the IDebugClient handed to us by WinDbg on !windbgskill start.
//                   We hold a reference to prevent it from being freed.
// g_ExecClient    : a sibling client (CreateClient) used exclusively by the
//                   HTTP server background thread for Execute() calls.
//                   Keeping a dedicated client avoids output callback races.
// g_Control       : IDebugControl4 QI'd from g_ExecClient.

static IDebugClient*   g_WinDbgClient = nullptr;
static IDebugClient*   g_ExecClient   = nullptr;
static IDebugControl4* g_Control      = nullptr;

// Serialises all calls into the debug engine from the HTTP thread.
// timed_mutex allows try_lock_for() so that a stuck Execute() never
// causes the HTTP thread to block forever.
static std::timed_mutex g_EngineMutex;

// Tracks the currently running command so /api/status can report it.
struct ExecInfo {
    std::atomic<bool>                    busy{ false };
    std::string                          cmd;
    std::chrono::steady_clock::time_point started;
    std::mutex                           mtx;  // protects cmd / started
};
static ExecInfo g_ExecInfo;

static std::unique_ptr<httplib::Server> g_HttpServer;
static std::thread                       g_HttpThread;
static std::atomic<int>                  g_HttpPort{ 0 };

// ---------------------------------------------------------------
// Minimal JSON helpers (no external dependency)
// ---------------------------------------------------------------

static std::string JsonEscape(const std::string& s)
{
    std::string r;
    r.reserve(s.size() + 2);
    r += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':  r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        case '\n': r += "\\n";  break;
        case '\r': r += "\\r";  break;
        case '\t': r += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                r += buf;
            }
            else {
                r += static_cast<char>(c);
            }
        }
    }
    r += '"';
    return r;
}


// ---------------------------------------------------------------
// Debug engine helpers
// ---------------------------------------------------------------

// Returns a short string describing the current execution state.
// Safe to call from the HTTP thread (read-only, no lock needed).
static std::string GetStatus()
{
    if (!g_Control) return "no_debugger";

    ULONG status = 0;
    if (FAILED(g_Control->GetExecutionStatus(&status))) return "unknown";

    switch (status) {
    case DEBUG_STATUS_BREAK:        return "broken";
    case DEBUG_STATUS_GO:           return "running";
    case DEBUG_STATUS_NO_DEBUGGEE:  return "no_target";
    case DEBUG_STATUS_STEP_INTO:
    case DEBUG_STATUS_STEP_OVER:
    case DEBUG_STATUS_STEP_BRANCH:  return "stepping";
    default:                        return "unknown";
    }
}

// Execute a WinDbg/kd command and return captured output.
// timeout_secs: if Execute() has not returned by this deadline, a passive
// interrupt is sent to unblock it. SetInterrupt is safe to call from any
// thread without the engine mutex, which is why the watchdog can do it
// while the main thread holds g_EngineMutex inside Execute().
static std::string ExecCommand(const std::string& cmd, int timeout_secs = 60)
{
    if (!g_ExecClient || !g_Control)
        return "[error] debugger not initialised - run !windbgskill start first";

    // Acquire lock with a deadline of (timeout + 5) seconds.
    // If the previous Execute() is still stuck after the watchdog fired,
    // we fail fast instead of blocking the HTTP thread forever.
    if (!g_EngineMutex.try_lock_for(std::chrono::seconds(timeout_secs + 5)))
        return "[error] engine busy - previous command may be stuck; try POST /api/break";

    std::lock_guard<std::timed_mutex> lock(g_EngineMutex, std::adopt_lock);

    {
        std::lock_guard<std::mutex> li(g_ExecInfo.mtx);
        g_ExecInfo.cmd     = cmd;
        g_ExecInfo.started = std::chrono::steady_clock::now();
        g_ExecInfo.busy.store(true);
    }

    OutputCapture cap;
    IDebugOutputCallbacks* oldCb = nullptr;
    g_ExecClient->GetOutputCallbacks(&oldCb);
    g_ExecClient->SetOutputCallbacks(&cap);

    // Watchdog thread: fires a passive interrupt if Execute() exceeds timeout.
    std::atomic<bool> execDone{ false };
    std::thread watchdog([&execDone, timeout_secs]()
    {
        const int ticks = timeout_secs * 10;
        for (int i = 0; i < ticks; ++i) {
            Sleep(100);
            if (execDone.load()) return;
        }
        if (!execDone.load() && g_Control)
            g_Control->SetInterrupt(DEBUG_INTERRUPT_PASSIVE);
    });

    const HRESULT hr = g_Control->Execute(
        DEBUG_OUTCTL_THIS_CLIENT,
        cmd.c_str(),
        DEBUG_EXECUTE_DEFAULT);

    execDone.store(true);
    watchdog.join();

    g_ExecClient->SetOutputCallbacks(oldCb);
    g_ExecInfo.busy.store(false);

    if (FAILED(hr) && cap.text.empty()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "[error] Execute failed: 0x%08X", hr);
        return buf;
    }
    return cap.text;
}

// ---------------------------------------------------------------
// HTTP route definitions
// ---------------------------------------------------------------

static void SetupRoutes(httplib::Server& svr)
{
    // ------------------------------------------------------------------
    // GET /api/help
    // Returns API documentation as JSON. Intended for LLM self-discovery:
    // an AI that connects to an unknown instance can GET /api/help to learn
    // all available endpoints without consulting the SKILL.md.
    // ------------------------------------------------------------------
    svr.Get("/api/help", [](const httplib::Request&, httplib::Response& res)
    {
        const char* body = R"({
  "plugin": "windbgskill",
  "endpoints": [
    {"method":"GET",  "path":"/api/help",     "description":"This help document (JSON)."},
    {"method":"GET",  "path":"/api/status",   "description":"JSON: {state, port, exec}. exec.busy=true shows which command is running and for how long (elapsed_s). Use this to diagnose hangs."},
    {"method":"POST", "path":"/api/exec",     "description":"Execute a WinDbg command. Body: raw command text. Optional ?timeout=60 (seconds, default 60). Returns plain text output. HTTP 409: not broken. HTTP 503: engine busy."},
    {"method":"POST", "path":"/api/break",    "description":"Interrupt a running or stuck target. Returns plain text state."},
    {"method":"POST", "path":"/api/go",       "description":"Resume target execution. Returns plain text 'running'."},
    {"method":"POST", "path":"/api/shutdown", "description":"Stop the HTTP server remotely."}
  ],
  "notes": [
    "Check GET /api/status before POST /api/exec.",
    "If /api/exec returns 503 (engine busy), call GET /api/status to see which command is stuck, then POST /api/break to interrupt it.",
    "~N = thread N in user-mode, processor/core N in kernel mode.",
    "!analyze -v and .reload /f can take 60-120 s - pass ?timeout=180 if needed.",
    "Resume commands (g, p, t) set state to running; check /api/status before the next exec."
  ]
})";
        res.set_content(body, "application/json");
    });

    // ------------------------------------------------------------------
    // GET /api/status
    // Returns current debugger execution state and listening port.
    //
    // Example response:
    //   {"state":"broken","port":9090}
    // ------------------------------------------------------------------
    svr.Get("/api/status", [](const httplib::Request&, httplib::Response& res)
    {
        const std::string state = GetStatus();
        std::string body =
            "{\"state\":" + JsonEscape(state) +
            ",\"port\":"  + std::to_string(g_HttpPort.load());

        if (g_ExecInfo.busy.load()) {
            std::lock_guard<std::mutex> li(g_ExecInfo.mtx);
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - g_ExecInfo.started).count();
            body += ",\"exec\":{\"busy\":true"
                    ",\"cmd\":"       + JsonEscape(g_ExecInfo.cmd) +
                    ",\"elapsed_s\":" + std::to_string(elapsed) + "}";
        } else {
            body += ",\"exec\":{\"busy\":false}";
        }

        body += "}";
        res.set_content(body, "application/json");
    });

    // ------------------------------------------------------------------
    // POST /api/exec
    // Execute a WinDbg command. Target must be in "broken" state.
    //
    // Body: raw command text
    //   k
    //   !analyze -v
    //
    // Optional timeout (seconds, default 60) via query param:
    //   POST /api/exec?timeout=180
    //
    // Response: raw WinDbg output (text/plain)
    // HTTP 409: target is running - call POST /api/break first
    // HTTP 503: engine busy (previous command stuck) - call POST /api/break
    // ------------------------------------------------------------------
    svr.Post("/api/exec", [](const httplib::Request& req, httplib::Response& res)
    {
        // Trim trailing whitespace / CRLF from the raw body
        std::string cmd = req.body;
        while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == '\n' || cmd.back() == ' '))
            cmd.pop_back();

        if (cmd.empty()) {
            res.status = 400;
            res.set_content("error: empty command", "text/plain");
            return;
        }

        const std::string state = GetStatus();
        if (state != "broken" && state != "stepping") {
            res.status = 409;
            res.set_content(
                "error: target not broken (state=" + state + ") - call POST /api/break first",
                "text/plain");
            return;
        }

        int timeout_secs = 60;
        auto it = req.params.find("timeout");
        if (it != req.params.end()) {
            try { timeout_secs = std::stoi(it->second); } catch (...) {}
        }

        const std::string output = ExecCommand(cmd, timeout_secs);
        if (output.rfind("[error] engine busy", 0) == 0)
            res.status = 503;
        res.set_content(output, "text/plain");
    });

    // ------------------------------------------------------------------
    // POST /api/break
    // Interrupt a running target (equivalent to Ctrl+Break in WinDbg).
    // No-op if the target is already broken.
    //
    // Response: plain text current state ("broken" / "running" / ...)
    // ------------------------------------------------------------------
    svr.Post("/api/break", [](const httplib::Request&, httplib::Response& res)
    {
        if (!g_Control) {
            res.status = 500;
            res.set_content("error: debugger not initialised", "text/plain");
            return;
        }

        // SetInterrupt is designed to be called from any thread and does NOT
        // require g_EngineMutex. Holding the mutex here would deadlock when
        // a long Execute() is in progress - exactly the situation we need to
        // interrupt.
        g_Control->SetInterrupt(DEBUG_INTERRUPT_ACTIVE);

        // Poll up to 5 s for the break to land
        for (int i = 0; i < 50; ++i) {
            Sleep(100);
            if (GetStatus() == "broken") break;
        }

        res.set_content(GetStatus(), "text/plain");
    });

    // ------------------------------------------------------------------
    // POST /api/go
    // Resume target execution (equivalent to 'g' in WinDbg).
    //
    // Example response:
    //   {"state":"running"}
    // ------------------------------------------------------------------
    svr.Post("/api/go", [](const httplib::Request&, httplib::Response& res)
    {
        ExecCommand("g");
        res.set_content("running", "text/plain");
    });

    // ------------------------------------------------------------------
    // POST /api/shutdown
    // Stop the HTTP server remotely. Equivalent to typing !windbgskill stop in
    // the WinDbg console. Useful when the AI needs to clean up without
    // physical access to the WinDbg prompt.
    // ------------------------------------------------------------------
    svr.Post("/api/shutdown", [](const httplib::Request&, httplib::Response& res)
    {
        res.set_content("shutting down", "text/plain");
        // Stop on a background thread so this response is sent before the
        // server socket closes.
        std::thread([]()
        {
            Sleep(200);
            if (g_HttpServer) {
                g_HttpServer->stop();
                g_HttpServer.reset();
                g_HttpPort.store(0);
            }
        }).detach();
    });
}

// ---------------------------------------------------------------
// !windbgskill command handler
//
//   !windbgskill start [port]   Start HTTP server (default port: 9090)
//   !windbgskill stop           Stop HTTP server
//   !windbgskill status         Print server and debugger status
// ---------------------------------------------------------------

extern "C" __declspec(dllexport)
HRESULT CALLBACK windbgskill(PDEBUG_CLIENT Client, PCSTR Args)
{
    if (!Client) return E_INVALIDARG;

    // Acquire IDebugControl4 from the WinDbg-provided client so we can
    // write output back to the WinDbg console.
    IDebugControl4* ctrl = nullptr;
    Client->QueryInterface(__uuidof(IDebugControl4), reinterpret_cast<PVOID*>(&ctrl));

    auto Print = [&](const char* fmt, ...)
    {
        if (!ctrl) return;
        char buf[2048];
        va_list va;
        va_start(va, fmt);
        vsnprintf(buf, sizeof(buf), fmt, va);
        va_end(va);
        ctrl->Output(DEBUG_OUTPUT_NORMAL, "%s", buf);
    };

    // Parse sub-command and optional argument
    std::string args(Args ? Args : "");
    while (!args.empty() && args.front() == ' ') args.erase(args.begin());
    const auto spacePos = args.find(' ');
    const std::string subcmd = args.substr(0, spacePos);
    std::string rest = (spacePos != std::string::npos) ? args.substr(spacePos + 1) : "";
    while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());

    // ---- start --------------------------------------------------------
    if (subcmd == "start")
    {
        if (g_HttpServer && g_HttpServer->is_running()) {
            Print("[windbgskill] Already running on port %d\n", g_HttpPort.load());
            if (ctrl) ctrl->Release();
            return S_OK;
        }

        int port = 9090;
        if (!rest.empty()) {
            try { port = std::stoi(rest); }
            catch (...) {
                Print("[windbgskill] Invalid port '%s', using 9090\n", rest.c_str());
            }
        }

        // Release previously stored interfaces (e.g. after a stop + restart)
        if (g_Control)      { g_Control->Release();      g_Control      = nullptr; }
        if (g_ExecClient)   { g_ExecClient->Release();   g_ExecClient   = nullptr; }
        if (g_WinDbgClient) { g_WinDbgClient->Release(); g_WinDbgClient = nullptr; }

        Client->AddRef();
        g_WinDbgClient = Client;

        // CreateClient() creates a sibling client that shares the same
        // debug session. We use it exclusively from the HTTP thread so
        // its output callbacks never interfere with WinDbg's own output.
        HRESULT hr = Client->CreateClient(&g_ExecClient);
        if (FAILED(hr)) {
            Print("[windbgskill] CreateClient failed: 0x%08X\n", hr);
            if (ctrl) ctrl->Release();
            return hr;
        }

        hr = g_ExecClient->QueryInterface(
            __uuidof(IDebugControl4), reinterpret_cast<PVOID*>(&g_Control));
        if (FAILED(hr)) {
            Print("[windbgskill] QueryInterface(IDebugControl4) failed: 0x%08X\n", hr);
            if (ctrl) ctrl->Release();
            return hr;
        }

        g_HttpServer = std::make_unique<httplib::Server>();
        SetupRoutes(*g_HttpServer);
        g_HttpPort.store(port);

        g_HttpThread = std::thread([port]()
        {
            g_HttpServer->listen("127.0.0.1", port);
        });
        g_HttpThread.detach();

        Sleep(200);  // give the server time to bind the socket

        Print("[windbgskill] HTTP server started on http://127.0.0.1:%d\n", port);

        if (ctrl) ctrl->Release();
        return S_OK;
    }

    // ---- stop ---------------------------------------------------------
    if (subcmd == "stop")
    {
        if (g_HttpServer) {
            g_HttpServer->stop();
            g_HttpServer.reset();
            g_HttpPort.store(0);
            Print("[windbgskill] Server stopped\n");
        }
        else {
            Print("[windbgskill] Server is not running\n");
        }
        if (ctrl) ctrl->Release();
        return S_OK;
    }

    // ---- status (or bare !windbgskill) ----------------------------------------
    if (subcmd == "status" || subcmd.empty())
    {
        if (g_HttpServer && g_HttpServer->is_running()) {
            Print("[windbgskill] Running on http://127.0.0.1:%d\n", g_HttpPort.load());
            Print("[windbgskill] Debugger state: %s\n", GetStatus().c_str());
        }
        else {
            Print("[windbgskill] Not running\n");
        }
        if (ctrl) ctrl->Release();
        return S_OK;
    }

    // Unknown sub-command -> print help
    Print("Usage:\n");
    Print("  !windbgskill start [port]   Start HTTP server (default: 9090)\n");
    Print("  !windbgskill stop           Stop HTTP server\n");
    Print("  !windbgskill status         Show server and debugger state\n");
    if (ctrl) ctrl->Release();
    return S_OK;
}

// ---------------------------------------------------------------
// Extension lifecycle
// ---------------------------------------------------------------

extern "C" __declspec(dllexport)
HRESULT CALLBACK DebugExtensionInitialize(PULONG Version, PULONG Flags)
{
    *Version = DEBUG_EXTENSION_VERSION(1, 0);
    *Flags   = 0;
    return S_OK;
}

extern "C" __declspec(dllexport)
void CALLBACK DebugExtensionUninitialize()
{
    if (g_HttpServer) {
        g_HttpServer->stop();
        g_HttpServer.reset();
    }
    if (g_Control)      { g_Control->Release();      g_Control      = nullptr; }
    if (g_ExecClient)   { g_ExecClient->Release();   g_ExecClient   = nullptr; }
    if (g_WinDbgClient) { g_WinDbgClient->Release(); g_WinDbgClient = nullptr; }
}
