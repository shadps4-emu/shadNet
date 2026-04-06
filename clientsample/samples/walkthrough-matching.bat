@echo off
:: Matching walkthrough,demonstrates the full room lifecycle with Shadow and Stephen.
:: Assumes setup-test-accounts.bat has already been run.
::
:: Design: each user is allowed exactly one TCP connection at a time.  The server
:: calls DoLeaveRoom when a connection drops, so the connection IS the room slot.
:: room-create and room-join open an interactive session window that keeps the
:: connection alive.  All subsequent commands for that user (room-list, room-leave)
:: are typed into that same window rather than opening a new process.

setlocal

set HOST=127.0.0.1
set PORT=31313
set EXE=shadnet-sample.exe

echo ============================================================
echo  Step 1: Room list before any rooms exist
echo ============================================================
"%EXE%" %HOST% %PORT% room-list Shadow 1234
echo.

echo ============================================================
echo  Step 2: Shadow creates a 2-slot room
echo    A new window opens.  It stays connected,the connection
echo    IS Shadow's room membership (server removes her on disconnect).
echo ============================================================
start "Shadow session" "%EXE%" %HOST% %PORT% room-create Shadow 1234 2
echo.

echo  Look at the "Shadow session" window for the roomId, then come back here.
set /p ROOM_ID="Enter roomId: "

echo ============================================================
echo  Step 3: Room list,should show Shadow's room
echo    Type this in the SHADOW SESSION window:
echo      room-list
echo ============================================================
echo.
pause

echo ============================================================
echo  Step 4: Stephen joins the room
echo    A new window opens for Stephen.
echo    Shadow's window will show MemberJoined + SignalingHelper.
echo    Both sides get SignalingEvent(ESTABLISHED) after ~2 s.
echo ============================================================
start "Stephen session" "%EXE%" %HOST% %PORT% room-join Stephen 12345 %ROOM_ID%
echo.
echo  (wait a moment for join + delayed ESTABLISHED notification)
timeout /t 3 /nobreak > nul

echo ============================================================
echo  Step 5: Room list,should show room with 2 members
echo    Type this in the STEPHEN SESSION window:
echo      room-list
echo ============================================================
echo.
pause

echo ============================================================
echo  Step 6: Stephen leaves the room
echo    Type this in the STEPHEN SESSION window:
echo      room-leave %ROOM_ID%
echo    Shadow's window will show MemberLeft.
echo    Stephen's window will exit automatically.
echo ============================================================
echo.
pause

echo ============================================================
echo  Step 7: Shadow leaves the room (room destroyed)
echo    Type this in the SHADOW SESSION window:
echo      room-leave %ROOM_ID%
echo    Shadow's window will exit automatically.
echo ============================================================
echo.
pause

echo ============================================================
echo  Step 8: Room list,should be empty again
echo ============================================================
"%EXE%" %HOST% %PORT% room-list Shadow 1234
echo.

echo ============================================================
echo  Walkthrough complete.
echo ============================================================
endlocal

pause
