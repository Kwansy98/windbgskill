# EXDI Debugging: Exception Stack Switch in KiPageFault → KiExceptionDispatch

Verified on Windows 10 19041 x64 via EXDI KD (QEMU-style hardware debug interface).

## Environment

```
eXDI KD
Windows 10 Kernel Version 19041 MP (2 procs) Free x64
```

EXDI limitations observed in this session:
- `Unable to read KTHREAD address` printed on every command — extension commands
  that rely on current thread context (`!process`, `!thread`, `~`) are unavailable.
- Software breakpoints (`bp`) **do work** — the EXDI implementation supports
  memory writes for int3 injection.
- Single-step (`p`, `t`) works normally.

---

## The Mechanism: Windows Exception Stack

On Windows 10 (high build), when an exception is dispatched, the kernel may switch
RSP to a **per-CPU dedicated exception stack** before calling `KiDispatchException`.
This prevents an exception handler from corrupting the faulting thread's kernel stack.

The exception stack pointer lives at:

```
_KPCR.Prcb.ExceptionStack   (KPRCB offset +0x7F28)
_KPCR.Prcb.ExceptionStackActive (KPRCB offset +0x7F26, flag)
```

Because `gs` points to `_KPCR`, code reads it as:

```asm
mov rsp, gs:[80A8h]   ; 80A8h = offsetof(KPCR,Prcb) + offsetof(KPRCB,ExceptionStack)
                      ; = 0x180 + 0x7F28 = 0x80A8
```

Verification:
```
? @@c++(#FIELD_OFFSET(nt!_KPCR,Prcb)) + @@c++(#FIELD_OFFSET(nt!_KPRCB,ExceptionStack))
Evaluate expression: 32936 = 0x80a8   ✓
```

---

## Call Flow: KiPageFault → KiExceptionDispatch → Stack Switch

```
#PF interrupt
  └─ CPU delivers exception, sets up interrupt frame on kernel stack
       └─ nt!KiPageFault (sub rsp, 158h  → builds KTRAP_FRAME)
            └─ [fault not resolved by MmAccessFault]
                 └─ KiPageFault+0x438: call nt!KiExceptionDispatch
                      └─ nt!KiExceptionDispatch (sub rsp, 1D8h)
                           ├─ [if already on exception stack] → call KiDispatchException
                           └─ [if NOT on exception stack]
                                └─ call KiExceptionDispatchOnExceptionStack
                                     │  mov rax, rsp          ; save old RSP
                                     │  mov rsp, gs:[80A8h]   ; ← SWITCH to exception stack
                                     │  mov [rsp+40h], rax    ; save old RSP on exception stack
                                     │  call KiDispatchException   ; dispatch on exception stack
                                     │  mov rsp, [rsp+40h]    ; ← RESTORE old RSP
                                     └─ return to KiExceptionDispatch → return to KiPageFault
```

---

## RSP Values Observed

Breakpoints set at `nt!KiPageFault` and `nt!KiExceptionDispatch`.
Values from a real nested-fault capture (KiPageFault called during `MiFastLockLeafPageTable`):

| Point | RSP | Notes |
|-------|-----|-------|
| MiFastLockLeafPageTable (faulting site) | `ffff88800c729f60` | normal kernel stack |
| KiPageFault entry (inner, after CPU interrupt frame) | `ffff88800c729f60` → `ffff88800c729da8` | CPU pushed interrupt frame; KiPageFault: `push rbp` + `sub rsp,158h` = −0x160 total |
| `call KiExceptionDispatch` (KiPageFault+0x438) | `ffff88800c729dd0` | RSP after KiPageFault prologue |
| KiExceptionDispatch entry | `ffff88800c729dc8` | call pushed return addr (−8) |
| `mov rsp, gs:[80A8h]` | switches to exception stack | completely different address range |
| KiDispatchException entry | exception stack address | pre-allocated, per-CPU, isolated |
| After `mov rsp, [rsp+40h]` | `ffff88800c729dc8` restored | back to original KiPageFault stack |

---

## Key Assembly: The Stack Switch

