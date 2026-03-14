@echo off
:: Block a user.
::
:: If the target was a friend, the friendship is simultaneously severed and
:: both sides receive a FriendLost notification.
:: Blocked users cannot send you friend requests.
::
:: Usage:
::   block-add.bat <my_npid> <my_password> <target_npid>
::
:: Example:
::   block-add.bat Shadow 1234 Stephen

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

if "%~3"=="" (
    echo Usage: block-add.bat ^<my_npid^> ^<my_password^> ^<target_npid^>
    exit /b 1
)

set NPID=%~1
set PASSWORD=%~2
set TARGET=%~3

echo [block-add] %NPID% blocks %TARGET%
echo.

"%EXE%" %HOST% %PORT% block-add "%NPID%" "%PASSWORD%" "%TARGET%"

echo.
echo Exit code: %ERRORLEVEL%
endlocal
