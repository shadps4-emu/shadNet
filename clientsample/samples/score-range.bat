@echo off
:: Fetch a range of leaderboard entries by rank position.
::
:: Returns entries startRank through startRank+numRanks-1 (1-based).
:: Prints each entry's rank, NP ID, score, and pcId.
::
:: Usage:
::   score-range.bat <npid> <password> <comId> <boardId> <startRank> <numRanks>
::
:: Examples:
::   score-range.bat Shadow 1234 NPWR12345_00 1 1 10    <- top 10
::   score-range.bat Shadow 1234 NPWR12345_00 1 11 10   <- positions 11-20

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

if "%~6"=="" (
    echo Usage: score-range.bat ^<npid^> ^<password^> ^<comId^> ^<boardId^> ^<startRank^> ^<numRanks^>
    exit /b 1
)

echo [score-range] board=%~4  ranks %~5 to %~6
echo.

"%EXE%" %HOST% %PORT% score-range "%~1" "%~2" "%~3" "%~4" "%~5" "%~6"

echo.
echo Exit code: %ERRORLEVEL%
endlocal
