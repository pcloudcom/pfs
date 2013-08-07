// SettingsDlg.cpp : implementation file
//

#include "stdafx.h"
#include "PCloud.h"
#include "SettingsDlg.h"
#include "afxdialogex.h"


// CSettingsDlg dialog

IMPLEMENT_DYNAMIC(CSettingsDlg, CDialogEx)

CSettingsDlg::CSettingsDlg(CWnd* pParent /*=NULL*/)
	: CDialogEx(CSettingsDlg::IDD, pParent)
{
    m_fChecked = false;
}

CSettingsDlg::~CSettingsDlg()
{
}

void CSettingsDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_CHECK1, m_btnUseSSL);
}

bool CSettingsDlg::getSsl()
{
    return m_fChecked;
}

int CSettingsDlg::getCache()
{
    return m_nSize;
}

BEGIN_MESSAGE_MAP(CSettingsDlg, CDialogEx)
    ON_BN_CLICKED(IDOK, &CSettingsDlg::OnBnClickedOk)
END_MESSAGE_MAP()


// CSettingsDlg message handlers


void CSettingsDlg::OnBnClickedOk()
{
    m_fChecked = m_btnUseSSL.GetCheck() == BST_CHECKED;
    WCHAR size[MAX_PATH];
    GetDlgItemText(IDC_EDIT_CACHE, size, MAX_PATH);
    m_nSize = _wtoi(size);
    CDialogEx::OnOK();
}
