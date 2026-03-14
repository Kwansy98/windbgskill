---
name: windbgskill
description: Control WinDbg via HTTP REST API for three scenarios - dump analysis (process crash dumps, kernel crash dumps / BSOD), live kernel debugging (KDNET, local kernel, VM kernel), and user-mode process debugging (attach to process, application crash). Use when the user mentions WinDbg, crash dump, .dmp file, BSOD, bugcheck, kernel debug, process debug, !analyze, breakpoints, or says the windbgskill plugin is running on a port number.
---

# WinDbg HTTP Bridge

Controls WinDbg through `windbgskill.dll`, exposing a local HTTP server.
The user loads the plugin once per session; you communicate via HTTP (curl, curl.exe, Python, or any HTTP client).

All three debug modes share the same plugin interface — the only difference is which WinDbg commands you send.

## Quick Start

User loads the plugin in WinDbg once per session:
```
.load windbgskill.dll

# Default: listen on 127.0.0.1:9090 (local access only)
!windbgskill start

# Specify port only (still binds to 127.0.0.1)
!windbgskill start 9090

# Specify IP and port (use 0.0.0.0 to allow remote machine access)
!windbgskill start 0.0.0.0 9090
```

User tells you the port (e.g. "windbgskill plugin is on port 9090"):

```powershell
curl.exe -s http://127.0.0.1:9090/api/status
curl.exe -s -X POST http://127.0.0.1:9090/api/exec -d "!analyze -v"
```

**Always check `/api/status` first. If state is `running`, call `/api/break` before sending any commands.**

## Debugger States

| State | Meaning | Action |
|-------|---------|--------|
| `broken` | Target paused, debugger prompt active | Send commands freely |
| `running` | Target executing | Call `/api/break` first |
| `no_target` | No debug session open | Ask user to open dump or attach |
| `stepping` | Single-step mode | Treat same as `broken` |

State transitions: `running` → **POST /api/break** → `broken` → **POST /api/go** → `running`

## Scenario Detection

If the user doesn't specify which mode, infer from context before asking.

| Signal | Likely Scenario |
|--------|----------------|
| `.dmp` / `.mdmp` file path mentioned | Dump analysis |
| "BSOD", "bugcheck", "blue screen", kernel dump | Kernel dump analysis |
| "attached kernel", "KDNET", "VM debug", "local kernel" | Live kernel debugging |
| "attached process", "process hang", "application crash" | User-mode debugging |
| `!analyze -v` output has `BUGCHECK_CODE` | Kernel dump |
| `!analyze -v` output has `EXCEPTION_CODE` without bugcheck | Process dump or user-mode |
| `!process 0 0` returns process list | Live kernel debugging |

> When the scenario is ambiguous and cannot be inferred from context, ask:
> "Is this dump analysis, live kernel debugging, or user-mode process debugging?"

## API Reference

Base URL: `http://127.0.0.1:{PORT}`

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/help` | GET | API docs (LLM self-discovery) |
| `/api/status` | GET | Debugger state, port, and current command info |
| `/api/exec` | POST | Execute a WinDbg command |
| `/api/break` | POST | Interrupt a running or stuck target |
| `/api/go` | POST | Resume target execution |
| `/api/shutdown` | POST | Stop the HTTP server remotely |

```powershell
# Self-discovery
curl.exe -s http://127.0.0.1:{PORT}/api/help

# Check state - also shows which command is currently running
# {"state":"broken","port":9090,"exec":{"busy":false}}
# {"state":"broken","port":9090,"exec":{"busy":true,"cmd":"!analyze -v","elapsed_s":47}}
curl.exe -s http://127.0.0.1:{PORT}/api/status

# Execute a command (body = raw WinDbg command, response = raw output)
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/exec -d "k"
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/exec -d "!analyze -v"

# Custom timeout via query param (default 60 s)
curl.exe -s -X POST "http://127.0.0.1:{PORT}/api/exec?timeout=180" -d "!analyze -v"

# Interrupt / resume
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/break
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/go

