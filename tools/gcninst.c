/*
 * gcninst - install / remove the gcn4xp driver from the command line.
 *
 *   gcninst status          list display adapters and their driver state
 *   gcninst rescan          re-enumerate hardware
 *   gcninst install <inf>   install the INF on every adapter it supports
 *   gcninst remove          remove adapters using gcnmp and delete the driver
 */
#include "tool.h"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <newdev.h>

#define SERVICE_NAME "gcnmp"

static BOOL MultiSzContains(const char *msz, const char *needle)
{
    for (; *msz; msz += lstrlenA(msz) + 1) {
        if (lstrcmpiA(msz, needle) == 0)
            return TRUE;
    }
    return FALSE;
}

/* TRUE for PCI devices whose class code says "VGA display controller". */
static BOOL IsDisplayAdapter(HDEVINFO set, SP_DEVINFO_DATA *dev)
{
    char ids[1024];

    ids[0] = ids[1] = 0;
    if (!SetupDiGetDeviceRegistryPropertyA(set, dev, SPDRP_COMPATIBLEIDS,
                                           NULL, (PBYTE)ids, sizeof(ids) - 2,
                                           NULL))
        return FALSE;
    return MultiSzContains(ids, "PCI\\CC_0300") ||
           MultiSzContains(ids, "PCI\\CC_0380");
}

static void DevProp(HDEVINFO set, SP_DEVINFO_DATA *dev, DWORD prop,
                    char *buf, DWORD cb)
{
    buf[0] = buf[1] = 0;
    SetupDiGetDeviceRegistryPropertyA(set, dev, prop, NULL, (PBYTE)buf,
                                      cb - 2, NULL);
}

static int Rescan(void)
{
    DEVINST root;

    if (CM_Locate_DevNodeA(&root, NULL, CM_LOCATE_DEVNODE_NORMAL) !=
            CR_SUCCESS ||
        CM_Reenumerate_DevNode(root, CM_REENUMERATE_SYNCHRONOUS) !=
            CR_SUCCESS) {
        Print("rescan failed\n");
        return 1;
    }
    Print("rescan done\n");
    return 0;
}

