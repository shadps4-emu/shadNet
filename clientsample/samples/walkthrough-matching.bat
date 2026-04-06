@echo off
:: Matching walkthrough — demonstrates the full room lifecycle with Shadow and Stephen.
:: Assumes setup-test-accounts.bat has already been run.
::
:: Steps:
::   1. Shadow lists rooms (empty)
::   2. Shadow creates a room with 2 slots
::   3. Shadow lists rooms again (sees her own room)
::   4. Stephen joins the room
::      — Server sends MemberJoined to Shadow
::      — Server sends SignalingHelper to both sides with each other's P2P endpoint
::      — After 2 s: server sends SignalingEvent(ESTABLISHED) to both sides
::   5. Stephen lists rooms (sees the room with 2 members)
::   6. Stephen leaves the room
::      — Server sends MemberLeft to Shadow
::   7. Shadow leaves the room (room is now empty and destroyed)
::   8. Shadow lists rooms (empty again)

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
echo ============================================================
"%EXE%" %HOST% %PORT% room-create Shadow 1234 2
echo.

echo  NOTE: copy the roomId from the output above and set it below.
set /p ROOM_ID="Enter roomId: "

echo ============================================================
echo  Step 3: Room list — should show Shadow's room
echo ============================================================
"%EXE%" %HOST% %PORT% room-list Shadow 1234
echo.

echo ============================================================
echo  Step 4: Stephen joins the room
echo    Shadow will receive MemberJoined + SignalingHelper
echo    Both will receive SignalingEvent(ESTABLISHED) after ~2 s
echo ============================================================
"%EXE%" %HOST% %PORT% room-join Stephen 12345 %ROOM_ID%
echo.

echo  (wait a moment for the delayed ESTABLISHED notification)
timeout /t 3 /nobreak > nul

echo ============================================================
echo  Step 5: Room list — should show room with 2 members
echo ============================================================
"%EXE%" %HOST% %PORT% room-list Stephen 12345
echo.

echo ============================================================
echo  Step 6: Stephen leaves the room
echo    Shadow will receive MemberLeft
echo ============================================================
"%EXE%" %HOST% %PORT% room-leave Stephen 12345 %ROOM_ID%
echo.

echo ============================================================
echo  Step 7: Shadow leaves the room (room destroyed)
echo ============================================================
"%EXE%" %HOST% %PORT% room-leave Shadow 1234 %ROOM_ID%
echo.

echo ============================================================
echo  Step 8: Room list — should be empty again
echo ============================================================
"%EXE%" %HOST% %PORT% room-list Shadow 1234
echo.

echo ============================================================
echo  Walkthrough complete.
echo ============================================================
endlocal

pause
