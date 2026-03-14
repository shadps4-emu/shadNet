@echo off
:: Unblock a previously blocked user.
::
:: Removing a block does not automatically restore the friendship — the friend
:: request process must be started again from scratch.
::
:: Usage:
::   block-remove.bat <my_npid> <my_password> <target_npid>
::
:: Example:
::   block-remove.bat Shadow 1234 Stephen

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

if "%~3"=="" (
    echo Usage: block-remove.bat ^<my_npid^> ^<my_password^> ^<target_npid^>
    exit /b 1
)

set NPID=%~1
set PASSWORD=%~2
set TARGET=%~3

echo [block-remove] %NPID% unblocks %TARGET%
echo.

"%EXE%" %HOST% %PORT% block-remove "%NPID%" "%PASSWORD%" "%TARGET%"

echo.
echo Exit code: %ERRORLEVEL%
endlocal
pause
