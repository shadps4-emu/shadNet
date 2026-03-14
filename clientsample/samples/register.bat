@echo off
:: Register a new account on the ShadNet server.
::
:: Usage:
::   register.bat <npid> <password> <onlineName> <email>
::
:: Example:
::   register.bat Shadow 1234 "Shadow Something" shadow@example.com

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

if "%~4"=="" (
    echo Usage: register.bat ^<npid^> ^<password^> ^<onlineName^> ^<email^>
    echo.
    echo Example:
    echo   register.bat Shadow 1234 "Shadow Something" shadow@example.com
    exit /b 1
)

set NPID=%~1
set PASSWORD=%~2
set ONLINE_NAME=%~3
set EMAIL=%~4

echo [register] npid=%NPID%  onlineName=%ONLINE_NAME%  email=%EMAIL%
echo.

"%EXE%" %HOST% %PORT% register "%NPID%" "%PASSWORD%" "%ONLINE_NAME%" "%EMAIL%"

echo.
echo Exit code: %ERRORLEVEL%
endlocal
pause
