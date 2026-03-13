# windbgskill

**WinDbg HTTP 桥接层** — 将 WinDbg 以本地 REST API 的形式对外暴露，让 AI 助手（Cursor / Claude）能够以编程方式驱动崩溃转储分析、内核调试和用户态进程调试。

[English](README.md)

---

## 项目简介

`windbgskill` 是一个 WinDbg 扩展 DLL，它在 WinDbg 内部启动一个轻量级 HTTP 服务器。加载后，任何 HTTP 客户端（包括 AI 编程助手）都可以发送 WinDbg 命令、读取输出、控制执行流程并对结果做出反应，而无需触碰 WinDbg 的 UI 界面。

这让 WinDbg 从一个交互式工具变成了**可编程的调试服务**，支持以下工作流：

- AI 引导的崩溃转储分析（`!analyze -v` → 栈回溯 → 根因总结）
- 自动化内核调试会话（设置断点、恢复执行、捕获输出）
- 脚本化用户态调试（按需检查线程、堆、句柄）

## 架构

```
┌─────────────────────────────────────────┐
│  WinDbg (windbg.exe / kd.exe / cdb.exe) │
│                                          │
│  .load windbgskill.dll                   │
│  !windbgskill start 9090                 │
│                                          │
│  ┌──────────────────────────────────┐    │
│  │  HTTP 服务器 (127.0.0.1:9090)    │    │
│  │  /api/exec   /api/status         │    │
│  │  /api/break  /api/go             │    │
│  └──────────────────────────────────┘    │
└──────────────┬──────────────────────────┘
               │ HTTP
   ┌───────────▼───────────┐
   │  AI 助手              │
   │  (Cursor / Claude /   │
   │   任意 HTTP 客户端)   │
   └───────────────────────┘
```

## 支持的调试场景

| 场景 | 示例 |
|------|------|
| **崩溃转储分析** | 进程崩溃转储（`.dmp`、`.mdmp`）、内核 BSOD 转储 |
| **实时内核调试** | KDNET、本地内核调试、VM 内核调试（VirtualBox、Hyper-V、EXDI） |
| **用户态进程调试** | 附加到进程、应用程序挂起 / 崩溃 |

## 快速上手

### 1. 获取 DLL

**方式 A — 直接使用预编译二进制（无需自行构建）**

从 [Releases](../../releases) 页面下载最新版本。

| 架构 | 文件 |
|---|---|
| x64（推荐） | `windbgskill64.dll` |
| x86 | `windbgskill.dll` |

请根据你的 WinDbg 安装版本选择对应架构。

**方式 B — 从源码构建**

用 Visual Studio 2019 或更高版本打开 `windbgskill/windbgskill.sln`，选择 **Release x64**（若 WinDbg 为 32 位则选 Win32）进行构建。

