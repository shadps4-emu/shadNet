@echo off
:: Leave a room.
::
:: Usage: room-leave.bat <npid> <password> <roomId>

setlocal
set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

if "%~3"=="" ( echo Usage: room-leave.bat ^<npid^> ^<password^> ^<roomId^> & exit /b 1 )

echo [room-leave] %~1 leaving room %~3
echo.

"%EXE%" %HOST% %PORT% room-leave "%~1" "%~2" "%~3"

echo.
echo Exit code: %ERRORLEVEL%
endlocal

pause
