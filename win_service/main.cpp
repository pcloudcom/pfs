#include <stdio.h>
#include <windows.h>
#include <aclapi.h>
#include <stdint.h>
#include <Dbt.h>

#include "pfs.h"

extern "C" int pfs_main(int argc, char **argv, const pfs_params* params);

#define SZSERVICENAME          L"pfs"
#define SZSERVICEDISPLAYNAME   L"PCloud File System"
#define DOKAN_DLL              L"dokan.dll"

#define KEY_DELETE             "del"

#ifndef ENOTCONN
#   define ENOTCONN        107
#endif

#ifndef EACCES
#define EACCES 13
#endif

DWORD                   dwErr = 0;
BOOL                    bStop = FALSE;
SERVICE_STATUS_HANDLE   sshStatusHandle;
SERVICE_STATUS          ssStatus;
HANDLE                  hPipe = INVALID_HANDLE_VALUE;
HANDLE                  hMountThread = INVALID_HANDLE_VALUE;

#define OPT_MASK_COMMAND    0x00FF0000
#define OPT_MASK_OPTS       0x0000FF00
#define OPT_MASK_LETTER     0x000000FF

#define OPT_COMMAND_MOUNT   0x00010000
#define OPT_COMMAND_UMOUNT  0x00000000

#define getMountLetter(P) (((P)&OPT_MASK_LETTER) + 'A')
#define  PIPE_NAME L"\\\\.\\pipe\\pfsservicepipe"

typedef struct
{
    uint32_t options; // mount / umount, ssl, mount letter
    uint32_t cache;
    char auth[120];
}mount_params;


typedef BOOL (__stdcall *DokanUnmountType)(WCHAR DriveLetter);


#include <time.h>
void do_debug(const char *file, const char *function, int unsigned line, int unsigned level, const char *fmt, ...){
  static const struct {
    int unsigned level;
    const char *name;
  } debug_levels[]=DEBUG_LEVELS;
  static FILE *log=NULL;
  char format[512];
  va_list ap;
  const char *errname;
  int unsigned i;
  time_t currenttime;
  errname="BAD_ERROR_CODE";
  for (i=0; i<sizeof(debug_levels)/sizeof(debug_levels[0]); i++)
    if (debug_levels[i].level==level){
      errname=debug_levels[i].name;
      break;
    }
  if (!log){
    log=fopen(DEBUG_FILE, "a+");
    if (!log)
      return;
  }
  time(&currenttime);
  snprintf(format, sizeof(format), "%s: %s:%u (function %s): %s\n", errname, file, line, function, fmt);
  format[sizeof(format)-1]=0;
  va_start(ap, fmt);
  vfprintf(log, format, ap);
  va_end(ap);
  fflush(log);
}


static BOOL ReportStatusToSCMgr(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint)
{
    static DWORD dwCheckPoint = 1;
    BOOL fResult = TRUE;

    if (dwCurrentState == SERVICE_START_PENDING)
        ssStatus.dwControlsAccepted = 0;
    else
        ssStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

    ssStatus.dwCurrentState = dwCurrentState;
    ssStatus.dwWin32ExitCode = dwWin32ExitCode;
    ssStatus.dwWaitHint = dwWaitHint;

    if (( dwCurrentState == SERVICE_RUNNING ) ||
        ( dwCurrentState == SERVICE_STOPPED ))
        ssStatus.dwCheckPoint = 0;
    else
        ssStatus.dwCheckPoint = dwCheckPoint++;
    fResult = SetServiceStatus(sshStatusHandle, &ssStatus);
    return fResult;
}


static char getFirstFreeDevice()
{
    DWORD devices = GetLogicalDrives();
    for (int i = 3; i < 32; ++i)
        if ((devices & (1<<i))==0)
            return i + 'A';
    return 0;
}


static bool isFreeDevice(char letter)
{
    DWORD devices = GetLogicalDrives();
    if (letter >='a' && letter <= 'z')
        letter = letter - 'a' + 'A';
    if (letter >= 'A' && letter <= 'Z')
        return (devices & (1<<(letter-'A'))) == 0;
    return false;
}


