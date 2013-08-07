
// PCloudDlg.h : header file
//

#pragma once

#define REGISTRY_KEY_PCLOUD    L"SOFTWARE\\PCloud\\pfs"
#define SZSERVICENAME          L"pfs"


// CPCloudDlg dialog
class CPCloudDlg : public CDialogEx
{
// Construction
public:
	CPCloudDlg(CWnd* pParent = NULL);	// standard constructor

// Dialog Data
	enum { IDD = IDD_PCLOUD_DIALOG };

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);	// DDX/DDV support


// Implementation
protected:
	HICON m_hIcon;

	// Generated message map functions
	virtual BOOL OnInitDialog();
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	DECLARE_MESSAGE_MAP()
public:
    afx_msg void OnBnClickedCancel();
    afx_msg void OnBnClickedOk();
    afx_msg void OnNMClickSyslink1(NMHDR *pNMHDR, LRESULT *pResult);
    afx_msg LRESULT OnServiceNotify(WPARAM wParam, LPARAM lParam);

private:
    bool setDataToRegistry(LPWSTR username, LPWSTR pass);
    void restartService();

    bool m_fUseSsl;
    int m_nCacheSize;
public:
    afx_msg void OnBnClickedSettings();
};
