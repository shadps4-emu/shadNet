@echo off
:: Send a friend request to another user, or accept a pending incoming request.
::
:: If the target has already sent you a request, this command completes the
:: mutual friendship and both sides receive a FriendNew notification.
:: If no request exists yet, a FriendQuery notification is sent to the target.
::
:: Usage:
::   friend-add.bat <my_npid> <my_password> <target_npid>
::
:: Examples:
::   friend-add.bat Shadow 1234 Stephen     <- Shadow sends Stephen a request
::   friend-add.bat Stephen 12345   Shadow   <- Stephen accepts (mutual friendship formed)

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

if "%~3"=="" (
    echo Usage: friend-add.bat ^<my_npid^> ^<my_password^> ^<target_npid^>
    exit /b 1
)

set NPID=%~1
set PASSWORD=%~2
set TARGET=%~3

echo [friend-add] %NPID% -^> %TARGET%
echo.

"%EXE%" %HOST% %PORT% friend-add "%NPID%" "%PASSWORD%" "%TARGET%"

echo.
echo Exit code: %ERRORLEVEL%
endlocal
