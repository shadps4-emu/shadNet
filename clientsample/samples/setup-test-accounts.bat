@echo off
:: Register two test accounts (Shadow and Stephen) used by the other example scripts.
:: Run this once before trying the friend/block/score examples.
::
:: Accounts created:
::   npid=Shadow     password=1234     email=shadow@test.local
::   npid=Stephen    password=12345    email=stephen@test.local
::
:: If the server requires a registration secret key, pass it as the first argument:
::   setup-test-accounts.bat MySecretKey

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe
set SECRET=%~1

echo [setup] Registering Shadow ...
if "%SECRET%"=="" (
    "%EXE%" %HOST% %PORT% register Shadow 1234 shadow@test.local
) else (
    "%EXE%" %HOST% %PORT% register Shadow 1234 shadow@test.local "%SECRET%"
)
if %ERRORLEVEL% NEQ 0 (
    echo [setup] Shadow registration failed ^(account may already exist^)
)

echo.
echo [setup] Registering Stephen ...
if "%SECRET%"=="" (
    "%EXE%" %HOST% %PORT% register Stephen 12345 stephen@test.local
) else (
    "%EXE%" %HOST% %PORT% register Stephen 12345 stephen@test.local "%SECRET%"
)
if %ERRORLEVEL% NEQ 0 (
    echo [setup] Stephen registration failed ^(account may already exist^)
)

echo.
echo [setup] Done. Run walkthrough-friends.bat to test friend commands.
endlocal
pause
