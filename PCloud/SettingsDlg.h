#pragma once
#include "afxwin.h"


// CSettingsDlg dialog

class CSettingsDlg : public CDialogEx
{
	DECLARE_DYNAMIC(CSettingsDlg)

public:
	CSettingsDlg(CWnd* pParent = NULL);   // standard constructor
	virtual ~CSettingsDlg();

// Dialog Data
	enum { IDD = IDD_DIALOG_SETTINGS };

protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support

	DECLARE_MESSAGE_MAP()

    CButton m_btnUseSSL;
    bool m_fChecked;
    int m_nSize;
public:
    bool getSsl();
    int getCache();
    afx_msg void OnBnClickedOk();
};
