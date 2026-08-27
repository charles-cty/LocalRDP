#include "common.hpp"

#include <algorithm>

namespace {

void PrintHelp() {
    PrintOut(L"LocalRDP " LOCALRDP_VERSION_STRING);
    PrintOut(L"Local loopback Remote Desktop child session for the current user.");
    PrintOut(L"");
    PrintOut(L"Usage:");
    PrintOut(L"  localrdp status");
    PrintOut(L"  localrdp start [options] -- <program> [args...]");
    PrintOut(L"  localrdp desktop [options] [-- <program> [args...]]");
    PrintOut(L"  localrdp stop");
    PrintOut(L"  localrdp help");
    PrintOut(L"");
    PrintOut(L"Options (start / desktop):");
    PrintOut(L"  --width N          Initial window client width (even; default ~5/6 of the monitor)");
    PrintOut(L"  --height N         Initial window client height");
    PrintOut(L"  --x N --y N        Host window position");
    PrintOut(L"  --scale N          Desktop scale percent 100–500 (default: follow window DPI)");
    PrintOut(L"  --fullscreen       Start full screen (F11 or Ctrl+Alt+Break to toggle)");
    PrintOut(L"  --title TEXT       Host window title");
    PrintOut(L"  --workdir DIR      Working directory for the launched program");
    PrintOut(L"  --user NAME        Optional explicit user name");
    PrintOut(L"  --password PASS    Optional password (prefer --password-env)");
    PrintOut(L"  --password-env VAR Read password from the environment variable VAR");
    PrintOut(L"  --timeout SEC      Connect timeout (default 90)");
    PrintOut(L"  --minimized        Start the host window minimized");
    PrintOut(L"  --hidden           Hide the host window (session still runs)");
    PrintOut(L"  --no-clipboard     Do not redirect the clipboard");
    PrintOut(L"  --view-only        Ignore local keyboard and mouse in the host window");
    PrintOut(L"  --keep-session     Do not log off the child session when the host exits");
    PrintOut(L"");
    PrintOut(L"The child session is the same Windows account as this process. It is not a");
    PrintOut(L"security boundary: files, tokens and installed apps are shared.");
    PrintOut(L"Windows allows only one child session at a time.");
}

bool Eq(const wchar_t* a, const wchar_t* b) {
    return _wcsicmp(a, b) == 0;
}

bool NeedValue(int i, int argc, const wchar_t* flag) {
    if (i + 1 >= argc) {
        PrintErr(std::wstring(L"Missing value for ") + flag);
        return false;
    }
    return true;
}

int ParseInt(const wchar_t* s, int def) {
    if (!s || !*s) return def;
    return _wtoi(s);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    LogInit();

    if (argc >= 2 && Eq(argv[1], L"--session-entry")) {
        std::wstring job;
        DWORD expect_session = kNoSession;
        for (int i = 2; i < argc; ++i) {
            if (Eq(argv[i], L"--job") && i + 1 < argc) {
                job = argv[++i];
            } else if (Eq(argv[i], L"--expect-session") && i + 1 < argc) {
                expect_session = static_cast<DWORD>(_wtol(argv[++i]));
            }
        }
        if (job.empty()) {
            LogError(L"--session-entry requires --job");
            return 1;
        }
        return CmdSessionEntry(job, expect_session);
    }

    StartOptions opt;
    std::wstring verb;
    std::vector<std::wstring> program_args;
    bool after_dash = false;

    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (after_dash) {
            program_args.emplace_back(a);
            continue;
        }
        if (Eq(a, L"--")) {
            after_dash = true;
            continue;
        }
        if (Eq(a, L"-h") || Eq(a, L"--help") || Eq(a, L"/?")) {
            PrintHelp();
            return 0;
        }
        if (Eq(a, L"--version") || Eq(a, L"-V")) {
            PrintOut(L"LocalRDP " LOCALRDP_VERSION_STRING);
            return 0;
        }
        if (Eq(a, L"--width")) {
            if (!NeedValue(i, argc, a)) return 1;
            opt.width = ParseInt(argv[++i], opt.width);
            continue;
        }
        if (Eq(a, L"--height")) {
            if (!NeedValue(i, argc, a)) return 1;
            opt.height = ParseInt(argv[++i], opt.height);
            continue;
        }
        if (Eq(a, L"--x")) {
            if (!NeedValue(i, argc, a)) return 1;
            opt.x = ParseInt(argv[++i], opt.x);
            continue;
        }
        if (Eq(a, L"--y")) {
            if (!NeedValue(i, argc, a)) return 1;
            opt.y = ParseInt(argv[++i], opt.y);
            continue;
        }
        if (Eq(a, L"--scale")) {
            if (!NeedValue(i, argc, a)) return 1;
            opt.scale = ParseInt(argv[++i], opt.scale);
            continue;
        }
        if (Eq(a, L"--timeout")) {
            if (!NeedValue(i, argc, a)) return 1;
            opt.timeout_sec = std::max(5, ParseInt(argv[++i], opt.timeout_sec));
            continue;
        }
        if (Eq(a, L"--title")) {
            if (!NeedValue(i, argc, a)) return 1;
            opt.title = argv[++i];
            continue;
        }
        if (Eq(a, L"--workdir")) {
            if (!NeedValue(i, argc, a)) return 1;
            opt.workdir = argv[++i];
            continue;
        }
        if (Eq(a, L"--user")) {
            if (!NeedValue(i, argc, a)) return 1;
            opt.user = argv[++i];
            continue;
        }
        if (Eq(a, L"--password")) {
            if (!NeedValue(i, argc, a)) return 1;
            opt.password = argv[++i];
            continue;
        }
        if (Eq(a, L"--password-env")) {
            if (!NeedValue(i, argc, a)) return 1;
            opt.password = GetEnvVar(argv[++i]);
            continue;
        }
        if (Eq(a, L"--minimized")) {
            opt.minimized = true;
            continue;
        }
        if (Eq(a, L"--hidden")) {
            opt.hidden = true;
            continue;
        }
        if (Eq(a, L"--no-clipboard")) {
            opt.clipboard = false;
            continue;
        }
        if (Eq(a, L"--view-only")) {
            opt.view_only = true;
            continue;
        }
        if (Eq(a, L"--keep-session")) {
            opt.keep_session = true;
            continue;
        }
        if (Eq(a, L"--fullscreen") || Eq(a, L"--full-screen")) {
            opt.fullscreen = true;
            continue;
        }
        if (a[0] == L'-') {
            PrintErr(std::wstring(L"Unknown option: ") + a);
            PrintErr(L"Try `localrdp help`.");
            return 1;
        }
        if (verb.empty()) {
            verb = a;
            continue;
        }
        program_args.emplace_back(a);
    }