static void storeKey(LPCSTR key, const char * val)
{
    HRESULT hr;
    HKEY hKey;
    hr = RegCreateKeyExA(HKEY_LOCAL_MACHINE, REGISTRY_KEY_PCLOUD, 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &hKey, NULL);
    if (!hr)
    {
        hr = RegSetValueExA(hKey, key, 0, REG_SZ, (LPBYTE)val, strlen(val)+1);
        RegCloseKey(hKey);
    }
}


static void getDataFromRegistry(const char* key, char data[MAX_PATH])
{
    HRESULT hr;
    char buffer[MAX_PATH];
    DWORD cbDataSize = sizeof(buffer);
    HKEY hKey;
    hr = RegOpenKeyExA(HKEY_LOCAL_MACHINE, REGISTRY_KEY_PCLOUD, 0, KEY_READ, &hKey);
    if (hr)
    {
        storeKey(key, "");
        hr = RegOpenKeyExA(HKEY_LOCAL_MACHINE, REGISTRY_KEY_PCLOUD, 0, KEY_READ, &hKey);
    }
    if (!hr)
    {
        hr = RegQueryValueExA(hKey, key, NULL, NULL, (LPBYTE)data, &cbDataSize);
        RegCloseKey(hKey);
    }
}


static void setVolumeIcon(char letter, bool create)
{
    HRESULT hr;
    WCHAR data[MAX_PATH];
    DWORD cbDataSize = sizeof(data);
    HKEY hKey;

    char path[] = "Software\\Classes\\Applications\\explorer.exe\\Drives\\a\\DefaultIcon";
    path[sizeof(path)-sizeof("DefaultIcon")-2] = letter;

    if (create)
    {
        hr = RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"Software\\PCloud\\pCloud", 0, KEY_READ, &hKey);
        if (!hr)
        {
            hr = RegQueryValueEx(hKey, L"Install_Dir", NULL, NULL, (LPBYTE)data, &cbDataSize);
            wcscat(data, L"\\pCloud.exe,0");
            RegCloseKey(hKey);
            if (!hr)
            {
                hr = RegCreateKeyExA(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hKey, NULL);
                if(!hr)
                {
                    hr = RegSetValueEx(hKey, NULL, 0, REG_SZ, (LPBYTE)data, 2*(wcslen(data)+1));
                    RegCloseKey(hKey);
                }
            }
        }
    }
    else
    {
        hr = RegDeleteKeyA(HKEY_LOCAL_MACHINE, path);
    }
}


static void setRecycleBin(char letter, bool create)
{
#ifndef _USE_PIPE
    HRESULT hr;
    DWORD data;
    HKEY hKey;

    char root[] = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\BitBucket";
    char path[] = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\BitBucket\\a";
    path[sizeof(path)-2] = letter;

    if (create)
    {
        hr = RegOpenKeyExA(HKEY_LOCAL_MACHINE, root, 0, KEY_ALL_ACCESS, &hKey);
        debug(D_NOTICE, "RegOpenKeyExA - %ld, %s", hr, path);
        if (hr == ERROR_FILE_NOT_FOUND)
        {
            hr = RegCreateKeyExA(HKEY_LOCAL_MACHINE, root, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hKey, NULL);
            debug(D_NOTICE, "RegCreateKeyExA - %ld", hr);
        }
        if (!hr)
        {
            data = 0;
            hr = RegSetValueExA(hKey, "UseGlobalSettings", 0, REG_DWORD, (LPBYTE)&data, 4);
            debug(D_NOTICE, "RegSetValueExA - %ld", hr);
            RegCloseKey(hKey);
            if (!hr)
            {
                hr = RegCreateKeyExA(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hKey, NULL);
                debug(D_NOTICE, "RegCreateKeyExA - %ld", hr);
                if(!hr)
                {
                    data = 1;
                    hr = RegSetValueExA(hKey, "NukeOnDelete", 0, REG_DWORD, (LPBYTE)&data, 4);
                    debug(D_NOTICE, "RegSetValueExA - %ld", hr);
                    RegCloseKey(hKey);
                }
            }
        }
    }
    else
    {
        hr = RegDeleteKeyA(HKEY_LOCAL_MACHINE, path);
        debug(D_NOTICE, "RegDeleteKeyA - %ld", hr);
    }
#endif //_USE_PIPE
}

