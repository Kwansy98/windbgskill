# windbgskill

**WinDbg HTTP Bridge** — expose WinDbg as a local REST API so AI assistants (Cursor / Claude) can programmatically drive crash dump analysis, live kernel debugging, and user-mode process debugging.

[中文文档](README_CN.md)

---

## What is this?

`windbgskill` is a WinDbg extension DLL that starts a lightweight HTTP server inside WinDbg. Once loaded, any HTTP client — including an AI coding assistant — can send WinDbg commands, read their output, control execution flow, and react to results, all without touching the WinDbg UI.

This turns WinDbg from an interactive tool into a **programmable debugging service**, enabling workflows like:

- AI-guided crash dump triage (`!analyze -v` → stack walk → root cause summary)
- Automated kernel debugging sessions (set breakpoints, resume, capture output)
- Scripted user-mode debugging (inspect threads, heap, handles on demand)

## Architecture

```
┌─────────────────────────────────────────┐
│  WinDbg (windbg.exe / kd.exe / cdb.exe) │
│                                          │
│  .load windbgskill.dll                   │
│  !windbgskill start 9090                 │
│                                          │
│  ┌──────────────────────────────────┐    │
│  │  HTTP Server (127.0.0.1:9090)    │    │
│  │  /api/exec   /api/status         │    │
│  │  /api/break  /api/go             │    │
│  └──────────────────────────────────┘    │
└──────────────┬──────────────────────────┘
               │ HTTP
   ┌───────────▼───────────┐
   │  AI Assistant         │
   │  (Cursor / Claude /   │
   │   any HTTP client)    │
   └───────────────────────┘
```

## Supported Scenarios

| Scenario | Examples |
|----------|---------|
| **Crash dump analysis** | Process crash dumps (`.dmp`, `.mdmp`), kernel BSOD dumps |
| **Live kernel debugging** | KDNET, local kernel debugging, VM kernel debugging (VirtualBox, Hyper-V, EXDI) |
| **User-mode debugging** | Attach to process, application hang / crash |

## Quick Start

### 1. Get the DLL

**Option A — Pre-built binaries (no build required)**

Download the latest release from the [Releases](../../releases) page.

| Architecture | File |
|---|---|
| x64 (recommended) | `windbgskill64.dll` |
| x86 | `windbgskill.dll` |

Use the architecture that matches your WinDbg installation.

**Option B — Build from source**

Open `windbgskill/windbgskill.sln` in Visual Studio 2019 or later and build in **Release x64** (or Win32 if your WinDbg is 32-bit).

Dependencies already included:
- [`cpp-httplib`](https://github.com/yhirose/cpp-httplib) — single-header HTTP library (bundled as `httplib.h`)
- `dbgeng.lib` — from the Windows SDK / WinDbg SDK

### 2. Load the plugin in WinDbg

```
.load C:\path\to\windbgskill.dll
!windbgskill start 9090
```

Optional commands:

```
!windbgskill status   ; check whether the server is running
!windbgskill stop     ; shut down the HTTP server
```

### 3. Talk to it over HTTP

```powershell
# Check debugger state
curl.exe -s http://127.0.0.1:9090/api/status

# Run a command and get its output
curl.exe -s -X POST http://127.0.0.1:9090/api/exec -d "k"

# Run !analyze -v with a longer timeout (symbol downloads can be slow)
curl.exe -s -X POST "http://127.0.0.1:9090/api/exec?timeout=180" -d "!analyze -v"

# Break into a running target
curl.exe -s -X POST http://127.0.0.1:9090/api/break

# Resume execution
curl.exe -s -X POST http://127.0.0.1:9090/api/go
```

## API Reference

Base URL: `http://127.0.0.1:{PORT}`

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/help` | GET | Self-describing API docs (JSON) — useful for LLM self-discovery |
| `/api/status` | GET | Debugger state, port, and current-command info |
| `/api/exec` | POST | Execute a WinDbg command; body = raw command text |
| `/api/break` | POST | Interrupt a running or stuck target |
| `/api/go` | POST | Resume target execution |
| `/api/shutdown` | POST | Stop the HTTP server remotely |

### `/api/status` response

```json
{ "state": "broken", "port": 9090, "exec": { "busy": false } }
{ "state": "broken", "port": 9090, "exec": { "busy": true, "cmd": "!analyze -v", "elapsed_s": 47 } }
```

### Debugger states

| State | Meaning | Required action before `/api/exec` |
|-------|---------|-------------------------------------|
| `broken` | Target paused, prompt active | None — send commands freely |
| `running` | Target executing | Call `/api/break` first |
| `no_target` | No session open | Ask user to open dump or attach |
| `stepping` | Single-step mode | Treat same as `broken` |

### HTTP error codes

| Code | Meaning |
|------|---------|
| 409 | Target is running — call `/api/break` first |
| 503 | Engine busy — check `exec.cmd` / `exec.elapsed_s` in `/api/status`, then call `/api/break` to unblock |

### Timeout parameter

```powershell
# Default timeout is 60 s; increase for slow commands
curl.exe -s -X POST "http://127.0.0.1:9090/api/exec?timeout=180" -d "!analyze -v"
```

## Using with Cursor (AI-assisted debugging)

This repo ships a **Cursor Agent Skill** at `skills/windbg/SKILL.md`. Install it as a project skill and Cursor will automatically know how to:

- Detect the debugging scenario (dump analysis / kernel / user-mode)
- Check state before sending commands
- Handle `running` → `break` → `broken` transitions
- Use appropriate timeouts for slow commands
- Recover from 503 / stuck-engine situations

See [`skills/windbg/examples/`](skills/windbg/examples/) for real-world debugging sessions documented as worked examples.

## Common WinDbg Commands

### Dump analysis

| Goal | Command |
|------|---------|
| Automated crash analysis | `!analyze -v` |
| Switch to faulting thread context | `.ecxr` |
| Call stack | `k` / `kb` / `kn` |
| All threads' stacks | `~*k` |
| Registers | `r` |
| Loaded modules | `lm` |
| Reload symbols | `.reload <module>` |
| Display structure | `dt <type> [addr]` |
| Heap overview | `!heap -s` |

### Live kernel debugging

| Goal | Command |
|------|---------|
| List all processes | `!process 0 0` |
| Processes with thread details | `!process 0 7` |
| Current thread info | `!thread` |
| Driver object | `!drvobj <name> 7` |
| Pool tag analysis | `!pool <addr>` |
| Set kernel breakpoint | `bp nt!NtCreateFile` |
| Virtual memory stats | `!vm` |

### User-mode debugging

| Goal | Command |
|------|---------|
| List threads | `~` |
| All threads' stacks | `~*k` |
| Switch to thread N | `~Ns` |
| Display locals | `dv` |
| Search symbols | `x <module>!<pattern>` |
| Handle table | `!handle` |
| Set breakpoint | `bp <module>!<function>` |

> **Note:** `~N` means **thread N** in user-mode, **processor/core N** in kernel mode and kernel dumps.

## Implementation Notes

- Uses a dedicated `IDebugClient` (`g_ExecClient`) for the HTTP thread to avoid output-callback conflicts with the WinDbg UI client.
- `OutputCapture` implements `IDebugOutputCallbacks` to redirect command output into a `std::string`.
- A `std::timed_mutex` (`g_EngineMutex`) serializes all engine calls.
- A watchdog thread monitors running commands; if a command exceeds its timeout, it sends `DEBUG_INTERRUPT_PASSIVE` to unblock the engine.
- The server binds to `127.0.0.1` only — no external network exposure.

## License

MIT
