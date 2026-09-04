#include "Game/LocalServerSessionSubsystem.h"

#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Meteor/MeteorProfileSettings.h"
#include "WorldStorageProcessRole.h"
#include "WorldStorageSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogElementSandboxLocalServer, Log, All);

namespace
{
	// Editor Server 冷启动还要装载百万世界种子，不能沿用只够普通地图启动的短期限。
	constexpr double LocalServerReadyTimeoutSeconds = 120.0;
	constexpr double LocalServerShutdownTimeoutSeconds = 8.0;

	bool WriteMarker(const FString& Path, const FString& Contents)
	{
		return IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true)
			&& FFileHelper::SaveStringToFile(Contents, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool ParseSessionToken(FString& OutToken)
	{
		FString RawToken;
		FGuid Parsed;
		return FParse::Value(FCommandLine::Get(), TEXT("LocalServerSession="), RawToken)
			&& FGuid::ParseExact(RawToken, EGuidFormats::Digits, Parsed)
			&& (OutToken = Parsed.ToString(EGuidFormats::Digits), true);
	}

}

class FLocalServerSessionRuntime final
{
public:
	FProcHandle ServerProcess;
	FTSTicker::FDelegateHandle TickerHandle;
	FString SessionToken;
	FString SessionRoot;
	FString ReadyPath;
	FString ShutdownPath;
	FString StoppedPath;
	int32 Port = 0;
	double LaunchTimeSeconds = 0.0;
	bool bServerChild = false;
	bool bReadyPublished = false;
	bool bReadyObserved = false;
	bool bTravelIssued = false;
	bool bCheckpointRequested = false;
	bool bTerminalFailure = false;
};

void ULocalServerSessionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Runtime = MakePimpl<FLocalServerSessionRuntime>();
	Runtime->bServerChild = FParse::Param(
		FCommandLine::Get(), TEXT("ElementSandboxLocalServerChild"));
	if (Runtime->bServerChild)
	{
		if (!ParseSessionToken(Runtime->SessionToken))
		{
			UE_LOG(LogElementSandboxLocalServer, Error, TEXT("本地 Server Child 缺少有效 Session Token。"));
			return;
		}
		Runtime->SessionRoot = FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("LocalServerSessions"), Runtime->SessionToken);
		Runtime->ReadyPath = FPaths::Combine(Runtime->SessionRoot, TEXT("Ready"));
		Runtime->ShutdownPath = FPaths::Combine(Runtime->SessionRoot, TEXT("Shutdown"));
		Runtime->StoppedPath = FPaths::Combine(Runtime->SessionRoot, TEXT("Stopped"));
		Runtime->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &ULocalServerSessionSubsystem::TickSession));
		return;
	}

	const UWorld* InitialWorld = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (FWorldStorageProcessRole::ShouldUseExternalLocalServerForCurrentWorld(InitialWorld)
		&& !LaunchClientServerProcess())
	{
		UE_LOG(LogElementSandboxLocalServer, Error,
			TEXT("无法启动隐藏本地服务器；单机不会回退到进程内 Authority。"));
	}
}

void ULocalServerSessionSubsystem::Deinitialize()
{
	if (Runtime)
	{
		if (Runtime->TickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(Runtime->TickerHandle);
			Runtime->TickerHandle.Reset();
		}
		if (!Runtime->bServerChild && Runtime->ServerProcess.IsValid())
		{
			RequestServerShutdownAndWait();
		}
	}
	Runtime.Reset();
	Super::Deinitialize();
}

bool ULocalServerSessionSubsystem::IsManagingLocalServer() const
{
	return Runtime && (Runtime->bServerChild || Runtime->ServerProcess.IsValid());
}

bool ULocalServerSessionSubsystem::IsLocalServerReady() const
{
	return Runtime && (Runtime->bReadyPublished || Runtime->bReadyObserved);
}

