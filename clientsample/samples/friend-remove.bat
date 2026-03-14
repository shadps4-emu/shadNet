@echo off
:: Remove a friend or cancel a pending friend request.
::
:: Both parties receive a FriendLost notification.
::
:: Usage:
::   friend-remove.bat <my_npid> <my_password> <target_npid>
::
:: Example:
::   friend-remove.bat Shadow 1234 Stephen

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=..\rpcn-sample.exe

if "%~3"=="" (
    echo Usage: friend-remove.bat ^<my_npid^> ^<my_password^> ^<target_npid^>
    exit /b 1
)

set NPID=%~1
set PASSWORD=%~2
set TARGET=%~3

echo [friend-remove] %NPID% removes %TARGET%
echo.

"%EXE%" %HOST% %PORT% friend-remove "%NPID%" "%PASSWORD%" "%TARGET%"

echo.
echo Exit code: %ERRORLEVEL%
endlocal
pause