### KiExceptionDispatch: decision logic (offset +0xe8 to +0x127)

```asm
; Check if RSP is already inside the exception stack
fffff800`16a14e1d  cmp   byte ptr gs:[80A6h], 0      ; ExceptionStackActive flag
fffff800`16a14e26  jne   +0x127                       ; already on it → skip switch

fffff800`16a14e28  mov   r10, gs:[80A8h]              ; load ExceptionStack top
fffff800`16a14e31  add   r10, 50h
fffff800`16a14e35  cmp   rsp, r10                     ; above top? check bottom
fffff800`16a14e38  ja    +0x106

fffff800`16a14e3a  sub   r10, 6000h                   ; check 6000h range
fffff800`16a14e41  cmp   rsp, r10
fffff800`16a14e44  jae   +0x127                       ; within range → no switch needed

fffff800`16a14e46  mov   r10, gs:[8758h]              ; also check IsrStack
fffff800`16a14e4f  cmp   rsp, r10
fffff800`16a14e52  ja    +0x120

fffff800`16a14e54  sub   r10, 6000h
fffff800`16a14e5b  cmp   rsp, r10
fffff800`16a14e5e  jae   +0x127

; RSP is not in any exception stack → switch
fffff800`16a14e60  call  nt!KiExceptionDispatchOnExceptionStack
fffff800`16a14e65  jmp   +0x12c

; RSP already on exception stack → dispatch directly
fffff800`16a14e67  call  nt!KiDispatchException
```

### KiExceptionDispatchOnExceptionStack: the actual switch

```asm
fffff800`16a01d00  mov   rax, rsp              ; [1] save current kernel stack RSP
fffff800`16a01d03  mov   r10, [rsp+28h]        ; save argument
fffff800`16a01d08  cli                          ; disable interrupts during switch
fffff800`16a01d09  mov   rsp, gs:[80A8h]       ; [2] ← SWITCH RSP to ExceptionStack
fffff800`16a01d12  mov   [rsp+40h], rax        ; [3] save old RSP on exception stack
fffff800`16a01d17  mov   [rsp+20h], r10
fffff800`16a01d1c  jmp   KxExceptionDispatchOnExceptionStack

; ... (conditionally sti) ...
fffff800`16a01d4d  call  nt!KiDispatchException      ; runs on exception stack
fffff800`16a01d52  cli
fffff800`16a01d53  mov   rsp, [rsp+40h]        ; [4] ← RESTORE old RSP
fffff800`16a01d58  jmp   KiExceptionDispatchOnExceptionStackContinue
fffff800`16a01d2d  ret
```

The RSP switch is atomic from an interrupt perspective:
- `cli` disables interrupts before `mov rsp, gs:[80A8h]`
- `sti` is restored only if RFLAGS.IF was set in the interrupted context (`[rbp+0F8h]` bit 9)

---

## Why This Exists

Without a dedicated exception stack:
1. A stack overflow exception (`EXCEPTION_STACK_OVERFLOW`) could fault while trying
   to grow the faulting stack → infinite recursion, unrecoverable crash.
2. Exception handler code (SEH/VEH) runs on the same stack as the faulting code,
   making stack analysis and recovery harder.

The exception stack (`ExceptionStack`) is a pre-allocated, fixed-size kernel stack
separate from the normal per-thread kernel stack. It is per-CPU (in `KPRCB`), so
exception dispatch is always safe regardless of the current thread's stack state.

---

## EXDI-Specific Notes

- **Do not use** `!process`, `!thread`, `~*k` in EXDI sessions — these rely on
  `KTHREAD` walking which fails with `Unable to read KTHREAD address`.
- **Use** `k`, `r`, `u`, `dt`, `dp`/`dq`, `x`, `bp`/`ba`, `p`/`t` — all work.
- Setting both `bp nt!KiPageFault` and `bp nt!KiExceptionDispatch` simultaneously:
  KiPageFault fires far more frequently (every demand-page fault); disable it with
  `bd 0` and enable only `KiExceptionDispatch` to reduce noise.
- EXDI software breakpoints write `CC` (int3) to target memory — confirmed working.