已内置依赖：
- [`cpp-httplib`](https://github.com/yhirose/cpp-httplib) — 单头文件 HTTP 库（已捆绑为 `httplib.h`）
- `dbgeng.lib` — 来自 Windows SDK / WinDbg SDK

### 2. 在 WinDbg 中加载插件

```
.load C:\path\to\windbgskill.dll
!windbgskill start 9090
```

其他可用命令：

```
!windbgskill status   ; 查看服务器是否正在运行
!windbgskill stop     ; 停止 HTTP 服务器
```

### 3. 通过 HTTP 与其交互

```powershell
# 查看调试器状态
curl.exe -s http://127.0.0.1:9090/api/status

# 执行命令并获取输出
curl.exe -s -X POST http://127.0.0.1:9090/api/exec -d "k"

# 执行 !analyze -v，设置更长超时（符号下载可能较慢）
curl.exe -s -X POST "http://127.0.0.1:9090/api/exec?timeout=180" -d "!analyze -v"

# 中断运行中的目标
curl.exe -s -X POST http://127.0.0.1:9090/api/break

# 恢复执行
curl.exe -s -X POST http://127.0.0.1:9090/api/go
```

## API 参考

Base URL：`http://127.0.0.1:{PORT}`

| 端点 | 方法 | 说明 |
|------|------|------|
| `/api/help` | GET | 自描述 API 文档（JSON）—— 供 LLM 自发现 |
| `/api/status` | GET | 调试器状态、端口及当前命令信息 |
| `/api/exec` | POST | 执行 WinDbg 命令；请求体为原始命令文本 |
| `/api/break` | POST | 中断运行中或卡住的目标 |
| `/api/go` | POST | 恢复目标执行 |
| `/api/shutdown` | POST | 远程停止 HTTP 服务器 |

### `/api/status` 响应示例

```json
{ "state": "broken", "port": 9090, "exec": { "busy": false } }
{ "state": "broken", "port": 9090, "exec": { "busy": true, "cmd": "!analyze -v", "elapsed_s": 47 } }
```

### 调试器状态说明

| 状态 | 含义 | 执行 `/api/exec` 前的准备动作 |
|------|------|-------------------------------|
| `broken` | 目标已暂停，调试器提示符活跃 | 无需额外操作，可直接发命令 |
| `running` | 目标正在执行 | 先调用 `/api/break` |
| `no_target` | 没有打开调试会话 | 请用户打开转储或附加进程 |
| `stepping` | 单步模式 | 同 `broken`，可直接发命令 |

### HTTP 错误码

| 状态码 | 含义 |
|--------|------|
| 409 | 目标正在运行，请先调用 `/api/break` |
| 503 | 引擎忙，查看 `/api/status` 中的 `exec.cmd` / `exec.elapsed_s`，再调用 `/api/break` 解除阻塞 |

### 超时参数

```powershell
# 默认超时 60 秒；对于慢速命令建议增加超时
curl.exe -s -X POST "http://127.0.0.1:9090/api/exec?timeout=180" -d "!analyze -v"
```

## 与 Cursor 配合使用（AI 辅助调试）

本仓库在 `skills/windbg/SKILL.md` 中提供了一个 **Cursor Agent Skill**。将其安装为项目级 Skill 后，Cursor 将自动了解如何：

- 识别调试场景（转储分析 / 内核调试 / 用户态调试）
- 在发送命令前检查调试器状态
- 处理 `running` → 中断 → `broken` 的状态转换
- 为慢速命令设置合适的超时时间
- 从 503 / 引擎卡死情况中恢复

参见 [`skills/windbg/examples/`](skills/windbg/examples/)，其中记录了真实调试会话作为参考案例。

## 常用 WinDbg 命令速查

### 转储分析

| 目标 | 命令 |
|------|------|
| 自动崩溃分析 | `!analyze -v` |
| 切换到出错线程上下文 | `.ecxr` |
| 调用栈 | `k` / `kb` / `kn` |
| 所有线程调用栈 | `~*k` |
| 寄存器 | `r` |
| 已加载模块 | `lm` |
| 重新加载符号 | `.reload <module>` |
| 查看结构体 | `dt <type> [addr]` |
| 堆概览 | `!heap -s` |

### 实时内核调试

| 目标 | 命令 |
|------|------|
| 列出所有进程 | `!process 0 0` |
| 进程及线程详情 | `!process 0 7` |
| 当前线程信息 | `!thread` |
| 驱动对象 | `!drvobj <name> 7` |
| 池标签分析 | `!pool <addr>` |
| 设置内核断点 | `bp nt!NtCreateFile` |
| 虚拟内存统计 | `!vm` |

### 用户态进程调试

| 目标 | 命令 |
|------|------|
| 列出线程 | `~` |
| 所有线程调用栈 | `~*k` |
| 切换到第 N 个线程 | `~Ns` |
| 显示局部变量 | `dv` |
| 搜索符号 | `x <module>!<pattern>` |
| 句柄表 | `!handle` |
| 设置断点 | `bp <module>!<function>` |

> **注意：** `~N` 在用户态表示**第 N 个线程**，在内核态和内核转储中表示**第 N 个处理器/核心**。

## 实现细节

- 使用独立的 `IDebugClient`（`g_ExecClient`）供 HTTP 线程使用，避免与 WinDbg UI 客户端的输出回调冲突。
- `OutputCapture` 实现了 `IDebugOutputCallbacks`，将命令输出重定向到 `std::string`。
- 使用 `std::timed_mutex`（`g_EngineMutex`）序列化所有引擎调用。
- Watchdog 线程监控正在运行的命令；若命令超时，自动发送 `DEBUG_INTERRUPT_PASSIVE` 解除引擎阻塞。
- 服务器仅绑定到 `127.0.0.1`，不对外暴露网络。

## 许可证

MIT