static int Status(void)
{
    HDEVINFO set;
    SP_DEVINFO_DATA dev;
    DWORD i, found = 0;

    set = SetupDiGetClassDevsA(NULL, "PCI", NULL,
                               DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return 1;
    dev.cbSize = sizeof(dev);
    for (i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        char hwid[1024], desc[256], svc[64];
        ULONG status = 0, problem = 0;

        if (!IsDisplayAdapter(set, &dev))
            continue;
        found++;
        DevProp(set, &dev, SPDRP_HARDWAREID, hwid, sizeof(hwid));
        DevProp(set, &dev, SPDRP_DEVICEDESC, desc, sizeof(desc));
        DevProp(set, &dev, SPDRP_SERVICE, svc, sizeof(svc));
        CM_Get_DevNode_Status(&status, &problem, dev.DevInst, 0);
        Print("%s\n  description: %s\n  service:     %s\n"
              "  status:      %08lx%s%s, problem code %lu\n",
              hwid, desc[0] ? desc : "(none)", svc[0] ? svc : "(none)",
              status, (status & DN_STARTED) ? " started" : "",
              (status & DN_HAS_PROBLEM) ? " has-problem" : "", problem);
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!found)
        Print("no display adapter present (try: gcninst rescan)\n");
    return 0;
}

static int Install(const char *inf)
{
    char path[MAX_PATH], *file;
    HDEVINFO set;
    SP_DEVINFO_DATA dev;
    DWORD i;
    int installed = 0, candidates = 0;
    BOOL reboot = FALSE;

    if (!GetFullPathNameA(inf, sizeof(path), path, &file) ||
        GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        Print("cannot find %s\n", inf);
        return 1;
    }

    set = SetupDiGetClassDevsA(NULL, "PCI", NULL,
                               DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return 1;
    dev.cbSize = sizeof(dev);
    for (i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        char hwid[1024], *p;
        BOOL r = FALSE;

        if (!IsDisplayAdapter(set, &dev))
            continue;
        DevProp(set, &dev, SPDRP_HARDWAREID, hwid, sizeof(hwid));
        /* reduce PCI\VEN_x&DEV_y&SUBSYS_... to the two-part ID */
        for (p = hwid; *p; p++) {
            if (p[0] == '&' && p[1] == 'S') {
                *p = 0;
                break;
            }
        }
        candidates++;
        Print("%s: ", hwid);
        if (UpdateDriverForPlugAndPlayDevicesA(NULL, hwid, path,
                                               INSTALLFLAG_FORCE, &r)) {
            Print("installed%s\n", r ? " (reboot required)" : "");
            installed++;
            reboot |= r;
        } else {
            DWORD err = GetLastError();
            if (err == ERROR_NO_SUCH_DEVINST || err == ERROR_NO_MORE_ITEMS)
                Print("not supported by this INF\n");
            else
                Print("failed, error 0x%08lx\n", err);
        }
    }
    SetupDiDestroyDeviceInfoList(set);

    if (!candidates)
        Print("no display adapter present (try: gcninst rescan)\n");
    if (installed)
        Print("%s\n", reboot ? "Reboot to start the driver."
                             : "Driver installed.");
    return installed ? 0 : 1;
}

static int Remove(void)
{
    HDEVINFO set;
    SP_DEVINFO_DATA dev;
    SC_HANDLE scm, svc;
    DWORD i;

    set = SetupDiGetClassDevsA(NULL, "PCI", NULL,
                               DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return 1;
    dev.cbSize = sizeof(dev);
    for (i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        char svcName[64], infName[MAX_PATH];
        DWORD cb = sizeof(infName);
        HKEY key;

        DevProp(set, &dev, SPDRP_SERVICE, svcName, sizeof(svcName));
        if (lstrcmpiA(svcName, SERVICE_NAME) != 0)
            continue;

        infName[0] = 0;
        key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DRV,
                                   KEY_READ);
        if (key != INVALID_HANDLE_VALUE) {
            RegQueryValueExA(key, "InfPath", NULL, NULL, (LPBYTE)infName,
                             &cb);
            RegCloseKey(key);
        }
        if (SetupDiCallClassInstaller(DIF_REMOVE, set, &dev))
            Print("device removed\n");
        else
            Print("device removal failed, error 0x%08lx\n", GetLastError());
        if (infName[0] && SetupUninstallOEMInfA(infName, SUOI_FORCEDELETE,
                                                NULL))
            Print("%s deleted\n", infName);
    }
    SetupDiDestroyDeviceInfoList(set);

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scm != NULL) {
        svc = OpenServiceA(scm, SERVICE_NAME, DELETE);
        if (svc != NULL) {
            if (DeleteService(svc))
                Print("service deleted\n");
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }
    Print("Reboot to finish; the adapter falls back to the VGA driver.\n");
    return 0;
}

/*
 * Setup treats a process on a non-interactive window station (a service, a
 * remote shell) as a "server-side" install and refuses unsigned drivers
 * outright instead of applying the machine's driver signing policy.  That
 * decision is made when setupapi loads, so run a copy of ourselves on the
 * interactive window station and relay its exit code.
 */
static BOOL RelaunchInteractive(int *exitCode)
{
    char name[64], marker[4];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD cb = 0, code = 1;

    if (GetEnvironmentVariableA("GCNINST_CHILD", marker, sizeof(marker)))
        return FALSE;
    name[0] = 0;
    GetUserObjectInformationA(GetProcessWindowStation(), UOI_NAME, name,
                              sizeof(name), &cb);
    if (lstrcmpiA(name, "WinSta0") == 0)
        return FALSE;

    SetEnvironmentVariableA("GCNINST_CHILD", "1");
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = "WinSta0\\Default";
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessA(NULL, GetCommandLineA(), NULL, NULL, TRUE, 0, NULL,
                        NULL, &si, &pi))
        return FALSE;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    *exitCode = (int)code;
    return TRUE;
}

int ToolMain(int argc, char **argv)
{
    int code;

    if (argc >= 2 && (StrEq(argv[1], "install") || StrEq(argv[1], "remove")) &&
        RelaunchInteractive(&code))
        return code;
    if (argc >= 2 && StrEq(argv[1], "status"))
        return Status();
    if (argc >= 2 && StrEq(argv[1], "rescan"))
        return Rescan();
    if (argc >= 3 && StrEq(argv[1], "install"))
        return Install(argv[2]);
    if (argc >= 2 && StrEq(argv[1], "remove"))
        return Remove();
    Print("usage: gcninst status | rescan | install <file.inf> | remove\n");
    return 2;
}
