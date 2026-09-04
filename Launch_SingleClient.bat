@echo off
setlocal EnableExtensions

set "PROJECT_FILE=%~dp0ElementSandbox.uproject"
set "MAP=/Game/Maps/DefaultMap"
set "WORLD_SEED=%~dp0WorldSeeds\MillionSettlement"
set "WORLD_SAVE_ROOT=%~dp0Saved\WorldSaves\DefaultWorld"
set "PORT=17777"
if defined ELEMENT_SANDBOX_PORT set "PORT=%ELEMENT_SANDBOX_PORT%"

rem Force every client to native 1920x1080 output.
set "CLIENT_RES_X=1920"
set "CLIENT_RES_Y=1080"
set "QUALITY_COMMANDS=sg.ResolutionQuality 100,sg.ViewDistanceQuality 2,sg.AntiAliasingQuality 2,sg.PostProcessQuality 1,sg.TextureQuality 2,sg.EffectsQuality 2,sg.FoliageQuality 2,sg.ShadingQuality 2,sg.LandscapeQuality 2,r.ScreenPercentage 100,r.SecondaryScreenPercentage.GameViewport 100,r.DynamicRes.OperationMode 0"

call :ResolveEditor
if errorlevel 1 goto :MissingEditor
if not exist "%PROJECT_FILE%" goto :MissingProject
if not exist "%WORLD_SEED%\World.manifest" goto :MissingSeed

if /I "%ELEMENT_SANDBOX_DRY_RUN%"=="1" (
	echo [ElementSandbox] DRY RUN - no process will be created.
	echo [ElementSandbox] Client: "%UNREAL_EDITOR%" "%PROJECT_FILE%" "%MAP%" -game -LocalServerPort=%PORT% -WorldSaveRoot="%WORLD_SAVE_ROOT%" -WorldSeedRoot="%WORLD_SEED%" -windowed -ForceRes -ResX=%CLIENT_RES_X% -ResY=%CLIENT_RES_Y% -ExecCmds="%QUALITY_COMMANDS%"
	echo [ElementSandbox] Display: native %CLIENT_RES_X%x%CLIENT_RES_Y%, 100%% render scale, balanced high-clarity settings.
	echo [ElementSandbox] The game starts a hidden local server and saves and stops it on exit.
	exit /b 0
)

call :EnsurePortIsFree
if errorlevel 1 goto :Failed

set "LOG_DIR=%TEMP%\ElementSandbox\LaunchLogs"
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%" >nul 2>&1
set "CLIENT_LOG=%LOG_DIR%\SingleClient_Client.log"
del /q "%CLIENT_LOG%" >nul 2>&1

echo [ElementSandbox] Starting the game with a hidden local server on 127.0.0.1:%PORT% ...
start "Element Sandbox Client" "%UNREAL_EDITOR%" "%PROJECT_FILE%" "%MAP%" -game -LocalServerPort=%PORT% -WorldSaveRoot="%WORLD_SAVE_ROOT%" -WorldSeedRoot="%WORLD_SEED%" -windowed -ForceRes -ResX=%CLIENT_RES_X% -ResY=%CLIENT_RES_Y% -nosplash -ExecCmds="%QUALITY_COMMANDS%" -abslog="%CLIENT_LOG%"
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

:Failed
pause
exit /b 1
