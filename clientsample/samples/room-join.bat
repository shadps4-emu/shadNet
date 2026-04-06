@echo off
:: Join an existing room by ID.
:: Get the room ID from room-create.bat output or room-list.bat.
::
:: Usage: room-join.bat <npid> <password> <roomId>

setlocal
set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

if "%~3"=="" ( echo Usage: room-join.bat ^<npid^> ^<password^> ^<roomId^> & exit /b 1 )

echo [room-join] %~1 joining room %~3
echo.

"%EXE%" %HOST% %PORT% room-join "%~1" "%~2" "%~3"

echo.
echo Exit code: %ERRORLEVEL%
endlocal

pause