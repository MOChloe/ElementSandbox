@echo off
setlocal EnableExtensions

set "PROJECT_FILE=%~dp0ElementSandbox.uproject"
set "SAVED_DIR=%~dp0Saved"
set "SOURCE_SEED=%~dp0WorldSeeds\MillionSettlement"
set "DEFAULT_WORLD=%SAVED_DIR%\WorldSaves\DefaultWorld"
set "LOCAL_SINGLE_PLAYER=%SAVED_DIR%\WorldSaves\LocalSinglePlayer"

if not exist "%PROJECT_FILE%" goto :MissingProject

if /I "%ELEMENT_SANDBOX_DRY_RUN%"=="1" (
	echo [ElementSandbox] DRY RUN - no files will be deleted.
	echo [ElementSandbox] Multiplayer save: "%DEFAULT_WORLD%"
	echo [ElementSandbox] Local single-player save: "%LOCAL_SINGLE_PLAYER%"
	echo [ElementSandbox] Preserved source seed: "%SOURCE_SEED%"
	echo [ElementSandbox] Preserved: "%SAVED_DIR%\WorldChunkCache"
	exit /b 0
)

%SystemRoot%\System32\tasklist.exe /FO CSV /NH 2>nul | %SystemRoot%\System32\findstr.exe /I /C:"UnrealEditor" /C:"ElementSandbox" >nul
if not errorlevel 1 goto :ProjectRunning

echo [ElementSandbox] This resets the active writable worlds used by the launch BATs.
echo [ElementSandbox] Multiplayer save: "%DEFAULT_WORLD%"
echo [ElementSandbox] Local single-player save: "%LOCAL_SINGLE_PLAYER%"
echo [ElementSandbox] The source seed and client cache are preserved.
echo.
%SystemRoot%\System32\choice.exe /C YN /N /M "[ElementSandbox] Continue? [Y/N] "
if errorlevel 2 goto :Cancelled

if exist "%DEFAULT_WORLD%\" (
	rmdir /S /Q "%DEFAULT_WORLD%"
	if exist "%DEFAULT_WORLD%\" goto :DefaultWorldDeleteFailed
	echo [ElementSandbox] Removed multiplayer save.
) else (
	echo [ElementSandbox] Already clean: multiplayer save
)

if exist "%LOCAL_SINGLE_PLAYER%\" (
	rmdir /S /Q "%LOCAL_SINGLE_PLAYER%"
	if exist "%LOCAL_SINGLE_PLAYER%\" goto :LocalSinglePlayerDeleteFailed
	echo [ElementSandbox] Removed local single-player save.
) else (
	echo [ElementSandbox] Already clean: local single-player save
)

echo.
echo [ElementSandbox] World reset complete.
echo [ElementSandbox] The next server start will create clean writable saves from the source seed.
pause
exit /b 0

:DefaultWorldDeleteFailed
echo [ElementSandbox] Failed to remove multiplayer save: "%DEFAULT_WORLD%"
goto :Failed

:LocalSinglePlayerDeleteFailed
echo [ElementSandbox] Failed to remove local single-player save: "%LOCAL_SINGLE_PLAYER%"
goto :Failed

:ProjectRunning
echo [ElementSandbox] Unreal Editor or an Element Sandbox process is still running.
echo [ElementSandbox] Close the editor, server, and clients before resetting the world.
pause
exit /b 1

:MissingProject
echo [ElementSandbox] ElementSandbox.uproject was not found beside this BAT.
pause
exit /b 1

:Cancelled
echo [ElementSandbox] World reset cancelled. Nothing was deleted.
exit /b 0

:Failed
echo [ElementSandbox] World reset failed. Check the messages above.
pause
exit /b 1
