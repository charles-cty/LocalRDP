# LocalRDP

Windows CLI that opens a **local loopback Remote Desktop child session** for the current user and starts a program inside it.

The child session is a second interactive logon of the **same account**. Keyboard and mouse are isolated (each session has its own input queue). Files, installed applications, and profile data are shared. This is **not** a security boundary.

## Requirements

- Windows 10 1903+, Windows 11, or Windows Server — **Pro / Enterprise / Education / Server**. Home editions have no Remote Desktop *server* component.
- Visual Studio 2022 with MSVC and ATL (to build)
- CMake 3.21+
- If Windows child sessions are not already enabled, the first `start` / `desktop` may prompt once for administrator approval (`WTSEnableChildSessions`). LocalRDP does not change the firewall, RDP listener, TermService, or credential-delegation policy.

## Build

From PowerShell 7:

```powershell
.\build.ps1              # Release
.\build.ps1 -Config Debug
.\build.ps1 -Clean
```

The binary is `build\bin\Release\localrdp.exe`.

## Usage

```text
localrdp status
localrdp start [options] -- <program> [args...]
localrdp desktop [options] [-- <program> [args...]]
localrdp stop
```

Examples:

```powershell
localrdp start -- notepad.exe
localrdp start --fullscreen -- pwsh.exe -NoLogo
localrdp start --workdir C:\src -- gitk
localrdp desktop --title "Child desktop"
localrdp desktop -- notepad.exe
localrdp stop
```

`start` creates the child session, connects over loopback with the Remote Desktop ActiveX control (`ConnectToChildSession`), then starts your program **inside that session** (same account, using a process already running there — no Run/RunOnce registry keys). When that process exits, the child session logs off and the CLI returns.

`desktop` leaves the normal shell (Explorer) running so you get a full second desktop. An optional `-- program` is started after logon; the session stays up when that program exits.

Close the host window or press Ctrl+C to disconnect. By default the child session is then logged off; pass `--keep-session` to leave it.

## How it works

```
WTSEnableChildSessions(true)          on start, if not already on
        │
        ▼
Remote Desktop ActiveX (mstscax)
    ConnectToChildSession = true      parent session only
        │
        ▼
same-user child session (loopback)
        │
        ▼
after logon: spawn localrdp --session-entry --job <file>
    using a process already in that session (token / parent)
        │
        ▼
user-specified process
```

Traffic stays on the machine. Windows allows **one** child session at a time.

`start` does not write Run/RunOnce, does not open TCP 3389, and does not change firewall, RDP-listener, TermService, or credential-delegation policy.

## Options

| Flag | Meaning |
|------|---------|
| `--width` / `--height` | Initial **window client** size (even width). The remote desktop tracks the window; resize updates the session. Default is about 5/6 of the current monitor work area, centered. |
| `--scale` | 100–500 percent; default follows the host window DPI and updates if the window moves to another monitor |
| `--fullscreen` | Start full screen (toggle with F11 or Ctrl+Alt+Break) |
| `--workdir` | Working directory for the launched program |
| `--hidden` | Hide the RDP host window |
| `--view-only` | Ignore local keyboard/mouse; injected input in the child session is unaffected |
| `--no-clipboard` | Do not redirect clipboard |
| `--keep-session` | Do not log off on disconnect |
| `--user` / `--password-env` | Only if auto-logon fails (PIN-only accounts, blocked delegation) |

## Limits (Windows behaviour, not bugs)

- Home edition is unsupported.
- One child session per machine.
- Auto-logon needs a real account password; Windows Hello / PIN-only accounts may prompt.
- Same user, same files: do not treat this as sandboxing.
- The in-session helper dismisses only the known `CrossDeviceResume.exe` error dialog that Windows itself shows in child sessions.

## Logs

`%LOCALAPPDATA%\LocalRDP\localrdp.log`

## License

MIT
