# LocalRDP

A CLI tool on Windows that opens a local loopback RDP child session for the current user.

The RDP child session is a second interactive logon of the same account via the Remote Desktop technology on Windows. Display, keyboard and mouse are isolated while files, applications, and user profile data are shared.

This is **NOT** a security boundary or an isolation boundary.

## Requirements

- Windows 10 1903+, Windows 11, or Windows Server — **Pro / Enterprise / Education / Server**. (Home editions have no RDP *server* component.)
- Visual Studio 2022
- CMake

If Windows child sessions are not already enabled, the first `start` / `desktop` may prompt once for administrator approval (`WTSEnableChildSessions`).

By default, it doesn't require authentication or user logins, but sometimes the security policy will block it (so such logins are necessary).

## Build

```powershell
.\build.ps1              # Release
.\build.ps1 -Config Debug
.\build.ps1 -Clean
```

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

Clipboard redirection includes **text** (RDP CLIPRDR) and **files**. Child-session RDP does not advertise CLIPRDR file streams or create `\\tsclient` shares (the volumes are already local). File copy therefore publishes `CF_HDROP` in the other session using the shared filesystem.

`--drives` still turns on the RDP control's drive mapping (`RedirectDrives` + `DriveCollection`). That is off by default: on a child session it does not create `\\tsclient`, and it can stall the connection handshake.

Pass `--no-clipboard` to disable text and file clipboard.

`start` does not write Run/RunOnce, does not open TCP 3389, and does not change firewall, RDP-listener, TermService, or credential-delegation policy.

## Options

| Flag | Meaning |
|------|---------|
| `--width` / `--height` | Initial **window client** size (even width). The remote desktop tracks the window; resize updates the session. Default is about 5/6 of the current monitor work area, centered. |
| `--x` / `--y` | Initial host-window position in screen pixels |
| `--scale` | 100–500 percent; default follows the host window DPI and updates if the window moves to another monitor |
| `--fullscreen` | Start full screen (toggle with F11 or Ctrl+Alt+Break) |
| `--title` | Host window title |
| `--workdir` | Working directory for the launched program |
| `--timeout` | Seconds to wait for the RDP connection; default 90, minimum 5 |
| `--minimized` | Start the host window minimized |
| `--hidden` | Hide the RDP host window |
| `--view-only` | Ignore local keyboard/mouse; injected input in the child session is unaffected |
| `--no-clipboard` | Do not redirect clipboard (text or files) |
| `--drives` | Request RDP drive mapping (`\\tsclient`); off by default |
| `--keep-session` | Do not log off on disconnect |
| `--user` | Explicit username, only if auto-logon fails (PIN-only accounts, blocked delegation) |
| `--password` | Explicit password. Avoid it when possible: it is visible in the command line; prefer `--password-env`. |
| `--password-env` | Environment-variable name containing the password |

## Limitations

- Home edition is unsupported.
- One active child session **per machine**.
- Same user, same files, no isolation.
- The in-session helper dismisses only the known `CrossDeviceResume.exe` error dialog that Windows itself shows in child sessions.

## Logs

Placed at `%LOCALAPPDATA%\LocalRDP\localrdp.log`.

## License

MIT
