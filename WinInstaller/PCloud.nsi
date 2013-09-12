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
  
  IfFileExists $INSTDIR\win_service.exe Installed

  WriteRegStr HKCU SOFTWARE\NSISTest\BigNSISTest "Install_Dir" "$INSTDIR"

  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "DisplayName" "PCloud Service"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "UninstallString" '"$INSTDIR\pfs-uninst.exe"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "NoRepair" 1
  WriteUninstaller "pfs-uninst.exe"
  
  File *.dll
  File *.bat
  File start.xml
  File end.xml
  File DokanInstall.exe
  File win_service.exe
  File pCloud.exe
  
  ClearErrors
  ExecWait '"$INSTDIR\DokanInstall.exe" /S'
  IfErrors 0 noError
    MessageBox MB_OK|MB_ICONEXCLAMATION "There is a problem installing Dokan driver."
    Quit
  noError:
  
  
  ExecWait '"$INSTDIR\start.bat" "$INSTDIR"'
  
  Delete  "$INSTDIR\DokanInstall.exe"

  CreateDirectory "$SMPROGRAMS\PCloud"
  CreateShortCut "$SMPROGRAMS\PCloud\pCloud.lnk" "$INSTDIR\pCloud.exe" "" ""
  CreateShortCut "$DESKTOP\pCloud.lnk" "$INSTDIR\pCloud.exe" "" ""
  CreateShortCut "$SMPROGRAMS\PCloud\uninstall.lnk" "$INSTDIR\pfs-uninst.exe" "" ""

  MessageBox MB_YESNO|MB_ICONQUESTION "Do you want PCloud control application to start with windows?" IDNO NoStartup
    ExecWait '"$INSTDIR\CreateTask.bat" "$INSTDIR\pCloud.exe"'
    ;WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "PCloud" "$INSTDIR\pCloud.exe"
  NoStartup:
  
  Delete start.xml
  Delete end.xml
  Delete CreateTask.bat
  
  MessageBox MB_YESNO|MB_ICONQUESTION "A reboot is required. Do you want to reboot now?" IDNO NoReboot
    Reboot
  NoReboot:
  Quit
  
  Installed:
    MessageBox MB_OK|MB_ICONEXCLAMATION "There is a PCloud already installed on your system. Please uninstall old version first."
  
SectionEnd ; end the section


UninstallText "This will uninstall PCloud. Hit next to continue."


Section "Uninstall"

  !include nsProcess.nsh
  ${nsProcess::FindProcess} "pCloud.exe" $R0
  StrCmp $R0 0 0 +2
  MessageBox MB_OK|MB_ICONEXCLAMATION 'The pCloud is running. It will be closed.' IDOK
  ${nsProcess::KillProcess} "pCloud.exe" $R0

  Delete "$SMPROGRAMS\PCloud\pCloud.lnk"
  Delete "$DESKTOP\pCloud.lnk"
  Delete "$SMPROGRAMS\PCloud\uninstall.lnk"
  RMDir "$SMPROGRAMS\PCloud"

  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud"
  DeleteRegKey HKCU "SOFTWARE\PCloud"

  ExecWait '"$INSTDIR\stop.bat" "$INSTDIR"'
  ExecWait 'schtasks /delete /tn "PCloud" /F'
  
  Delete "$INSTDIR\win_service.exe"
  Delete "$INSTDIR\*.*"
    
  RMDir "$INSTDIR"
  Exec '"$PROGRAMFILES\Dokan\DokanLibrary\DokanUninstall.exe" /S'
  
  Quit
SectionEnd