bool ULocalServerSessionSubsystem::LaunchClientServerProcess()
{
	check(Runtime && !Runtime->bServerChild);
	Runtime->SessionToken = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Runtime->SessionRoot = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("LocalServerSessions"), Runtime->SessionToken);
	Runtime->ReadyPath = FPaths::Combine(Runtime->SessionRoot, TEXT("Ready"));
	Runtime->ShutdownPath = FPaths::Combine(Runtime->SessionRoot, TEXT("Shutdown"));
	Runtime->StoppedPath = FPaths::Combine(Runtime->SessionRoot, TEXT("Stopped"));
	if (!IFileManager::Get().MakeDirectory(*Runtime->SessionRoot, true))
	{
		return false;
	}

	int32 RequestedPort = 0;
	FParse::Value(FCommandLine::Get(), TEXT("LocalServerPort="), RequestedPort);
	Runtime->Port = RequestedPort > 0 && RequestedPort <= 65535
		? RequestedPort
		: 17777 + static_cast<int32>(FPlatformProcess::GetCurrentProcessId() % 1000);

	FString ServerExecutable = FPaths::Combine(
		FPaths::ProjectDir(), TEXT("Binaries/Win64/ElementSandboxServer.exe"));
	bool bEditorServer = false;
	if (!IFileManager::Get().FileExists(*ServerExecutable))
	{
#if WITH_EDITOR
		ServerExecutable = FPlatformProcess::ExecutablePath();
		bEditorServer = true;
#else
		return false;
#endif
	}

		FString SaveRootOverride;
		FParse::Value(FCommandLine::Get(), TEXT("WorldSaveRoot="), SaveRootOverride);
		const FString SaveRoot = FPaths::ConvertRelativePathToFull(SaveRootOverride.IsEmpty()
			? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("WorldSaves/LocalSinglePlayer"))
			: SaveRootOverride);
		FString SeedRootOverride;
		FParse::Value(FCommandLine::Get(), TEXT("WorldSeedRoot="), SeedRootOverride);
		const FString SeedRoot = FPaths::ConvertRelativePathToFull(SeedRootOverride.IsEmpty()
			? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("WorldSeeds/MillionSettlement"))
			: SeedRootOverride);
	const FString ServerLogPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(Runtime->SessionRoot, TEXT("Server.log")));
	FString Arguments;
	if (bEditorServer)
	{
		Arguments += FString::Printf(TEXT("\"%s\" "), *FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
		// Source Editor 以 -server/-nosound 启动时仍会装载 MetaSound，并可能在其 Frontend
		// 静态初始化阶段崩溃。隐藏本地 Server 没有音频职责，明确禁用插件可避免加载无关模块；
		// 正式 ElementSandboxServer.exe 不走此 Editor 专用参数。
		Arguments += TEXT("-DisablePlugins=Metasound ");
	}
		Arguments += FString::Printf(
			TEXT("/Game/Maps/DefaultMap -server -game -unattended -nosplash -nosound -nullrhi ")
		TEXT("-port=%d -ElementSandboxLocalServerChild -LocalServerSession=%s ")
		TEXT("-WorldSaveRoot=\"%s\" -WorldSeedRoot=\"%s\" -abslog=\"%s\""),
		Runtime->Port,
		*Runtime->SessionToken,
		*SaveRoot,
			*SeedRoot,
			*ServerLogPath);
	// 非 Shipping Profile 参数只转交给真正拥有 Authority 的 Server Child。
	double MeteorProfileStrikeDelaySeconds = 0.0;
	if (FParse::Value(
		FCommandLine::Get(), TEXT("MeteorProfileStrikeDelay="), MeteorProfileStrikeDelaySeconds))
	{
		Arguments += FString::Printf(
			TEXT(" -MeteorProfileStrikeDelay=%.3f"),
			UE::ElementSandbox::Meteor::Profile::SanitizeStrikeDelaySeconds(
				MeteorProfileStrikeDelaySeconds));
	}
	double MeteorProfileAuthorityImpactTimeSeconds = 0.0;
	if (FParse::Value(
		FCommandLine::Get(),
		TEXT("MeteorProfileAuthorityImpactTime="),
		MeteorProfileAuthorityImpactTimeSeconds))
	{
		Arguments += FString::Printf(
			TEXT(" -MeteorProfileAuthorityImpactTime=%.3f"),
			UE::ElementSandbox::Meteor::Profile::SanitizeAuthorityImpactTimeSeconds(
				MeteorProfileAuthorityImpactTimeSeconds));
	}
	int32 MeteorProfileWarmupSeconds = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("MeteorProfileWarmupSeconds="), MeteorProfileWarmupSeconds))
	{
		Arguments += FString::Printf(TEXT(" -MeteorProfileWarmupSeconds=%d"), MeteorProfileWarmupSeconds);
	}

	Runtime->ServerProcess = FPlatformProcess::CreateProc(
		*ServerExecutable,
		*Arguments,
		true,
		true,
		true,
		nullptr,
		0,
		*FPaths::GetPath(ServerExecutable),
		nullptr,
		nullptr,
		nullptr);
	if (!Runtime->ServerProcess.IsValid())
	{
		return false;
	}
	Runtime->LaunchTimeSeconds = FPlatformTime::Seconds();
	Runtime->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &ULocalServerSessionSubsystem::TickSession));
	UE_LOG(LogElementSandboxLocalServer, Display,
		TEXT("已隐藏启动本地服务器，等待 Ready：127.0.0.1:%d。"), Runtime->Port);
	return true;
}

