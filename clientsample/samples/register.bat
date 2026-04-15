@echo off
:: Register a new account on the shadNet server.
::
:: Usage:
::   register.bat <npid> <password> <email> [secretKey]
::
:: secretKey is optional. If the server has RegistrationSecretKey set in its
:: config, you must supply the matching key or the registration will be rejected.
::
:: Examples:
::   register.bat Shadow 1234 shadow@example.com
::   register.bat Shadow 1234 shadow@example.com MySecretKey

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

if "%~3"=="" (
    echo Usage: register.bat ^<npid^> ^<password^> ^<email^> [secretKey]
    echo.
    echo Examples:
    echo   register.bat Shadow 1234 shadow@example.com
    echo   register.bat Shadow 1234 shadow@example.com MySecretKey
    exit /b 1
)

set NPID=%~1
set PASSWORD=%~2
set EMAIL=%~3
set SECRET=%~4

echo [register] npid=%NPID%  email=%EMAIL%
echo.

if "%SECRET%"=="" (
    "%EXE%" %HOST% %PORT% register "%NPID%" "%PASSWORD%" "%EMAIL%"
) else (
    "%EXE%" %HOST% %PORT% register "%NPID%" "%PASSWORD%" "%EMAIL%" "%SECRET%"
)

echo.
echo Exit code: %ERRORLEVEL%
endlocal
pause
