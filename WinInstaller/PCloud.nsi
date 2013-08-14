; The name of the installer
Name "PCloud"

; The file to write
OutFile "PCloudInstall.exe"

InstallDir $PROGRAMFILES\PCloud
RequestExecutionLevel admin
Caption "PCloud FS Installer"
InstallDirRegKey HKLM "Software\PCloud\Install" "Install_Dir"
LicenseText "You agree with everithing?"
LicenseData "license.txt"

;--------------------------------

; Pages
Page license
Page directory
Page instfiles

UninstPage uninstConfirm
UninstPage instfiles

;--------------------------------

Section "Install"

  SetOutPath $INSTDIR
  WriteRegStr HKLM SOFTWARE\NSISTest\BigNSISTest "Install_Dir" "$INSTDIR"

  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "DisplayName" "PCloud Service"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "UninstallString" '"$INSTDIR\pfs-uninst.exe"'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "NoRepair" 1
  WriteUninstaller "pfs-uninst.exe"
  
  File *.dll
  File *.bat
  File DokanInstall.exe
  File PCloudConfig.exe
  File win_service.exe
  
  ExecWait '"$INSTDIR\DokanInstall.exe"'
  ExecWait '"$INSTDIR\start.bat" "$INSTDIR"'
  ExecWait '"$INSTDIR\PCloudConfig.Exe"'
  
  Delete  "$INSTDIR\DokanInstall.exe"

  createDirectory "$SMPROGRAMS\PCloud"
  createShortCut "$SMPROGRAMS\PCloud\PCloudConfig.lnk" "$INSTDIR\PCloudConfig.exe" "" ""

  
  MessageBox MB_YESNO|MB_ICONQUESTION "A reboot is required. Do you want to reboot now?" IDNO NoReboot
    Reboot
  NoReboot:
  
SectionEnd ; end the section


UninstallText "This will uninstall PCloud. Hit next to continue."

Section "Uninstall"

  Delete "$SMPROGRAMS\PCloud\PCloudConfig.lnk"  
  RMDir "$SMPROGRAMS\PCloud"

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud"
  DeleteRegKey HKLM "SOFTWARE\PCloud"

  ExecWait '"$INSTDIR\stop.bat" "$INSTDIR"'

  Delete "$INSTDIR\*.*"
    
  RMDir "$INSTDIR"
  Exec '"$PROGRAMFILES\Dokan\DokanLibrary\DokanUninstall.exe"'

SectionEnd
