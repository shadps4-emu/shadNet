@echo off
:: Submit a score to a leaderboard.
::
:: The server applies update_mode rules automatically:
::   NORMAL_UPDATE — only stores the score if it is better than the existing one
::   FORCE_UPDATE  — always overwrites regardless of value
::
:: The reply prints the 1-based rank achieved.
:: If the score does not make the board (worse than all existing entries)
:: the server returns ScoreNotBest.
::
:: Usage:
::   score-record.bat <npid> <password> <comId> <boardId> <pcId> <score> [comment]
::
:: pcId   — character/slot identifier; use 0 for single-character games
:: score  — integer; higher is better for DESCENDING boards (most games)
::           lower is better for ASCENDING boards (time trial / speedrun)
::
:: Examples:
::   score-record.bat Shadow 1234 NPWR12345_00 1 0 98500
::   score-record.bat Shadow 1234 NPWR12345_00 1 0 98500 "Perfect run"

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

if "%~6"=="" (
    echo Usage: score-record.bat ^<npid^> ^<password^> ^<comId^> ^<boardId^> ^<pcId^> ^<score^> [comment]
    exit /b 1
)

set NPID=%~1
set PASSWORD=%~2
set COMID=%~3
set BOARD=%~4
set PCID=%~5
set SCORE=%~6
set COMMENT=%~7

if "%COMMENT%"=="" (
    echo [score-record] %NPID%  board=%BOARD%  pcId=%PCID%  score=%SCORE%
) else (
    echo [score-record] %NPID%  board=%BOARD%  pcId=%PCID%  score=%SCORE%  comment="%COMMENT%"
)
echo.

if "%COMMENT%"=="" (
    "%EXE%" %HOST% %PORT% score-record "%NPID%" "%PASSWORD%" "%COMID%" "%BOARD%" "%PCID%" "%SCORE%"
) else (
    "%EXE%" %HOST% %PORT% score-record "%NPID%" "%PASSWORD%" "%COMID%" "%BOARD%" "%PCID%" "%SCORE%" "%COMMENT%"
)

echo.
echo Exit code: %ERRORLEVEL%
endlocal

pause
