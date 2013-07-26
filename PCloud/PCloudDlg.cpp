// PCloudDlg.cpp : implementation file
//


#include "stdafx.h"
#include "PCloud.h"
#include "PCloudDlg.h"
#include "afxdialogex.h"


#ifdef _DEBUG
#define new DEBUG_NEW
#endif


static bool readKey(LPCSTR key, LPSTR val)
{
    HRESULT hr;
    DWORD cbDataSize = MAX_PATH;
    HKEY hKey;
    hr = RegOpenKeyEx(HKEY_LOCAL_MACHINE, REGISTRY_KEY_PCLOUD, 0, KEY_READ, &hKey);
    if (!hr)
    {
        hr = RegQueryValueExA(hKey, key, NULL, NULL, (LPBYTE)val, &cbDataSize);
        RegCloseKey(hKey);
        return !hr;
    }
    return false;
}


static bool storeKey(LPCSTR key, const char * val)
{
    HRESULT hr;
    HKEY hKey;
    hr = RegCreateKeyEx(HKEY_LOCAL_MACHINE, REGISTRY_KEY_PCLOUD, 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &hKey, NULL);
    if (!hr)
    {
        hr = RegSetValueExA(hKey, key, 0, REG_SZ, (LPBYTE)val, strlen(val)+1);
        RegCloseKey(hKey);
        return hr == 0;
    }
    return false;
}


bool CPCloudDlg::setDataToRegistry(LPWSTR username, LPWSTR pass)
{
    char mbUser[2*MAX_PATH];
    char mbPass[2*MAX_PATH];
    wcstombs(mbUser, username, sizeof(mbUser));
    wcstombs(mbPass, pass, sizeof(mbPass));
    return (storeKey("username", mbUser) && storeKey("pass", mbPass));
}


void CPCloudDlg::restartService()
{
    SC_HANDLE       schService;
    SERVICE_STATUS  ssStatus;
    SC_HANDLE       schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

    if (!schSCManager) return;
    schService = OpenService(schSCManager, SZSERVICENAME, SERVICE_ALL_ACCESS);
    if (schService)
    {
        ControlService(schService, SERVICE_CONTROL_STOP, &ssStatus);
        int retry = 5;
        while(QueryServiceStatus(schService, &ssStatus) && retry)
        {
            if (ssStatus.dwCurrentState == SERVICE_STOPPED)
                break;
            Sleep(1000);
            --retry;
        }

        if (StartService(schService, 0, NULL))
        {
            Sleep(1000);
            int retry = 5;
            while(QueryServiceStatus(schService, &ssStatus) && retry)
            {
                --retry;
                if (ssStatus.dwCurrentState == SERVICE_START_PENDING)
                    Sleep(1000);
                else break;
            }
        }
        Sleep(1000);
        char val[32] = {0,};
        if (readKey("lr", val))
        {
            if (val[0] == '1')
                MessageBox(L"Failed to connecto to the PCloud. Check your connection,", L"Network Error", MB_ICONEXCLAMATION);
            else if (val[0] == '2')
                MessageBox(L"Failed to login!");
            storeKey("lr", "");
        }
    }
}


CPCloudDlg::CPCloudDlg(CWnd* pParent /*=NULL*/)
	: CDialogEx(CPCloudDlg::IDD, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CPCloudDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CPCloudDlg, CDialogEx)
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
    ON_BN_CLICKED(IDCANCEL, &CPCloudDlg::OnBnClickedCancel)
    ON_BN_CLICKED(IDOK, &CPCloudDlg::OnBnClickedOk)
    ON_NOTIFY(NM_CLICK, IDC_SYSLINK1, &CPCloudDlg::OnNMClickSyslink1)
END_MESSAGE_MAP()


// CPCloudDlg message handlers

BOOL CPCloudDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	// Set the icon for this dialog.  The framework does this automatically
	//  when the application's main window is not a dialog
	SetIcon(m_hIcon, TRUE);			// Set big icon
	SetIcon(m_hIcon, FALSE);		// Set small icon

    char mbUser[MAX_PATH];
    WCHAR username[MAX_PATH];
    if (readKey("username", mbUser))
    {
        mbstowcs(username, mbUser, MAX_PATH);
        SetDlgItemText(IDC_EDIT_UN, username);
    }
    
    return TRUE;  // return TRUE  unless you set the focus to a control
}

// If you add a minimize button to your dialog, you will need the code below
//  to draw the icon.  For MFC applications using the document/view model,
//  this is automatically done for you by the framework.

void CPCloudDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this); // device context for painting

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		// Center icon in client rectangle
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		// Draw the icon
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialogEx::OnPaint();
	}
}

// The system calls this function to obtain the cursor to display while the user drags
//  the minimized window.
HCURSOR CPCloudDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}


void CPCloudDlg::OnBnClickedCancel()
{
    // TODO: Add your control notification handler code here
    CDialogEx::OnCancel();
}


void CPCloudDlg::OnBnClickedOk()
{
    WCHAR username[MAX_PATH];
    WCHAR password[MAX_PATH];
    GetDlgItemText(IDC_EDIT_UN, username, MAX_PATH);
    GetDlgItemText(IDC_EDIT_PASS, password, MAX_PATH);
    if (setDataToRegistry(username, password))
    {
        if (MessageBox(L"Reconnect PCloud?", L"Reconnect", MB_YESNO) == IDYES)
        {
            restartService();
        }
    }
    else
    {
        MessageBox(L"Failed to save password", L"Error", MB_ICONSTOP);
    }
    CDialogEx::OnOK();
}

void CPCloudDlg::OnNMClickSyslink1(NMHDR *pNMHDR, LRESULT *pResult)
{
    PNMLINK pNMLink = (PNMLINK)pNMHDR;
    ::ShellExecute(m_hWnd, L"open", pNMLink->item.szUrl, NULL, NULL, SW_SHOWMAXIMIZED);
    *pResult = 0;
}
