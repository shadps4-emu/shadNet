@echo off
:: Log in and print the current friend lists for an account.
::
:: Usage:
::   login.bat <npid> <password> [token]
::
:: Example:
::   login.bat shadow 1234
::   login.bat shadow 1234 MYTOKEN123   (with email validation token unsupported)

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

if "%~2"=="" (
    echo Usage: login.bat ^<npid^> ^<password^> [token]
    exit /b 1
)

set NPID=%~1
set PASSWORD=%~2
set TOKEN=%~3

echo [login] npid=%NPID%
echo.

if "%TOKEN%"=="" (
    "%EXE%" %HOST% %PORT% login "%NPID%" "%PASSWORD%"
) else (
    "%EXE%" %HOST% %PORT% login "%NPID%" "%PASSWORD%" "%TOKEN%"
)

echo.
echo Exit code: %ERRORLEVEL%
endlocal
pause
