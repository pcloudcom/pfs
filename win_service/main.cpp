#include <stdio.h>
#include <windows.h>
#include <ShlObj.h>

#include "pfs.h"

#define SZSERVICENAME          L"pfs"
#define SZSERVICEDISPLAYNAME   L"PCloud File System"

#define REGISTRY_KEY_PCLOUD    L"SOFTWARE\\PCloud\\pfs"

#define KEY_USER               "username"
#define KEY_PASS               "pass"
#define KEY_AUTH               "auth"

SERVICE_STATUS          ssStatus;
SERVICE_STATUS_HANDLE   sshStatusHandle;
DWORD                   dwErr = 0;
BOOL                    bStop = FALSE;
WCHAR                   szErr[256];


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
    for (int i = 4; i < 32; ++i)
        if ((devices & (1<<i))==0)
            return i + 'A';
    return 0;
}


static void storeKeys()
{
    HRESULT hr;
    char buffer[8];
    HKEY hKey;
    hr = RegCreateKeyEx(HKEY_LOCAL_MACHINE, REGISTRY_KEY_PCLOUD, 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &hKey, NULL);
    if (!hr)
    {
        strcpy(buffer, "");
        hr = RegSetValueExA(hKey, KEY_USER, 0, REG_SZ, (LPBYTE)buffer, strlen(buffer)+1);
        hr = RegSetValueExA(hKey, KEY_PASS, 0, REG_SZ, (LPBYTE)buffer, strlen(buffer)+1);
        RegCloseKey(hKey);
    }
}


static void getDataFromRegistry(const char* key, char data[MAX_PATH])
{
    HRESULT hr;
    char buffer[MAX_PATH];
    DWORD cbDataSize = sizeof(buffer);
    HKEY hKey;
    hr = RegOpenKeyEx(HKEY_LOCAL_MACHINE, REGISTRY_KEY_PCLOUD, 0, KEY_READ, &hKey);
    if (hr)
    {
        storeKeys();
        hr = RegOpenKeyEx(HKEY_LOCAL_MACHINE, REGISTRY_KEY_PCLOUD, 0, KEY_READ, &hKey);
    }
    if (!hr)
    {
        hr = RegQueryValueExA(hKey, key, NULL, NULL, (LPBYTE)data, &cbDataSize);
        RegCloseKey(hKey);
    }
}


DWORD WINAPI ThreadProc(LPVOID lpParam)
{
    char mountPoint[3] = "g:";
    char username[MAX_PATH]="";
    char password[MAX_PATH]="";

    char* params[2] = {(char *)"pfs", mountPoint};

    mountPoint[0] = getFirstFreeDevice();
    getDataFromRegistry(KEY_USER, username);
    getDataFromRegistry(KEY_PASS, password);

    return pfs_main(2, params, username, password);
}


VOID WINAPI ServiceStart(const wchar_t * config_file)
{
    ReportStatusToSCMgr(SERVICE_RUNNING, NO_ERROR, 0);
    HANDLE hThread = CreateThread(NULL, 0, ThreadProc, NULL, 0, NULL);
    debug("Thread created\n");
    while (!bStop)
    {
        Sleep (1000);
    }
    debug("Thread stopped\n");
    WaitForSingleObject(hThread, 500);
    CloseHandle(hThread);
    ReportStatusToSCMgr(SERVICE_STOPPED, NO_ERROR, 0);
}


VOID WINAPI ServiceStop()
{
    bStop=TRUE;
    Sleep (2000);
}


VOID WINAPI service_ctrl(DWORD dwCtrlCode)
{
    switch(dwCtrlCode)
    {
        case SERVICE_CONTROL_STOP:
            ReportStatusToSCMgr(SERVICE_STOP_PENDING, NO_ERROR, 0);
            ServiceStop();
            return;
        case SERVICE_CONTROL_SHUTDOWN:
            ReportStatusToSCMgr(SERVICE_STOP_PENDING, NO_ERROR, 20000);
            ServiceStop();
            return;
        default:
            break;
    }
    ReportStatusToSCMgr(ssStatus.dwCurrentState, NO_ERROR, 0);
}


