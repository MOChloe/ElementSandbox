@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "PROJECT_FILE=%~dp0ElementSandbox.uproject"
set "MAP=/Game/Maps/DefaultMap"
set "WORLD_SEED=%~dp0WorldSeeds\MillionSettlement"
set "PORT=17777"
if defined ELEMENT_SANDBOX_PORT set "PORT=%ELEMENT_SANDBOX_PORT%"

rem Both clients run at native 1920x1080. On a single monitor, use Alt+Tab to
rem switch between the two windowed clients.
set "CLIENT_RES_X=1920"
set "CLIENT_RES_Y=1080"
set "QUALITY_COMMANDS=sg.ResolutionQuality 100,sg.ViewDistanceQuality 2,sg.AntiAliasingQuality 2,sg.PostProcessQuality 1,sg.TextureQuality 2,sg.EffectsQuality 2,sg.FoliageQuality 2,sg.ShadingQuality 2,sg.LandscapeQuality 2,r.ScreenPercentage 100,r.SecondaryScreenPercentage.GameViewport 100,r.DynamicRes.OperationMode 0"

call :ResolveEditor
if errorlevel 1 goto :MissingEditor
if not exist "%PROJECT_FILE%" goto :MissingProject
if not exist "%WORLD_SEED%\World.manifest" goto :MissingSeed

if /I "%ELEMENT_SANDBOX_DRY_RUN%"=="1" (
	echo [ElementSandbox] DRY RUN - no process will be created.
	echo [ElementSandbox] Server: "%UNREAL_EDITOR%" "%PROJECT_FILE%" "%MAP%" -server -port=%PORT% -WorldSeedRoot="%WORLD_SEED%"
	echo [ElementSandbox] Client 1: "%UNREAL_EDITOR%" "%PROJECT_FILE%" "127.0.0.1:%PORT%?Name=Client1" -game -windowed -ForceRes -ResX=%CLIENT_RES_X% -ResY=%CLIENT_RES_Y% -ExecCmds="%QUALITY_COMMANDS%"
	echo [ElementSandbox] Client 2: "%UNREAL_EDITOR%" "%PROJECT_FILE%" "127.0.0.1:%PORT%?Name=Client2" -game -windowed -ForceRes -ResX=%CLIENT_RES_X% -ResY=%CLIENT_RES_Y% -ExecCmds="%QUALITY_COMMANDS%"
	echo [ElementSandbox] Display: two native %CLIENT_RES_X%x%CLIENT_RES_Y% windowed clients, 100%% render scale.
	exit /b 0
)

call :EnsurePortIsFree
if errorlevel 1 goto :Failed

set "LOG_DIR=%TEMP%\ElementSandbox\LaunchLogs"
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%" >nul 2>&1
set "SERVER_LOG=%LOG_DIR%\TwoClients_Server.log"
set "CLIENT1_LOG=%LOG_DIR%\TwoClients_Client1.log"
set "CLIENT2_LOG=%LOG_DIR%\TwoClients_Client2.log"
del /q "%SERVER_LOG%" "%CLIENT1_LOG%" "%CLIENT2_LOG%" >nul 2>&1

echo [ElementSandbox] Starting source-project server on 127.0.0.1:%PORT% ...
start "Element Sandbox Server" "%UNREAL_EDITOR%" "%PROJECT_FILE%" "%MAP%" -server -game -unattended -nosplash -nosound -nullrhi -DisablePlugins=Metasound -port=%PORT% -ElementSandboxNoLocalServer -WorldSeedRoot="%WORLD_SEED%" -log -abslog="%SERVER_LOG%"

call :WaitForServer
if errorlevel 1 goto :ServerFailed

echo [ElementSandbox] Starting two source-project clients ...
start "Element Sandbox Client 1" "%UNREAL_EDITOR%" "%PROJECT_FILE%" "127.0.0.1:%PORT%?Name=Client1" -game -ElementSandboxNoLocalServer -windowed -ForceRes -ResX=%CLIENT_RES_X% -ResY=%CLIENT_RES_Y% -nosplash -ExecCmds="%QUALITY_COMMANDS%" -abslog="%CLIENT1_LOG%"
%SystemRoot%\System32\ping.exe -n 2 127.0.0.1 >nul
start "Element Sandbox Client 2" "%UNREAL_EDITOR%" "%PROJECT_FILE%" "127.0.0.1:%PORT%?Name=Client2" -game -ElementSandboxNoLocalServer -windowed -ForceRes -ResX=%CLIENT_RES_X% -ResY=%CLIENT_RES_Y% -nosplash -ExecCmds="%QUALITY_COMMANDS%" -abslog="%CLIENT2_LOG%"
exit /b 0

:ResolveEditor
set "UNREAL_EDITOR="
if defined ELEMENT_SANDBOX_ENGINE_ROOT if exist "%ELEMENT_SANDBOX_ENGINE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" set "UNREAL_EDITOR=%ELEMENT_SANDBOX_ENGINE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe"
for %%F in (
	"C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor.exe"
	"D:\UE_5.6\Engine\Binaries\Win64\UnrealEditor.exe"
) do if not defined UNREAL_EDITOR if exist "%%~fF" set "UNREAL_EDITOR=%%~fF"
if not defined UNREAL_EDITOR exit /b 1
exit /b 0

:EnsurePortIsFree
%SystemRoot%\System32\netstat.exe -ano -p UDP 2>nul | %SystemRoot%\System32\findstr.exe /C:":%PORT% " >nul
if not errorlevel 1 (
	echo [ElementSandbox] UDP port %PORT% is already in use. Close the old server or set ELEMENT_SANDBOX_PORT.
	exit /b 1
)
exit /b 0

:WaitForServer
set /a WAITED_SECONDS=0
:WaitForServerLoop
if exist "%SERVER_LOG%" (
	%SystemRoot%\System32\findstr.exe /C:"LogElementSandboxWorldStorage: Error:" "%SERVER_LOG%" >nul 2>&1
	if not errorlevel 1 exit /b 1
	%SystemRoot%\System32\findstr.exe /C:"LogElementSandboxWorldStorage: Display:" "%SERVER_LOG%" >nul 2>&1
	if not errorlevel 1 exit /b 0
)
if !WAITED_SECONDS! GEQ 120 exit /b 1
set /a WAITED_SECONDS+=1
%SystemRoot%\System32\ping.exe -n 2 127.0.0.1 >nul
goto :WaitForServerLoop

:MissingEditor
echo [ElementSandbox] Unreal Editor 5.6 was not found. Set ELEMENT_SANDBOX_ENGINE_ROOT to the UE 5.6 installation directory.
pause
exit /b 1

:MissingProject
echo [ElementSandbox] ElementSandbox.uproject was not found beside this BAT.
pause
exit /b 1

:MissingSeed
echo [ElementSandbox] World seed was not found: "%WORLD_SEED%\World.manifest"
pause
exit /b 1

:ServerFailed
echo [ElementSandbox] The source-project server did not become ready. Check "%SERVER_LOG%".
pause
exit /b 1

:Failed
pause
exit /b 1