bool ULocalServerSessionSubsystem::TickSession(const float DeltaSeconds)
{
	(void)DeltaSeconds;
	if (!Runtime)
	{
		return false;
	}
	if (Runtime->bServerChild)
	{
		TickServerChild();
	}
	else
	{
		TickClientParent();
		if (Runtime->bTravelIssued || Runtime->bTerminalFailure)
		{
			return false;
		}
	}
	return true;
}

void ULocalServerSessionSubsystem::TickServerChild()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	UWorldStorageSubsystem* Storage = World ? World->GetSubsystem<UWorldStorageSubsystem>() : nullptr;
	if (!Runtime->bReadyPublished && World && World->GetNetMode() == NM_DedicatedServer
		&& Storage && Storage->IsStorageReady())
	{
		const FString Contents = FString::Printf(
			TEXT("WorldId=%s\nPid=%u\n"),
			*Storage->GetWorldId().ToString(EGuidFormats::Digits),
			FPlatformProcess::GetCurrentProcessId());
		Runtime->bReadyPublished = WriteMarker(Runtime->ReadyPath, Contents);
	}
	if (!IFileManager::Get().FileExists(*Runtime->ShutdownPath) || !Storage)
	{
		return;
	}
	if (!Runtime->bCheckpointRequested)
	{
		Runtime->bCheckpointRequested = true;
		Storage->RequestCheckpoint();
	}
	const FWorldStorageRuntimeStats Stats = Storage->GetRuntimeStats();
	if (!Stats.bCheckpointInFlight && Stats.DirtyEntityCount == 0)
	{
		WriteMarker(Runtime->StoppedPath, TEXT("CheckpointComplete\n"));
		FPlatformMisc::RequestExit(false);
	}
}

void ULocalServerSessionSubsystem::TickClientParent()
{
	if (!Runtime->ServerProcess.IsValid() || Runtime->bTravelIssued || Runtime->bTerminalFailure)
	{
		return;
	}
	if (!FPlatformProcess::IsProcRunning(Runtime->ServerProcess))
	{
		int32 ReturnCode = 0;
		FPlatformProcess::GetProcReturnCode(Runtime->ServerProcess, &ReturnCode);
		UE_LOG(LogElementSandboxLocalServer, Error,
			TEXT("本地服务器在 Ready 前退出，ExitCode=%d；不会启用进程内回退。"), ReturnCode);
		FPlatformProcess::CloseProc(Runtime->ServerProcess);
		Runtime->ServerProcess.Reset();
		Runtime->bTerminalFailure = true;
		return;
	}
	if (!IFileManager::Get().FileExists(*Runtime->ReadyPath))
	{
		if (FPlatformTime::Seconds() - Runtime->LaunchTimeSeconds > LocalServerReadyTimeoutSeconds)
		{
			UE_LOG(LogElementSandboxLocalServer, Error,
				TEXT("等待本地服务器 Ready 超时（%.0f 秒）。"), LocalServerReadyTimeoutSeconds);
			FPlatformProcess::TerminateProc(Runtime->ServerProcess, true);
			FPlatformProcess::CloseProc(Runtime->ServerProcess);
			Runtime->ServerProcess.Reset();
			Runtime->bTerminalFailure = true;
		}
		return;
	}
	Runtime->bReadyObserved = true;
	APlayerController* Controller = GetGameInstance()
		? GetGameInstance()->GetFirstLocalPlayerController()
		: nullptr;
	if (!Controller)
	{
		return;
	}
	Runtime->bTravelIssued = true;
	Controller->ClientTravel(
		FString::Printf(TEXT("127.0.0.1:%d"), Runtime->Port),
		TRAVEL_Absolute);
}

void ULocalServerSessionSubsystem::RequestServerShutdownAndWait()
{
	check(Runtime && Runtime->ServerProcess.IsValid());
	WriteMarker(Runtime->ShutdownPath, TEXT("CheckpointAndStop\n"));
	const double Deadline = FPlatformTime::Seconds() + LocalServerShutdownTimeoutSeconds;
	while (FPlatformProcess::IsProcRunning(Runtime->ServerProcess)
		&& FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::Sleep(0.05f);
	}
	if (FPlatformProcess::IsProcRunning(Runtime->ServerProcess))
	{
		UE_LOG(LogElementSandboxLocalServer, Warning,
			TEXT("本地服务器未在退出期限内完成 Checkpoint，正在终止子进程。"));
		FPlatformProcess::TerminateProc(Runtime->ServerProcess, true);
	}
	FPlatformProcess::CloseProc(Runtime->ServerProcess);
	Runtime->ServerProcess.Reset();
}
