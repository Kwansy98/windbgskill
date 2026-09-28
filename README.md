# windbgskill

[中文版本](READMD_CN.md)

The windbg skill gives AI the ability to execute WinDbg commands. Typical use cases include kernel debugging, user-mode process debugging, and crash dump analysis.

## Installation

The skill file in this repository is located at `skills/windbg/SKILL.md`. Place it in the matching directory for your agent tool:


| Tool               | Path                               |
| ------------------ | ---------------------------------- |
| Cursor             | `.cursor/skills/windbg/SKILL.md`   |
| Claude Code        | `.claude/skills/windbg/SKILL.md`   |
| Codex              | `.codex/skills/windbg/SKILL.md`    |
| OpenCode           | `.opencode/skills/windbg/SKILL.md` |
| Other agents tools | `.agents/skills/windbg/SKILL.md`   |


Make sure `curl.exe` is available in `PATH`, otherwise you need to tell the AI to use another equivalent way to access the HTTP endpoint.

## Usage

### Crash dump analysis

Open the dump and initialize the HTTP bridge in one command:

```powershell
WinDbgX.exe -z "pathtodmp" -c ".load D:\All\project\mycode\windbgskill\windbgskill\x64\Release\windbgskill.dll; !windbgskill start 127.0.0.1 23333"
```

Replace `pathtodmp` with the dump's absolute path. `.load` must receive the absolute path to `windbgskill.dll`; passing only the filename depends on WinDbg's extension lookup and can fail with `0x80070002`.

### Existing debug session

For an already-open live kernel or user-mode session, run the following commands in WinDbg:

```text
.load D:\All\project\mycode\windbgskill\windbgskill\x64\Release\windbgskill.dll
!windbgskill start 6655
```

Expected output:

```text
[windbgskill] HTTP server started on http://127.0.0.1:6655
```

To allow connections from another host, specify the IP explicitly:

```text
!windbgskill start 192.168.1.10 6655
```

Then tell the agent directly:

```text
windbg skill is ready on port 6655, please use windbg skill to analyze this dump.
```

Or:

```text
windbg skill is ready on port 6655, please use windbg skill to debug this kernel.
```

## Examples

![](vscodeimages/2026-04-14-20-49-13.png)

![](vscodeimages/2026-04-14-20-51-34.png)
