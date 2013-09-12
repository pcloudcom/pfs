copy ..\BuildWin\bin\Service\fuse4Win.dll . /Y
copy ..\BuildWin\bin\Service\win_service.exe . /Y
copy ..\..\pfs-gui\build-pCloud-qt_static-Release\release\pCloud.exe . /Y

call sign.bat pCloud.exe
call sign.bat win_service.exe

"C:\Program Files (x86)\NSIS\makensis.exe" PCloud.nsi
call sign.bat PCloudInstall.exe

"C:\Program Files (x86)\NSIS\makensis.exe" PCloudXP.nsi
call sign.bat PCloudInstallXP.exe
