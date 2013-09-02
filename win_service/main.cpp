#include <stdio.h>
#include <windows.h>
#include <Dbt.h>
#include <ShlObj.h>

#include "pfs.h"

extern "C" int pfs_main(int argc, char **argv, const pfs_params* params);

#define SZSERVICENAME          L"pfs"
#define SZSERVICEDISPLAYNAME   L"PCloud File System"
#define DOKAN_DLL              L"dokan.dll"

#define KEY_USER               "username"
#define KEY_PASS               "pass"
#define KEY_AUTH               "auth"
#define KEY_CACHE_SIZE         "cachesize"
#define KEY_USE_SSL            "ssl"
#define KEY_DELETE             "del"
#define KEY_PATH               "path"

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

typedef BOOL (__stdcall *DokanUnmountType)(WCHAR DriveLetter);

BOOL ReportStatusToSCMgr(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint)
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
    hr = RegCreateKeyExA(HKEY_CURRENT_USER, REGISTRY_KEY_PCLOUD, 0, NULL, 0,
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
    hr = RegOpenKeyExA(HKEY_CURRENT_USER, REGISTRY_KEY_PCLOUD, 0, KEY_READ, &hKey);
    if (hr)
    {
        storeKey(key, "");
        hr = RegOpenKeyExA(HKEY_CURRENT_USER, REGISTRY_KEY_PCLOUD, 0, KEY_READ, &hKey);
    }
    if (!hr)
    {
        hr = RegQueryValueExA(hKey, key, NULL, NULL, (LPBYTE)data, &cbDataSize);
        RegCloseKey(hKey);
    }
}

int getIntFromRegistry(const char* key)
{
    HRESULT hr;
    DWORD val = 0;
    DWORD cbDataSize = sizeof(val);
    HKEY hKey;
    hr = RegOpenKeyExA(HKEY_CURRENT_USER, REGISTRY_KEY_PCLOUD, 0, KEY_READ, &hKey);
    if (!hr)
    {
        hr = RegQueryValueExA(hKey, key, NULL, NULL, (LPBYTE)&val, &cbDataSize);
        RegCloseKey(hKey);
        return val;
    }
    return 0;
}

char mountPoint[3] = "a:";

DWORD WINAPI ThreadProc(LPVOID lpParam)
{
    pfs_params params = {0,};
    char username[MAX_PATH]="";
    char password[MAX_PATH]="";
    char auth[MAX_PATH]="";
    char buff[MAX_PATH] = "";
    size_t cachesize;
    char* argv[2] = {(char *)"pfs", mountPoint};

    storeKey("lr", "");

    getDataFromRegistry(KEY_PATH, buff);
    if (buff[0] && isFreeDevice(buff[0]))
        mountPoint[0] = buff[0];
    else
        mountPoint[0] = getFirstFreeDevice();

    getDataFromRegistry(KEY_AUTH, auth);
    debug("auth:%s\n", auth);
    getDataFromRegistry(KEY_USER, username);
    debug("user:%s\n", username);
    getDataFromRegistry(KEY_PASS, password);
    debug("pass:%s\n", password);
    getDataFromRegistry(KEY_CACHE_SIZE, buff);
    cachesize = (size_t)atol(buff);
    debug("cache size:%u\n", cachesize);
    // Stored data is in MB - convert to bytes
    if (cachesize > 0 && cachesize < 3000)
        cachesize *= 1024*1024;
    getDataFromRegistry(KEY_USE_SSL, buff);
    debug("use SSL :%s\n", buff);
    if (!strcmp(buff, "ssl") || !strcmp(buff, "SSL"))
        params.use_ssl = 1;

    if (auth[0])
    {
        params.auth = auth;
        params.pass = NULL;
    }
    else
    {
        params.auth = NULL;
        params.pass = password;
    }

    params.username = username;
    params.use_ssl = 0;
    params.cache_size = cachesize?cachesize:512*1024*1024;

    int res = pfs_main(2, argv, &params);
    if (res == ENOTCONN)
    {
        storeKey("lr", "1");
        debug("Send NotConnected msg\n");
    }
    else if (res == EACCES)
    {
        storeKey("lr", "2");
        debug("Send Access denied msg\n");
    }
    return res;
}


VOID WINAPI ServiceStart(const wchar_t * config_file)
{
    HANDLE hThread = CreateThread(NULL, 0, ThreadProc, NULL, 0, NULL);
    debug("Thread created\n");
    unsigned int loop = 0;
    ReportStatusToSCMgr(SERVICE_RUNNING, NO_ERROR, 0);
    while (!bStop)
    {
        Sleep(500);
        if (loop < 1000 && loop % 10 == 0)
        {
            DWORD recipients = BSM_ALLDESKTOPS | BSM_APPLICATIONS;
            BroadcastSystemMessage(0, &recipients, WM_DEVICECHANGE, DBT_CONFIGCHANGED, 0);
            loop = 0;
        }
        ++loop;
    }
    ReportStatusToSCMgr(SERVICE_STOP_PENDING, NO_ERROR, 0);

    debug("Service main - waiting\n");
    WaitForSingleObject(hThread, 5000);

    debug("Service main - closing\n");
    CloseHandle(hThread);

    ReportStatusToSCMgr(SERVICE_STOPPED, NO_ERROR, 0);

    debug("Service main - exit\n");
}


VOID WINAPI ServiceStop()
{
    debug("ServiceStop\n");
    DokanUnmountType Unmount = NULL;
    HMODULE dokanDll = LoadLibraryW(DOKAN_DLL);
    if (dokanDll) Unmount=(DokanUnmountType)GetProcAddress(dokanDll, "DokanUnmount");

    bStop=TRUE;
    if (mountPoint[0] != 'a' && Unmount)
    {
        debug("Unmounting...\n");
        Unmount((WCHAR)mountPoint[0]);
    }
    if (dokanDll) FreeLibrary(dokanDll);
}


VOID WINAPI service_ctrl(DWORD dwCtrlCode)
{
    switch(dwCtrlCode)
    {
        case SERVICE_CONTROL_STOP:
            storeKey(KEY_DELETE, "+");
            debug("SERVICE_CONTROL_STOP \n");
            ReportStatusToSCMgr(SERVICE_STOP_PENDING, NO_ERROR, 0);
            ServiceStop();
            return;
        case SERVICE_CONTROL_SHUTDOWN:
            debug("SERVICE_CONTROL_SHUTDOWN \n");
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
        printf( "Unable to install %S\n", SZSERVICEDISPLAYNAME);
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
    debug("Called service main %d .\n", bStop);

    if (bStop) return;

    sshStatusHandle = RegisterServiceCtrlHandler(SZSERVICENAME, service_ctrl);
    if (sshStatusHandle)
    {
        ssStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        ssStatus.dwServiceSpecificExitCode = 0;

        if (ReportStatusToSCMgr(SERVICE_START_PENDING, NO_ERROR, 3000))
            ServiceStart(NULL);

        ReportStatusToSCMgr(SERVICE_STOPPED, dwErr, 0);
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
            debug ("Called main while service is stopping!\n");
            storeKey(KEY_DELETE, "");
            return 1;
        }

        StartServiceCtrlDispatcher(dispatchTable);
        schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
        if (schSCManager)
        {
            schService = OpenService(schSCManager, SZSERVICENAME, SERVICE_ALL_ACCESS);
            if (schService)
            {
                debug("called main function - no args\n");
                QueryServiceStatus(schService, &ssStatus);
                if (ssStatus.dwCurrentState != SERVICE_STOPPED)
                {
                    debug("called main function - status %lu\n", ssStatus.dwCurrentState);
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
