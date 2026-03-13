# Conditional Breakpoint: Trace Process Creation

Monitor every new process launched in the target system and print its name and PID.
Verified on Windows 10 19041 x64 via VMware kernel debug session.

## Background

`nt!PspInsertProcess` is called once per new process, at the moment the EPROCESS
structure is inserted into the system process list. At this point:

- `RCX` = pointer to the new process's `_EPROCESS`
- `_EPROCESS+0x440` = `UniqueProcessId` (pointer-sized, read with `poi`)
- `_EPROCESS+0x5a8` = `ImageFileName` (15-byte fixed array, may be truncated)

Field offsets vary by Windows version. Always verify before use:

```
dt nt!_EPROCESS 0 ImageFileName UniqueProcessId
```

## Setting the Breakpoint

The breakpoint command reads the two fields directly by offset and auto-continues
with `g` so the system keeps running:

```python
# Use a Python script to avoid PowerShell quoting issues with $ and &
import urllib.request

cmd = r'bp nt!PspInsertProcess ".printf \"[New Process] %ma  PID=%d\n\", @rcx+0x5a8, poi(@rcx+0x440); g"'
req = urllib.request.Request(
    'http://127.0.0.1:{PORT}/api/exec',
    data=cmd.encode(),
    method='POST'
)
print(urllib.request.urlopen(req).read().decode())
```

Or send via curl if the command contains no special shell characters:

```powershell
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/exec -d "bl"   # verify breakpoint was set
```

## Capturing Output

Breakpoint `.printf` output goes to the WinDbg console, **not** through the plugin's
`Execute()` callback. Use `.logopen` to redirect it to a file before resuming:

```powershell
# 1. Open log file
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/exec -d ".logopen C:\trace\proclog.txt"

# 2. Resume - breakpoint fires silently and logs each new process
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/go

# 3. Wait (collect data), then break back in and close the log
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/break
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/exec -d ".logclose"
```

## Sample Output (10-second capture)

```
[New Process] backgroundTask  PID=7716
[New Process] RuntimeBroker.  PID=7900
[New Process] RuntimeBroker.  PID=1632
[New Process] wermgr.exe      PID=7976
[New Process] wermgr.exe      PID=4228
[New Process] wermgr.exe      PID=1672
[New Process] KmdManager.exe  PID=3552
```

> `ImageFileName` is a 15-byte fixed array so names longer than 14 chars are
> truncated (e.g. `RuntimeBroker.` instead of `RuntimeBroker.exe`).
> For the full executable path use `SeAuditProcessCreationInfo.ImageFileName`.

## Cleanup

```powershell
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/exec -d "bc *"   # clear all breakpoints
curl.exe -s -X POST http://127.0.0.1:{PORT}/api/go               # resume target
```

## Notes

- `.printf` in a breakpoint command runs on the WinDbg engine thread; output is
  always written to the WinDbg console and the active log file, never returned
  by the plugin's `/api/exec` response.
- PowerShell mangles `$t0`, `->`, and `&` inside double-quoted strings.
  Use a Python script (shown above) or a `.wds` script file for complex
  breakpoint commands.
- For a live system, `PspInsertProcess` can fire dozens of times per minute.
  Always pair the breakpoint with `g` to avoid halting the OS.
