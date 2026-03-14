# windbgskill

**WinDbg HTTP 桥接层** — 将 WinDbg 以本地 REST API 的形式对外暴露，让 AI 助手（Cursor / Claude）能够以编程方式驱动崩溃转储分析、内核调试和用户态进程调试。

[English](README.md)

---

## 项目简介

`windbgskill` 是一个 WinDbg 扩展 DLL，它在 WinDbg 内部启动一个轻量级 HTTP 服务器。加载后，AI 助手即可发送 WinDbg 命令、读取输出、控制执行流程，全程无需触碰 WinDbg 的 UI 界面。

支持的调试场景：

| 场景 | 示例 |
|------|------|
| **崩溃转储分析** | 进程崩溃转储（`.dmp`、`.mdmp`）、内核 BSOD 转储 |
| **实时内核调试** | KDNET、串口调试、EXDI、本地内核调试 |
| **用户态进程调试** | 附加到进程、应用程序挂起 / 崩溃 |

## 快速上手

### 1. 获取 DLL

预编译二进制在 Releases 页面（x64 用 `windbgskill64.dll`，x86 用 `windbgskill.dll`）。如需自行构建，用 Visual Studio 2019+ 打开 `windbgskill/windbgskill.sln` 编译 Release x64。

### 2. 在 WinDbg 中加载插件

```
.load C:\path\to\windbgskill.dll

; 默认：监听 127.0.0.1:9090（仅本机访问）
!windbgskill start

; 仅指定端口（仍绑定到 127.0.0.1）
!windbgskill start 9090

; 指定 IP 和端口（使用 0.0.0.0 可允许其他机器远程访问）
!windbgskill start 0.0.0.0 9090
```

```
!windbgskill status   ; 查看服务器是否正在运行
!windbgskill stop     ; 停止 HTTP 服务器
```

### 3. 告诉 AI 端口号

> "windbgskill 插件运行在端口 9090"

AI 会自动接管后续操作。确保 `curl.exe` 已加入 `PATH`。

## 部署 AI Skill

本仓库在 `skills/windbg/SKILL.md` 中提供了一个 Cursor Agent Skill，教会 AI 如何使用本插件（状态机、超时处理、场景识别等）。

### Cursor

将 `skills/windbg/SKILL.md` 复制到目标项目的以下路径：

```
your-project/
└── .cursor/
    └── skills/
        └── windbg/
            └── SKILL.md
```

### Claude（claude.ai 项目 / Claude Desktop）

将 `skills/windbg/SKILL.md` 的内容粘贴到项目说明或自定义说明中。

---

参见 [`skills/windbg/examples/`](skills/windbg/examples/)，其中记录了真实调试会话作为参考案例。

## 许可证

MIT
