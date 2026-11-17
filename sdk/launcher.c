/*
 * launcher.c - one program for all the LLVM tool aliases in the SDK.
 *
 * LLVM tools take their personality from the name they are started under
 * (clang++ vs clang, ld.lld vs lld-link ...).  On a Unix system the aliases
 * are links; a zip archive cannot hold those, and copies would add hundreds
 * of megabytes.  So each alias is a copy of this small program: it starts
 * the real tool next to it with the option that selects the same
 * personality, followed by the original arguments, and passes the exit
 * status back.  (Passing the alias name as argv[0] does not work: on
 * Windows LLVM replaces argv[0] with the executable's own file name.)
 *
 * Tools without such an option (llvm-ranlib, llvm-strip) are real copies.
 *
 * Plain Win32, nothing newer than Windows XP.
 */
#include <windows.h>

static const struct {
    const wchar_t *alias;
    const wchar_t *tool;
    const wchar_t *args;    /* inserted before the original arguments */
} g_map[] = {
    { L"clang++", L"clang", L" --driver-mode=g++" },
    { L"clang-cl", L"clang", L" --driver-mode=cl" },
    { L"clang-cpp", L"clang", L" --driver-mode=cpp" },
    { L"ld.lld", L"lld", L" -flavor gnu" },
    { L"lld-link", L"lld", L" -flavor link" },
    { L"llvm-readelf", L"llvm-readobj", L" --elf-output-style=GNU" },
};

static void fail(const wchar_t *what, const wchar_t *name)
{
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    wchar_t msg[MAX_PATH + 128];
    DWORD n;

    lstrcpyW(msg, L"launcher: ");
    lstrcatW(msg, what);
    lstrcatW(msg, name);
    lstrcatW(msg, L"\r\n");
    WriteConsoleW(err, msg, lstrlenW(msg), &n, NULL);
    ExitProcess(127);
}

int main(void)
{
    wchar_t self[MAX_PATH], tool[MAX_PATH + 32], *base, *dot, *cmd, *rest;
    const wchar_t *target = NULL, *args = L"";
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1, len;
    unsigned i;

    len = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        fail(L"cannot find my own path", L"");

    /* which alias am I? */
    base = self;
    for (wchar_t *p = self; *p; p++)
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    lstrcpyW(tool, self);
    dot = NULL;
    for (wchar_t *p = base; *p; p++)
        if (*p == L'.')
            dot = p;
    {
        wchar_t name[MAX_PATH];

        lstrcpynW(name, base, MAX_PATH);
        if (dot != NULL && lstrcmpiW(dot, L".exe") == 0)
            name[dot - base] = 0;
        for (i = 0; i < sizeof(g_map) / sizeof(g_map[0]); i++)
            if (lstrcmpiW(name, g_map[i].alias) == 0) {
                target = g_map[i].tool;
                args = g_map[i].args;
            }
        if (target == NULL)
            fail(L"unknown alias ", name);
    }
    /* the tool is next to me */
    tool[base - self] = 0;
    lstrcatW(tool, target);
    lstrcatW(tool, L".exe");

    /* my command line, minus my own name (Microsoft's parsing rules) */
    rest = GetCommandLineW();
    if (*rest == L'"') {
        for (rest++; *rest && *rest != L'"'; rest++)
            ;
        if (*rest)
            rest++;
    } else {
        while (*rest && *rest != L' ' && *rest != L'\t')
            rest++;
    }
    cmd = (wchar_t *)HeapAlloc(GetProcessHeap(), 0,
                               (lstrlenW(tool) + lstrlenW(args) +
                                lstrlenW(rest) + 4) * sizeof(wchar_t));
    if (cmd == NULL)
        fail(L"out of memory", L"");
    lstrcpyW(cmd, L"\"");
    lstrcatW(cmd, tool);
    lstrcatW(cmd, L"\"");
    lstrcatW(cmd, args);
    lstrcatW(cmd, rest);

    /* Ctrl+C goes to the tool, which shares the console; keep waiting */
    SetConsoleCtrlHandler(NULL, TRUE);
    GetStartupInfoW(&si);
    if (!CreateProcessW(tool, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si,
                        &pi))
        fail(L"cannot start ", tool);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}
