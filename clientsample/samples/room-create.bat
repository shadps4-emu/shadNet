@echo off
:: Create a new matchmaking room and print the room ID.
:: Run this in one terminal, then room-join.bat in another.
::
:: Usage: room-create.bat <npid> <password> [maxSlots]

setlocal
set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

if "%~2"=="" ( echo Usage: room-create.bat ^<npid^> ^<password^> [maxSlots] & exit /b 1 )

set SLOTS=4
if not "%~3"=="" set SLOTS=%~3

echo [room-create] %~1 creating room with %SLOTS% slots
echo.

"%EXE%" %HOST% %PORT% room-create "%~1" "%~2" %SLOTS%

echo.
echo Exit code: %ERRORLEVEL%
endlocal

pause