# Stop server remotely
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/shutdown
```

## Common Commands by Scenario

### Dump Analysis (process dump or kernel dump)

| Goal | Command |
|------|---------|
| Automated crash analysis | `!analyze -v` |
| Switch to faulting thread context | `.ecxr` |
| Call stack | `k` / `kb` / `kn` |
| All threads' / processors' stacks | `~*k` |
| Switch to thread/processor N | `~Ns` |
| Registers | `r` |
| Loaded modules | `lm` |
| Reload symbols for a module | `.reload <module>` |
| Force reload all symbols | `.reload /f` |
| Verbose symbol diagnostics | `!sym noisy` then `.reload` |
| Display type / structure | `dt <type> [addr]` |
| Memory layout | `!address` |
| Process environment block | `!peb` |
| Thread environment block | `!teb` |
| Heap overview | `!heap -s` |

> **Symbol issues**: if output shows `??? symbols` or `symbol file could not be found`, run `.reload <module>` or `.reload /f`. Use `!sym noisy` before `.reload` to diagnose why symbols fail to load.

### Live Kernel Debugging

> **`~N` in kernel context = switch to processor/core N** (not thread). Use `!process` / `!thread` to navigate threads.

| Goal | Command |
|------|---------|
| List all processes | `!process 0 0` |
| Processes with thread details | `!process 0 7` |
| Inspect specific process | `!process <addr> 7` |
| Switch to processor/core N | `~Ns` |
| Current thread info | `!thread` |
| Driver object | `!drvobj <name> 7` |
| Device object | `!devobj <addr>` |
| Pool tag analysis | `!pool <addr>` / `!poolused` |
| Page table entry | `!pte <addr>` |
| Display EPROCESS | `dt nt!_EPROCESS <addr>` |
| List drivers / modules | `lm` |
| Set breakpoint on kernel symbol | `bp nt!NtCreateFile` |
| Virtual memory stats | `!vm` |
| Reload driver symbols | `.reload <driver.sys>` |

### User-mode Process Debugging

> **`~N` in user-mode context = switch to thread N** (e.g. `~0s`, `~1s`).

| Goal | Command |
|------|---------|
| List threads | `~` |
| All threads' call stacks | `~*k` |
| Switch to thread N | `~Ns` |
| Call stack | `k` / `kb` |
| Display local variables | `dv` |
| Search symbols | `x <module>!<pattern>` |
| Handle table | `!handle` |
| Heap overview | `!heap -s` |
| Process environment block | `!peb` |
| Set breakpoint | `bp <module>!<function>` |
| List breakpoints | `bl` |

## Workflows

### Crash dump analysis

```powershell
# 1. Confirm state (dump loads in broken state)
curl.exe -s http://127.0.0.1:{PORT}/api/status

# 2. Automated analysis (can take 60-120 s if downloading symbols)
curl.exe -s -X POST "http://127.0.0.1:{PORT}/api/exec?timeout=180" -d "!analyze -v"

# 3. Switch to faulting context and inspect stack
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/exec -d ".ecxr"
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/exec -d "k"
```

### Live kernel debugging

```powershell
# 1. Break in
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/break

# 2. Explore system state
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/exec -d "!process 0 0"

# 3. Set breakpoint and resume
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/exec -d "bp nt!NtCreateFile"
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/go

# 4. When breakpoint hits, inspect
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/exec -d "k"
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/exec -d "r"
```

### User-mode process debugging

```powershell
# 1. Break in
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/break

# 2. Overview
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/exec -d "~*k"
curl.exe -s -X POST "http://127.0.0.1:{PORT}/api/exec?timeout=120" -d "!analyze -v"

# 3. Set breakpoint and resume
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/exec -d "bp MyModule!MyFunction"
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/go
```

## Rules

- Use any available HTTP client (curl, curl.exe, Python requests, etc.).
- `/api/exec` body is plain text — send the WinDbg command directly, no JSON wrapping.
- Slow commands (`!analyze -v`, `.reload /f`, symbol downloads): append `?timeout=180`; default is 60 s.
- `/api/exec` returns HTTP 409 if target is running — call `/api/break` first.
- `/api/exec` returns HTTP 503 if the engine is stuck — diagnose and recover:
  1. `GET /api/status` → `exec.busy`, `exec.cmd`, `exec.elapsed_s` show what is blocking and for how long.
  2. `POST /api/break` → sends a passive interrupt to unblock the stuck command.
  3. Retry the original command or investigate the cause.
- After resume commands (`g`, `p`, `t`, `pt`, `tt`), state becomes `running`; check `/api/status` before next command.
- `~N` means **thread N** in user-mode, **processor/core N** in kernel mode and kernel dumps.
- If the debug scenario is unclear, infer from context first; ask the user only if inference is not possible.
