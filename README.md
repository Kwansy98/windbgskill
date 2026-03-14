# windbgskill

**WinDbg HTTP Bridge** — expose WinDbg as a local REST API so AI assistants (Cursor / Claude) can programmatically drive crash dump analysis, live kernel debugging, and user-mode process debugging.

[中文文档](README_CN.md)

---

## What is this?

`windbgskill` is a WinDbg extension DLL that starts a lightweight HTTP server inside WinDbg. Once loaded, an AI assistant can send WinDbg commands, read their output, and control execution flow — all without touching the WinDbg UI.

Supported scenarios:

| Scenario | Examples |
|----------|---------|
| **Crash dump analysis** | Process crash dumps (`.dmp`, `.mdmp`), kernel BSOD dumps |
| **Live kernel debugging** | KDNET, serial, EXDI, local kernel |
| **User-mode debugging** | Attach to process, application hang / crash |

## Quick Start

### 1. Get the DLL

Pre-built binaries are on the Releases page (`windbgskill64.dll` for x64, `windbgskill.dll` for x86). To build from source, open `windbgskill/windbgskill.sln` in Visual Studio 2019+ and build Release x64.

### 2. Load the plugin in WinDbg

```
.load C:\path\to\windbgskill.dll

# Default: listen on 127.0.0.1:9090 (local access only)
!windbgskill start

# Specify port only (still binds to 127.0.0.1)
!windbgskill start 9090

# Specify IP and port (use 0.0.0.0 to allow access from another machine)
!windbgskill start 0.0.0.0 9090
```

```
!windbgskill status   ; check whether the server is running
!windbgskill stop     ; shut down the HTTP server
```

### 3. Tell your AI assistant the port

> "windbgskill plugin is running on port 9090"

The AI will take it from there. Make sure `curl.exe` is in your `PATH`.

## Deploy the AI Skill

This repo ships a Cursor Agent Skill at `skills/windbg/SKILL.md` that teaches the AI how to use the plugin (state machine, timeouts, scenario detection, etc.).

### Cursor

Copy `skills/windbg/SKILL.md` into your project at:

```
your-project/
└── .cursor/
    └── skills/
        └── windbg/
            └── SKILL.md
```

### Claude (claude.ai Projects / Claude Desktop)

Paste the contents of `skills/windbg/SKILL.md` into your Project instructions or custom instructions.

---

See [`skills/windbg/examples/`](skills/windbg/examples/) for real-world debugging sessions.

## License

MIT
