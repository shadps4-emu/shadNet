@echo off
:: Query the configuration of a specific leaderboard.
::
:: Usage:
::   score-board.bat <npid> <password> <comId> <boardId>
::
:: Example:
::   score-board.bat Shadow 1234 NPWR12345_00 1

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

if "%~4"=="" (
    echo Usage: score-board.bat ^<npid^> ^<password^> ^<comId^> ^<boardId^>
    exit /b 1
)

echo [score-board] %~1 querying board %~4 on %~3
echo.

"%EXE%" %HOST% %PORT% score-board "%~1" "%~2" "%~3" "%~4"

echo.
echo Exit code: %ERRORLEVEL%
endlocal
