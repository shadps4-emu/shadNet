@echo off
:: List all open rooms on the server.
::
:: Usage: room-list.bat <npid> <password>

setlocal
set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

if "%~2"=="" ( echo Usage: room-list.bat ^<npid^> ^<password^> & exit /b 1 )

echo [room-list] fetching rooms...
echo.

"%EXE%" %HOST% %PORT% room-list "%~1" "%~2"

echo.
echo Exit code: %ERRORLEVEL%
endlocal

pause