char mountPoint[3] = "a:";

DWORD WINAPI ThreadProc(LPVOID lpParam)
{
    if (!lpParam)
    {
        debug(D_ERROR, "Invalid parameters passed to ThreadPorc");
        return 1;
    }

    mount_params mp = *((mount_params*)lpParam);
    memset(lpParam, 0, sizeof(mount_params));
    pfs_params params = {0,};
    char auth[MAX_PATH]="";
    size_t cachesize;
    char* argv[2] = {(char *)"pfs", mountPoint};

    if (isFreeDevice(getMountLetter(mp.options)))
    {
        mountPoint[0] = getMountLetter(mp.options);
    }
    else
    {
        mountPoint[0] = getFirstFreeDevice();
    }

    memcpy(auth, mp.auth, sizeof(mp.auth));
    debug(D_NOTICE, "auth:%s", auth);

    cachesize = mp.cache;
    debug(D_NOTICE, "cache size:%u", cachesize);

    // Stored data is in MB - convert to bytes
    if (cachesize > 0 && cachesize < 3000)
    {
        cachesize *= 1024*1024;
    }

    params.use_ssl = (mp.options & OPT_MASK_OPTS) != 0;
    debug(D_NOTICE, "use SSL :%d", params.use_ssl);

    if (auth[0])
    {
        params.auth = auth;
        params.pass = NULL;
    }
    else
    {
        params.auth = NULL;
    }

    params.username = NULL;
    params.cache_size = cachesize?cachesize:512*1024*1024;

    setVolumeIcon(mountPoint[0], true);
    setRecycleBin(mountPoint[0], true);
    int res = pfs_main(2, argv, &params);
    setVolumeIcon(mountPoint[0], false);
    setRecycleBin(mountPoint[0], false);
    if (res == ENOTCONN)
    {
        debug(D_NOTICE, "Send NotConnected msg");
    }
    else if (res == EACCES)
    {
        debug(D_NOTICE, "Send Access denied msg");
    }
    return res;
}

VOID mount(mount_params* pparams)
{
    if (hMountThread != INVALID_HANDLE_VALUE)
    {
        debug(D_WARNING, "Already mounted!");
        return;
    }

    hMountThread = CreateThread(NULL, 0, ThreadProc, pparams, 0, NULL);
    if (!hMountThread || hMountThread == INVALID_HANDLE_VALUE)
    {
        debug(D_ERROR, "Failed to create thread!");
        hMountThread = INVALID_HANDLE_VALUE;
        return;
    }

    debug(D_NOTICE, "Thread created");
    int loop = 3;
    while (loop > 0)
    {
        DWORD recipients = BSM_ALLDESKTOPS | BSM_APPLICATIONS;
        BroadcastSystemMessage(0, &recipients, WM_DEVICECHANGE, DBT_CONFIGCHANGED, 0);
        --loop;
    }
}

VOID unmount()
{
    DokanUnmountType Unmount = NULL;
    HMODULE dokanDll = LoadLibraryW(DOKAN_DLL);
    if (dokanDll) Unmount=(DokanUnmountType)GetProcAddress(dokanDll, "DokanUnmount");

    if (mountPoint[0] != 'a' && Unmount)
    {
        debug(D_NOTICE, "Unmounting %s ...", mountPoint);
        setVolumeIcon(mountPoint[0], false);
        Unmount((WCHAR)mountPoint[0]);
    }
    if (dokanDll) FreeLibrary(dokanDll);

    if (hMountThread && hMountThread != INVALID_HANDLE_VALUE)
    {
        WaitForSingleObject(hMountThread, 1000);
        debug(D_NOTICE, "Closing thread");
        TerminateThread(hMountThread, 0);
        CloseHandle(hMountThread);
        hMountThread = INVALID_HANDLE_VALUE;
    }

    DWORD recipients = BSM_ALLDESKTOPS | BSM_APPLICATIONS;
    BroadcastSystemMessage(0, &recipients, WM_DEVICECHANGE, DBT_CONFIGCHANGED, 0);

    debug(D_NOTICE, "Unmounted!");
    Sleep(2000);
    exit(0);
}

