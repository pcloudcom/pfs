Name "pCloud"

InstallDir $PROGRAMFILES\PCloud
RequestExecutionLevel admin
Caption "pCloud FS Installer"
InstallDirRegKey HKCU "Software\pCloud\Install" "Install_Dir"
LicenseText "Terms of service: "
LicenseData "license.rtf"

ShowInstDetails nevershow
ShowUninstDetails nevershow

;--------------------------------

; Pages
Page license
Page directory
Page instfiles

UninstPage uninstConfirm
UninstPage instfiles
;--------------------------------

!include nsProcess.nsh
!include LogicLib.nsh
!include x64.nsh
!include WinVer.nsh

!ifdef INNER
  OutFile "inst.exe"
!else
  !system "$\"${NSISDIR}\makensis$\" /DINNER PCloud.nsi" = 0
  !system "inst.exe" = 2

  !system 'sign.bat "$%TEMP%\pfs-uninst.exe"' = 0

  OutFile "pCloudInstall.exe"
!endif

Function .onInit
!ifdef INNER
  WriteUninstaller "$%TEMP%\pfs-uninst.exe"
  Quit
!endif

  IntOp $0 ${SF_SELECTED} | ${SF_RO}

  ${If} ${RunningX64}
    ${If} ${IsWin2003}
    ${ElseIf} ${IsWinVista}
    ${ElseIf} ${IsWin2008}
    ${ElseIf} ${IsWin2008R2}
    ${ElseIf} ${IsWin7}
    ${Else}
      MessageBox MB_OK "Your OS is not supported. pCloud supports Windows 2003, Vista, 2008, 2008R2 and 7 for x64."
      Abort
    ${EndIf}
  ${Else}
    ${If} ${IsWinXP}
    ${ElseIf} ${IsWin2003}
    ${ElseIf} ${IsWinVista}
    ${ElseIf} ${IsWin2008}
    ${ElseIf} ${IsWin7}
    ${Else}
      MessageBox MB_OK "Your OS is not supported. pCloud supports Windows XP, 2003, Vista, 2008 and 7 for x86."
      Abort
    ${EndIf}
  ${EndIf}
FunctionEnd


Section "Install"
  SetOutPath $INSTDIR
  
  IfFileExists $INSTDIR\win_service.exe Installed

  WriteRegStr HKCU "SOFTWARE\PCloud\pCloud" "Install_Dir" "$INSTDIR"

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
  
  ${If} ${IsWinXP}
    File VCRedist.exe
    MessageBox MB_OK|MB_ICONINFORMATION "The redistributable package will be installed. It could take several minutes. Please be patient."
    nsExec::Exec '"$INSTDIR\VCRedist.exe" /q'
  ${Else}
    File CreateTask.bat
    File start.xml
    File end.xml
  ${EndIf}
  
  File DokanInstall.exe
  File win_service.exe
  File pCloud.exe
  
  ClearErrors
  nsExec::Exec '"$INSTDIR\DokanInstall.exe" /S'
  IfErrors 0 noError
    MessageBox MB_OK|MB_ICONEXCLAMATION "There is a problem installing Dokan driver."
    Quit
  noError:
  
  nsExec::Exec '"$INSTDIR\start.bat" "$INSTDIR"'
  
  Delete  "$INSTDIR\DokanInstall.exe"
  
  CreateDirectory "$SMPROGRAMS\PCloud"
  CreateShortCut "$SMPROGRAMS\PCloud\pCloud.lnk" "$INSTDIR\pCloud.exe" "" ""
  CreateShortCut "$DESKTOP\pCloud.lnk" "$INSTDIR\pCloud.exe" "" ""
  CreateShortCut "$SMPROGRAMS\PCloud\uninstall.lnk" "$INSTDIR\pfs-uninst.exe" "" ""

  ${If} ${IsWinXP}
    MessageBox MB_YESNO|MB_ICONQUESTION "Do you want to run pCloud automatically when Windows starts?" IDNO NoStartupXp
      CreateShortCut "$SMSTARTUP\pCloud.lnk" "$INSTDIR\pCloud.exe" "" ""
    NoStartupXp:
  ${Else}
    MessageBox MB_YESNO|MB_ICONQUESTION "Do you want to run pCloud automatically when Windows starts?" IDNO NoStartup
      nsExec::Exec '"$INSTDIR\CreateTask.bat" "$INSTDIR\pCloud.exe"'
    NoStartup:
    Delete start.xml
    Delete end.xml
    Delete CreateTask.bat
  ${EndIf}
  
  
  MessageBox MB_YESNO|MB_ICONQUESTION "A computer restart is required. Do you want to restart now?" IDNO NoReboot
    Reboot
  NoReboot:
  Quit
  
  Installed:
    MessageBox MB_YESNO|MB_ICONQUESTION "There is a PCloud already installed on your system. Uninstall old version now?" IDNO NoReboot
  nsExec::Exec '"$INSTDIR\pfs-uninst.exe"'
  WriteRegStr "HKLM" "SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce" "Installer" $EXEPATH
  Quit

SectionEnd ; end the section


UninstallText "This will uninstall PCloud. Hit next to continue."

!ifdef INNER
Section "Uninstall"

  ${nsProcess::FindProcess} "pCloud.exe" $R0
  StrCmp $R0 0 0 +2
  MessageBox MB_YESNO|MB_ICONEXCLAMATION 'The pCloud application is running. It will be closed.' IDNO NoStop
  ${nsProcess::KillProcess} "pCloud.exe" $R0

  Delete "$SMPROGRAMS\PCloud\pCloud.lnk"
  Delete "$DESKTOP\pCloud.lnk"
  Delete "$SMPROGRAMS\PCloud\uninstall.lnk"
  RMDir "$SMPROGRAMS\PCloud"

  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud"
  DeleteRegKey HKCU "SOFTWARE\PCloud"

  nsExec::Exec '"$INSTDIR\stop.bat" "$INSTDIR"'
  ${If} ${IsWinXP}
    Delete "$SMSTARTUP\pCloud.lnk"
  ${Else}
    nsExec::Exec 'schtasks /delete /tn "PCloud" /F'
  ${EndIf}
  
  ${If} ${IsWinXP}
    MessageBox MB_OK|MB_ICONINFORMATION "The redistributable package will be removed. It could take several minutes. Please be patient."
    nsExec::Exec '"$INSTDIR\VCRedist.exe" /qu'
  ${EndIf}
  
  Delete "$INSTDIR\*.*"
    
  RMDir "$INSTDIR"
  Exec '"$PROGRAMFILES\Dokan\DokanLibrary\DokanUninstall.exe" /S'
  
  
NoStop:
  Quit
SectionEnd
!endif
