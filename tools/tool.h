/*
 * tool.h - tiny runtime for the command line tools.  They are built without
 * a C runtime so that current compilers can still target Windows XP.
 */
#ifndef GCN_TOOL_H
#define GCN_TOOL_H

#include <windows.h>
#include <stdarg.h>

#define MAX_ARGS 16

static void Print(const char *fmt, ...)
{
    char buf[1100];
    DWORD n, written;
    va_list ap;

    va_start(ap, fmt);
    n = (DWORD)wvsprintfA(buf, fmt, ap);
    va_end(ap);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, n, &written, NULL);
}

static void PrintRaw(const char *data, DWORD n)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), data, n, &written, NULL);
}

/* Splits the process command line in place; returns argc. */
static int ParseArgs(char **argv)
{
    static char line[1024];
    char *p = line;
    int argc = 0;

    lstrcpynA(line, GetCommandLineA(), sizeof(line));
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"')
                p++;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
        }
        if (*p)
            *p++ = 0;
    }
    return argc;
}

/* Parses decimal, or hexadecimal with a 0x prefix. */
static DWORD ParseNum(const char *s)
{
    DWORD v = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (s += 2; *s; s++) {
            char c = *s;
            if (c >= '0' && c <= '9')
                v = v * 16 + (DWORD)(c - '0');
            else if (c >= 'a' && c <= 'f')
                v = v * 16 + (DWORD)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v = v * 16 + (DWORD)(c - 'A' + 10);
            else
                break;
        }
    } else {
        for (; *s >= '0' && *s <= '9'; s++)
            v = v * 10 + (DWORD)(*s - '0');
    }
    return v;
}

static int StrEq(const char *a, const char *b)
{
    return lstrcmpiA(a, b) == 0;
}

int ToolMain(int argc, char **argv);

void __cdecl ToolEntry(void)
{
    char *argv[MAX_ARGS];
    int argc = ParseArgs(argv);

    ExitProcess((UINT)ToolMain(argc, argv));
}

#endif