VOID disconnect_pipe()
{
    if (hPipe != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
        hPipe = INVALID_HANDLE_VALUE;
    }
}

VOID WINAPI ServiceStart(const wchar_t * config_file)
{
    PSID pEveryoneSID = NULL;
    PACL pACL = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;
    EXPLICIT_ACCESS ea[1];
    SID_IDENTIFIER_AUTHORITY SIDAuthWorld = {SECURITY_WORLD_SID_AUTHORITY};
    SECURITY_ATTRIBUTES sa;

    if(!AllocateAndInitializeSid(&SIDAuthWorld, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &pEveryoneSID))
    {
        goto cleanup;
    }

    ZeroMemory(&ea, sizeof(EXPLICIT_ACCESS));
    ea[0].grfAccessPermissions = SPECIFIC_RIGHTS_ALL | STANDARD_RIGHTS_ALL;
    ea[0].grfAccessMode = SET_ACCESS;
    ea[0].grfInheritance= NO_INHERITANCE;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[0].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea[0].Trustee.ptstrName  = (LPTSTR) pEveryoneSID;

    if (SetEntriesInAcl(1, ea, NULL, &pACL))
    {
        goto cleanup;
    }

    pSD = (PSECURITY_DESCRIPTOR) malloc(SECURITY_DESCRIPTOR_MIN_LENGTH);
    if(!InitializeSecurityDescriptor(pSD, SECURITY_DESCRIPTOR_REVISION))
    {
        goto cleanup;
    }
    if (!SetSecurityDescriptorDacl(pSD, TRUE, pACL, FALSE))
    {
        goto cleanup;
    }
    sa.nLength = sizeof (SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = pSD;
    sa.bInheritHandle = FALSE;

    hPipe = CreateNamedPipe(PIPE_NAME, PIPE_ACCESS_INBOUND,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                            1, 0, sizeof(mount_params), 0, &sa);
    mount_params params;
    ReportStatusToSCMgr(SERVICE_RUNNING, NO_ERROR, 0);
    while (!bStop)
    {
        DWORD read = 0, total;
        if (hPipe != INVALID_HANDLE_VALUE)
        {
            debug(D_NOTICE, "Service main - Connecting pipe...");
            if (!ConnectNamedPipe(hPipe, NULL))
            {
                Sleep(500);
                debug(D_ERROR, "Service main - failed to connect pipe %lu ...", GetLastError());
                continue;
            }
            debug(D_NOTICE, "Service main - Connected pipe...");
        }
        else
        {
            debug(D_ERROR, "Service main - Braking because of broken pipe.");
            break;
        }

        debug(D_NOTICE, "Service main - received command");

        if (ReadFile(hPipe, &params, sizeof(mount_params), &read, NULL))
        {
            total = read;
            while (read && total < sizeof(mount_params))
            {
                if (!ReadFile(hPipe, ((char*)&params+total), sizeof(mount_params)-total, &read, NULL))
                {
                    read = 0;
                    debug(D_ERROR, "Service main - error inner read data from pipe");
                }
                total += read;
            }

            if (total == sizeof(mount_params))
            {
                if ((params.options & OPT_MASK_COMMAND) == OPT_COMMAND_MOUNT)
                {
                    debug(D_NOTICE, "Service main - mounting, %s", params.auth);
                    mount(&params);
                    Sleep(200); // wait thread to start
                }
                else
                {
                    debug(D_NOTICE, "Service main - unmounting");
                    DisconnectNamedPipe(hPipe);
                    unmount();
                }
            }
        }
        else
        {
            debug(D_ERROR, "Service main - error read data from pipe");
        }
        DisconnectNamedPipe(hPipe);
    }

cleanup:
    if (pEveryoneSID) FreeSid(pEveryoneSID);
    if (pACL) free(pACL);
    if (pSD) free(pSD);

    ReportStatusToSCMgr(SERVICE_STOP_PENDING, NO_ERROR, 0);

    disconnect_pipe();
    unmount();

    ReportStatusToSCMgr(SERVICE_STOPPED, NO_ERROR, 0);
    storeKey(KEY_DELETE, "");

    debug(D_NOTICE, "Service main - exit");
}


VOID WINAPI ServiceStop()
{
    debug(D_NOTICE, "ServiceStop");
    bStop=TRUE;
    disconnect_pipe();
    unmount();
    ReportStatusToSCMgr(SERVICE_STOPPED, NO_ERROR, 0);
    storeKey(KEY_DELETE, "");
}


VOID WINAPI service_ctrl(DWORD dwCtrlCode)
{
    switch(dwCtrlCode)
    {
        case SERVICE_CONTROL_STOP:
            storeKey(KEY_DELETE, "+");
            debug(D_NOTICE, "SERVICE_CONTROL_STOP");
            ReportStatusToSCMgr(SERVICE_STOP_PENDING, NO_ERROR, 0);
            ServiceStop();
            return;
        case SERVICE_CONTROL_SHUTDOWN:
            debug(D_NOTICE, "SERVICE_CONTROL_SHUTDOWN");
            ReportStatusToSCMgr(SERVICE_STOP_PENDING, NO_ERROR, 2000);
            ServiceStop();
            return;
        default:
            break;
    }
    ReportStatusToSCMgr(ssStatus.dwCurrentState, NO_ERROR, 0);
}


void CmdInstallService(BOOL Start)
{
    SC_HANDLE       schService = NULL;
    SC_HANDLE       schSCManager = NULL;

    TCHAR szPath[512];

    if (GetModuleFileName(NULL, szPath, 512) == 0)
    {
        printf("Unable to install %S\n", SZSERVICEDISPLAYNAME);
        return;
    }

    schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (schSCManager)
    {
        schService = CreateService(schSCManager, SZSERVICENAME, SZSERVICEDISPLAYNAME,
            SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS  | SERVICE_INTERACTIVE_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            szPath, NULL, NULL, NULL, NULL, NULL);

        if (schService)
        {
            printf("%S installed.\n", SZSERVICEDISPLAYNAME);
        } else
        {
            printf("CreateService failed %lu!\n", GetLastError());
            return;
        }
        SC_ACTION action;
        SERVICE_FAILURE_ACTIONS sfa;
        sfa.dwResetPeriod = INFINITE;
        sfa.lpRebootMsg = NULL;
        sfa.lpCommand = NULL;
        sfa.cActions = 1;
        sfa.lpsaActions = &action;
        sfa.lpsaActions->Type = SC_ACTION_RESTART;
        sfa.lpsaActions->Delay = 2000;
        ChangeServiceConfig2(schService, SERVICE_CONFIG_FAILURE_ACTIONS, &sfa);
    } else
    {
        printf("OpenSCManager failed!\n");
        return;
    }

    if (Start && StartService(schService, 0, NULL))
    {
        printf("Starting %S.\n", SZSERVICEDISPLAYNAME);
        Sleep(1000);
        while (QueryServiceStatus(schService, &ssStatus))
        {
            if (ssStatus.dwCurrentState == SERVICE_START_PENDING)
                Sleep(1000);
            else
                break;
        }

        if (ssStatus.dwCurrentState == SERVICE_RUNNING)
        {
            printf("\n%S started.\n", SZSERVICEDISPLAYNAME);
        } else
        {
            printf("\n%S failed to start.\n", SZSERVICEDISPLAYNAME);
        }
    }
    else
    {
        if (Start) printf("\n%S failed to start.\n", SZSERVICEDISPLAYNAME);
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
}


void CmdRemoveService()
{
    SC_HANDLE       schService;
    SC_HANDLE       schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

    if (schSCManager)
    {
        schService = OpenService(schSCManager, SZSERVICENAME, SERVICE_ALL_ACCESS);
        if (schService)
        {
            ControlService(schService, SERVICE_CONTROL_STOP, &ssStatus);

            printf("Stopping %S.\n", SZSERVICEDISPLAYNAME);
            int retry = 5;

            while(QueryServiceStatus(schService, &ssStatus) && retry)
            {
                if (ssStatus.dwCurrentState == SERVICE_STOPPED)
                    break;
                if (ssStatus.dwCurrentState == SERVICE_STOP_PENDING)
                    Sleep(1000);
                else
                {
                    printf("Stopping service - status %lu?\n", ssStatus.dwCurrentState);
                    Sleep(1000);
                    --retry;
                }
            }
            if (ssStatus.dwCurrentState == SERVICE_STOPPED)
            {
                printf("\n%S stopped.\n", SZSERVICEDISPLAYNAME);
            }else
            {
                printf("\n%S failed to stop.\n", SZSERVICEDISPLAYNAME);
            }

            if(DeleteService(schService))
            {
                printf("%S removed.\n", SZSERVICEDISPLAYNAME);
            } else
            {
                printf("DeleteService failed %u!\n", (UINT32)GetLastError());
            }
            CloseServiceHandle(schService);
        }
        else
        {
            printf("OpenService failed!\n");
        }
        CloseServiceHandle(schSCManager);
    }
    else
    {
        printf("OpenSCManager failed!\n");
    }
}


VOID WINAPI service_main(DWORD dwArgc, LPTSTR *lpszArgv)
{
    debug(D_NOTICE, "Called service main %d .", bStop);

    if (bStop) return;

    sshStatusHandle = RegisterServiceCtrlHandler(SZSERVICENAME, service_ctrl);
    if (sshStatusHandle)
    {
        ssStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        ssStatus.dwServiceSpecificExitCode = 0;

        if (ReportStatusToSCMgr(SERVICE_START_PENDING, NO_ERROR, 3000))
            ServiceStart(NULL);

        ReportStatusToSCMgr(SERVICE_STOPPED, dwErr, 0);
        storeKey(KEY_DELETE, "");
    }
}


int main(int argc, char* args[])
{
    SERVICE_TABLE_ENTRY dispatchTable[] =
    {
        {(wchar_t*)SZSERVICENAME, (LPSERVICE_MAIN_FUNCTION)service_main},
        {NULL, NULL}
    };

    if (argc > 1)
    {
        if (((args[1][0] == '-') || (args[1][0] == '/')))
        {
            if (!strcmp("install", args[1]+1))
            {
                bStop = FALSE;
                CmdInstallService(argc > 2);
                return 0;
            }
            else if (!strcmp("remove", args[1]+1))
            {
                bStop = TRUE;
                CmdRemoveService();
                return 0;
            }
            else
            {
                goto dispatch;
            }
        }
    }
    else
    {
        SC_HANDLE       schService = NULL;
        SC_HANDLE       schSCManager = NULL;
        SERVICE_STATUS  ssStatus;
        char buff[8];

        getDataFromRegistry(KEY_DELETE, buff);
        if (buff[0] == '+')
        {
            debug (D_NOTICE, "Called main while service is stopping!");
            return 1;
        }

        StartServiceCtrlDispatcher(dispatchTable);
        schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
        if (schSCManager)
        {
            schService = OpenService(schSCManager, SZSERVICENAME, SERVICE_ALL_ACCESS);
            if (schService)
            {
                debug(D_NOTICE, "called main function - no args");
                QueryServiceStatus(schService, &ssStatus);
                if (ssStatus.dwCurrentState != SERVICE_STOPPED)
                {
                    debug(D_NOTICE, "called main function - status %lu", ssStatus.dwCurrentState);
                    return 1;
                }
                if (StartService(schService, 0, NULL))
                {
                    printf("Starting %S.\n", SZSERVICEDISPLAYNAME);
                    Sleep(1000);
                    int retry = 5;
                    while(QueryServiceStatus(schService, &ssStatus) && retry)
                    {
                        --retry;
                        if (ssStatus.dwCurrentState == SERVICE_START_PENDING)
                            Sleep(1000);
                        else
                            break;
                    }
                    if (ssStatus.dwCurrentState == SERVICE_RUNNING)
                    {
                        printf("\n%S started.\n", SZSERVICENAME);
                    }
                    else
                    {
                        printf("\n%S failed to start.\n", SZSERVICENAME);
                    }
                }
                else
                {
                    printf("\nFailed to start %S.\n", SZSERVICENAME);
                }
                CloseServiceHandle(schService);
            }
            else
            {
                printf("Failed to load the service... \n");
                return 1;
            }
        }

        CloseServiceHandle(schSCManager);
        return 0;
    }

dispatch:
    printf("Usage\n"
           "  -install\t\tinstall the service\n"
           "  -remove \t\tremove the service\n");
    return 1;
}