    if (verb.empty()) {
        PrintHelp();
        return 0;
    }

    LogInfo(L"verb=" + verb);

    if (Eq(verb.c_str(), L"help")) {
        PrintHelp();
        return 0;
    }
    if (Eq(verb.c_str(), L"version")) {
        PrintOut(L"LocalRDP " LOCALRDP_VERSION_STRING);
        return 0;
    }
    if (Eq(verb.c_str(), L"status")) {
        EnvReport r = QueryEnvironment();
        PrintStatus(r);
        return r.ready() ? 0 : static_cast<int>(ExitCode::NotReady);
    }
    if (Eq(verb.c_str(), L"setup")) {
        PrintErr(L"`setup` has been removed. Child sessions are enabled automatically by start / desktop.");
        PrintErr(L"LocalRDP does not change the firewall, RDP listener, TermService, or credential policy.");
        return 1;
    }
    if (Eq(verb.c_str(), L"stop")) {
        return CmdStop();
    }
    if (Eq(verb.c_str(), L"desktop")) {
        opt.desktop = true;
        if (!program_args.empty()) opt.command = BuildCommandLine(program_args);
        return CmdStart(opt);
    }
    if (Eq(verb.c_str(), L"start")) {
        if (program_args.empty()) {
            PrintErr(L"start requires a program. Example: localrdp start -- notepad.exe");
            PrintErr(L"For a full desktop, use: localrdp desktop");
            return 1;
        }
        opt.desktop = false;
        opt.command = BuildCommandLine(program_args);
        return CmdStart(opt);
    }

    PrintErr(L"Unknown command: " + verb);
    PrintErr(L"Try `localrdp help`.");
    return 1;
}
