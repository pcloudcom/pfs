; The name of the installer
Name "PCloud"

; The file to write
OutFile "PCloudInstall.exe"

InstallDir $PROGRAMFILES\PCloud
RequestExecutionLevel admin
Caption "PCloud FS Installer"
InstallDirRegKey HKCU "Software\PCloud\Install" "Install_Dir"
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
  WriteRegStr HKCU SOFTWARE\NSISTest\BigNSISTest "Install_Dir" "$INSTDIR"

  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "DisplayName" "PCloud Service"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "UninstallString" '"$INSTDIR\pfs-uninst.exe"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "NoRepair" 1
  WriteUninstaller "pfs-uninst.exe"
  
  File *.dll
  File *.bat
  File DokanInstall.exe
  File win_service.exe
  File pCloud.exe
  
  ExecWait '"$INSTDIR\DokanInstall.exe"'
  ExecWait '"$INSTDIR\start.bat" "$INSTDIR"'
  
  Delete  "$INSTDIR\DokanInstall.exe"

  createDirectory "$SMPROGRAMS\PCloud"
  createShortCut "$SMPROGRAMS\PCloud\pCloud.lnk" "$INSTDIR\pCloud.exe" "" ""
  createShortCut "$SMPROGRAMS\PCloud\uninstall.lnk" "$INSTDIR\pfs-uninst.exe" "" ""

  MessageBox MB_YESNO|MB_ICONQUESTION "Do you want PCloud control application to start with windows?" IDNO NoStartup
    createShortCut "$SMPROGRAMS\Startup\pCloud.lnk" "$INSTDIR\pCloud.exe" "" ""
  NoStartup:
  
  MessageBox MB_YESNO|MB_ICONQUESTION "A reboot is required. Do you want to reboot now?" IDNO NoReboot
    Reboot
  NoReboot:
  
SectionEnd ; end the section


UninstallText "This will uninstall PCloud. Hit next to continue."

Section "Uninstall"

  Delete "$SMPROGRAMS\PCloud\pCloud.lnk"
  Delete "$SMPROGRAMS\Startup\pCloud.lnk"
  Delete "$SMPROGRAMS\PCloud\uninstall.lnk"
  RMDir "$SMPROGRAMS\PCloud"

  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud"
  DeleteRegKey HKCU "SOFTWARE\PCloud"

  ExecWait '"$INSTDIR\stop.bat" "$INSTDIR"'

  Delete "$INSTDIR\*.*"
    
  RMDir "$INSTDIR"
  Exec '"$PROGRAMFILES\Dokan\DokanLibrary\DokanUninstall.exe"'

SectionEnd
