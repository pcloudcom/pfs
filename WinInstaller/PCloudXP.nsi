; The name of the installer
Name "pCloud"

InstallDir $PROGRAMFILES\PCloud
RequestExecutionLevel admin
Caption "pCloud FS Installer"
InstallDirRegKey HKCU "Software\pCloud\Install" "Install_Dir"
LicenseText "Terms of service: "
LicenseData "license.rtf"

;--------------------------------

; Pages
Page license
Page directory
Page instfiles

UninstPage uninstConfirm
UninstPage instfiles
;--------------------------------


!ifdef INNER
  OutFile "inst.exe"
!else
  !system "$\"${NSISDIR}\makensis$\" /DINNER PCloudXP.nsi" = 0 
  !system "inst.exe" = 2
 
  !system 'sign.bat "$%TEMP%\pfs-uninst.exe"' = 0

  OutFile "pCloudInstallXP.exe"
!endif

Function .onInit
!ifdef INNER
  WriteUninstaller "$%TEMP%\pfs-uninst.exe"
  Quit
!endif
FunctionEnd
 

Section "Install"
  SetOutPath $INSTDIR
  
  IfFileExists $INSTDIR\win_service.exe Installed

  WriteRegStr HKCU SOFTWARE\NSISTest\BigNSISTest "Install_Dir" "$INSTDIR"

  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "DisplayName" "PCloud Service"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "UninstallString" '"$INSTDIR\pfs-uninst.exe"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "NoRepair" 1

!ifndef INNER
  File "$%TEMP%\pfs-uninst.exe"
!endif
  
  File *.dll
  File start.bat
  File stop.bat
  File restart.bat
  File DokanInstall.exe
  File win_service.exe
  File pCloud.exe
  File VCRedist.exe
  
  ExecWait '"$INSTDIR\VCRedist.exe" /u'
  
  ClearErrors
  ExecWait '"$INSTDIR\DokanInstall.exe" /S'
  IfErrors 0 noError
    MessageBox MB_OK|MB_ICONEXCLAMATION "There is a problem installing Dokan driver."
    Quit
  noError:
  
  ExecWait '"$INSTDIR\start.bat" "$INSTDIR"'
  
  Delete  "$INSTDIR\DokanInstall.exe"
  Delete  "$INSTDIR\VCRedist.exe"
  
  CreateDirectory "$SMPROGRAMS\PCloud"
  CreateShortCut "$SMPROGRAMS\PCloud\pCloud.lnk" "$INSTDIR\pCloud.exe" "" ""
  CreateShortCut "$DESKTOP\pCloud.lnk" "$INSTDIR\pCloud.exe" ""
  CreateShortCut "$SMPROGRAMS\PCloud\uninstall.lnk" "$INSTDIR\pfs-uninst.exe" "" ""

  MessageBox MB_YESNO|MB_ICONQUESTION "Do you want PCloud control application to start with windows?" IDNO NoStartup
    CreateShortCut "$SMSTARTUP\pCloud.lnk" "$INSTDIR\pCloud.exe" "" ""
  NoStartup:
  
  MessageBox MB_YESNO|MB_ICONQUESTION "A reboot is required. Do you want to reboot now?" IDNO NoReboot
    Reboot
  NoReboot:
  Quit
  
  Installed:
    MessageBox MB_OK|MB_ICONEXCLAMATION "There is a PCloud already installed on your system. Please uninstall old version first."
  
SectionEnd ; end the section


UninstallText "This will uninstall pCloud. Hit next to continue."

!ifdef INNER
Section "Uninstall"

  !include nsProcess.nsh
  ${nsProcess::FindProcess} "pCloud.exe" $R0
  StrCmp $R0 0 0 +2
  MessageBox MB_YESNO|MB_ICONEXCLAMATION 'The pCloud application is running. It will be closed.' IDNO NoStop
  ${nsProcess::KillProcess} "pCloud.exe" $R0

  Delete "$SMPROGRAMS\PCloud\pCloud.lnk"
  Delete "$DESKTOP\pCloud.lnk"
  Delete "$SMPROGRAMS\PCloud\uninstall.lnk"
  Delete "$SMSTARTUP\pCloud.lnk"
  RMDir "$SMPROGRAMS\PCloud"

  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud"
  DeleteRegKey HKCU "SOFTWARE\PCloud"

  ExecWait '"$INSTDIR\stop.bat" "$INSTDIR"'
  
  Delete "$INSTDIR\*.*"
    
  RMDir "$INSTDIR"
  Exec '"$PROGRAMFILES\Dokan\DokanLibrary\DokanUninstall.exe" /S'
  
NoStop:
  Quit
SectionEnd
!endif
