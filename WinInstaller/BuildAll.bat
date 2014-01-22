copy ..\BuildWin\bin\Service\fuse4Win.dll . /Y
copy ..\BuildWin\bin\Service\win_service.exe . /Y

copy ..\BuildWin\bin\Service\fuse4Win-xp.dll .\XP /Y
copy ..\BuildWin\bin\Service\win_service-xp.exe .\XP /Y

copy ..\..\pfs-gui\build-pCloud-qt_static-Release\release\pCloud.exe . /Y

call sign.bat pCloud.exe
call sign.bat fuse4Win.dll
call sign.bat win_service.exe

call sign.bat .\XP\fuse4Win-xp.dll
call sign.bat .\XP\win_service-xp.exe


"C:\Program Files (x86)\NSIS\makensis.exe" PCloud.nsi
call sign.bat PCloudInstall.exe

del inst.exe

pause