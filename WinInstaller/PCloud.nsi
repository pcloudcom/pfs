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
	${ElseIf} ${IsWin8}
    ${Else}
      MessageBox MB_OK "Your OS is not supported. pCloud supports Windows 2003, Vista, 2008, 2008R2, Win7 and 8 for x64."
      Abort
    ${EndIf}
  ${Else}
    ${If} ${IsWinXP}
    ${ElseIf} ${IsWin2003}
    ${ElseIf} ${IsWinVista}
    ${ElseIf} ${IsWin2008}
    ${ElseIf} ${IsWin7}
	${ElseIf} ${IsWin8}
    ${Else}
      MessageBox MB_OK "Your OS is not supported. pCloud supports Windows XP, 2003, Vista, 2008, Win7 and 8  for x86."
      Abort
    ${EndIf}
  ${EndIf}
FunctionEnd


Section "Install"
  SetOutPath $INSTDIR
  ${If} ${IsWinXP}
    IfFileExists $INSTDIR\win_service-xp.exe Installed
  ${Else}
    IfFileExists $INSTDIR\win_service.exe Installed
  ${EndIf}
  
  WriteRegStr HKLM "SOFTWARE\PCloud\pCloud" "Install_Dir" "$INSTDIR"

  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "DisplayName" "PCloud Service"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "UninstallString" '"$INSTDIR\pfs-uninst.exe"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud" "NoRepair" 1

!ifndef INNER
  File "$%TEMP%\pfs-uninst.exe"
!endif
  
  File libeay32.dll
  File libgcc_s_dw2-1.dll
  File libssl32.dll
  File libstdc++-6.dll
  File libwinpthread-1.dll
  File pthreadGC2.dll
  File pthreadGCE2.dll
  File ssleay32.dll
  
  ${If} ${IsWinXP}
    File .\XP\VCRedist.exe
	File .\XP\fuse4Win-xp.dll
	File .\XP\win_service-xp.exe
    MessageBox MB_OK|MB_ICONINFORMATION "The redistributable package will be installed. It could take several minutes. Please be patient."
    nsExec::Exec '"$INSTDIR\VCRedist.exe" /q'
  ${Else}
    File fuse4Win.dll
	File win_service.exe
  ${EndIf}
  
  File DokanInstall.exe
  File pCloud.exe
  
  ClearErrors
  nsExec::Exec '"$INSTDIR\DokanInstall.exe" /S'
  IfErrors 0 noError
    MessageBox MB_OK|MB_ICONEXCLAMATION "There is a problem installing Dokan driver."
    Quit
  noError:
  
  ${If} ${IsWinXP}
    nsExec::Exec '"$INSTDIR\win_service-xp.exe" -install -run'
  ${Else}
    nsExec::Exec '"$INSTDIR\win_service.exe" -install -run'
  ${EndIf}
  
  Delete  "$INSTDIR\DokanInstall.exe"
  
  CreateDirectory "$SMPROGRAMS\PCloud"
  CreateShortCut "$SMPROGRAMS\PCloud\pCloud.lnk" "$INSTDIR\pCloud.exe" "" ""
  CreateShortCut "$DESKTOP\pCloud.lnk" "$INSTDIR\pCloud.exe" "" ""
  CreateShortCut "$SMPROGRAMS\PCloud\uninstall.lnk" "$INSTDIR\pfs-uninst.exe" "" ""

  MessageBox MB_YESNO|MB_ICONQUESTION "Do you want to run pCloud automatically when Windows starts?" IDNO NoStartup
	WriteRegStr "HKLM" "SOFTWARE\Microsoft\Windows\CurrentVersion\Run" "pCloud" "$INSTDIR\pCloud.exe"
  NoStartup:
  
  
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

  DeleteRegKey   HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PCloud"
  DeleteRegKey   HKCU "SOFTWARE\PCloud"
  DeleteRegKey 	 HKLM "SOFTWARE\PCloud"
  DeleteRegValue HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Run" "pCloud"

  
  ${If} ${IsWinXP}
    nsExec::Exec '"$INSTDIR\win_service-xp.exe" -remove'
	MessageBox MB_OK|MB_ICONINFORMATION "The redistributable package will be removed. It could take several minutes. Please be patient."
    nsExec::Exec '"$INSTDIR\VCRedist.exe" /qu'
  ${Else}
    nsExec::Exec '"$INSTDIR\win_service.exe" -remove'
  ${EndIf}
    
  Delete "$INSTDIR\*.*"
    
  RMDir "$INSTDIR"
  Exec '"$PROGRAMFILES\Dokan\DokanLibrary\DokanUninstall.exe" /S'

NoStop:
  Quit
SectionEnd
!endif