void CmdInstallService()
{
    SC_HANDLE   schService = NULL;
    SC_HANDLE   schSCManager = NULL;

    TCHAR szPath[512];

    if (GetModuleFileName(NULL, szPath, 512) == 0)
    {
        debug( "Unable to install %S\n", SZSERVICEDISPLAYNAME);
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
            debug("%S installed.\n", SZSERVICEDISPLAYNAME);
        } else
        {
            debug("CreateService failed!\n");
            return;
        }
    } else
    {
        debug("OpenSCManager failed!\n");
        return;
    }

    if (StartService(schService, 0, NULL))
    {
        debug("Starting %S.\n", SZSERVICEDISPLAYNAME);
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
            debug("\n%S started.\n", SZSERVICEDISPLAYNAME);
        } else
        {
            debug("\n%S failed to start.\n", SZSERVICEDISPLAYNAME);
        }
    }
    else
    {
        debug("\n%S failed to start.\n", SZSERVICEDISPLAYNAME);
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
}


void CmdRemoveService()
{
    SC_HANDLE   schService;
    SC_HANDLE   schSCManager;
    schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

    if (schSCManager)
    {
        schService = OpenService(schSCManager, SZSERVICENAME, SERVICE_ALL_ACCESS);
        if (schService)
        {
            if (ControlService(schService, SERVICE_CONTROL_STOP, &ssStatus))
            {
                debug("Stopping %S.\n", SZSERVICEDISPLAYNAME);
                Sleep(1000);
                while(QueryServiceStatus(schService, &ssStatus))
                {
                    if ( ssStatus.dwCurrentState == SERVICE_STOP_PENDING )
                        Sleep(1000);
                    else
                        break;
                }
                if (ssStatus.dwCurrentState == SERVICE_STOPPED)
                {
                    debug("\n%S stopped.\n", SZSERVICEDISPLAYNAME);
                }else
                {
                    debug("\n%S failed to stop.\n", SZSERVICEDISPLAYNAME);
                }
            }
            if(DeleteService(schService))
            {
                debug("%S removed.\n", SZSERVICEDISPLAYNAME);
            } else
            {
                debug("DeleteService failed!\n");
            }
            CloseServiceHandle(schService);
        }
        else
        {
            debug("OpenService failed!\n");
        }
        CloseServiceHandle(schSCManager);
    }
    else
    {
        debug("OpenSCManager failed!\n");
    }
}


VOID WINAPI service_main(DWORD dwArgc, LPTSTR *lpszArgv)
{
    debug("Called service main.\n");
    sshStatusHandle = RegisterServiceCtrlHandler( SZSERVICENAME, service_ctrl);
    if (sshStatusHandle)
    {

        ssStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        ssStatus.dwServiceSpecificExitCode = 0;

        if (ReportStatusToSCMgr(SERVICE_START_PENDING,NO_ERROR,3000))
            ServiceStart(NULL);

        ReportStatusToSCMgr(SERVICE_STOPPED,dwErr,0);
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
                CmdInstallService();
            }
            else if (!strcmp("remove", args[1]+1))
            {
                CmdRemoveService();
            }
            else
            {
                goto dispatch;
            }
            return 0;
        }
    }
    else
    {
        SC_HANDLE   schService = NULL;
        SC_HANDLE   schSCManager = NULL;
        SERVICE_STATUS          ssStatus;

        StartServiceCtrlDispatcher(dispatchTable);
        schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
        if (schSCManager)
        {
            schService = OpenService(schSCManager, SZSERVICENAME, SERVICE_ALL_ACCESS);
            if (schService)
            {
                if (StartService(schService, 0, NULL))
                {
                    debug("Starting %S.\n", SZSERVICENAME);
                    Sleep(1000);
                    while(QueryServiceStatus(schService, &ssStatus))
                    {
                        if (ssStatus.dwCurrentState == SERVICE_START_PENDING)
                            Sleep(1000);
                        else
                            break;
                    }
                    if (ssStatus.dwCurrentState == SERVICE_START)
                    {
                        debug("\n%S start.\n", SZSERVICENAME);
                    }
                    else
                    {
                        debug("\n%S failed to start.\n", SZSERVICENAME);
                    }
                }
                else
                {
                    debug("\n%S failed to start.\n", SZSERVICENAME);
                }
            }
            else
            {
                debug ("Failed to load the service... \n");
                return 1;
            }
        }

        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return 0;
    }

dispatch:
    fprintf(stdout,
        "Usage\n"
        "-install\t\tinstall the service\n"
        "-remove \t\tremove the service\n");
    return 1;
}
