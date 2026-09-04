#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Network/WorldChunkStreamingComponent.h"
#include "WorldStorageSubsystem.h"

namespace
{
/** 显式隔离服务器夹具：两客户端完成基线后依次传送，检查兴趣并集、最后引用回收与缓存重入。 */
struct FWorldChunkStreamingNetworkProbe final
{
	double StartedAt = FPlatformTime::Seconds();
	double StageStartedAt = 0.0;
	double NextReportAt = 0.0;
	TWeakObjectPtr<APlayerController> Players[2];
	FVector Origins[2];
	FWorldChunkCoord RetainedChunk;
	int32 Stage = 0;

	bool Fail(const TCHAR* Reason) const
	{
		UE_LOG(LogTemp, Error, TEXT("ChunkStreamingProbe: FAIL %s"), Reason);
		return false;
	}

	bool Tick(float)
	{
		const double Now = FPlatformTime::Seconds();
		if (Now - StartedAt > 180.0) return Fail(TEXT("timeout"));
		if (!GEngine) return true;
		UWorld* World = nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (UWorld* Candidate = Context.World(); Candidate && Candidate->IsGameWorld()
				&& Candidate->GetNetMode() == NM_DedicatedServer)
			{
				World = Candidate;
				break;
			}
		}
		if (!World) return true;
		UWorldStorageSubsystem* Storage = World->GetSubsystem<UWorldStorageSubsystem>();
		if (!Storage) return true;
		if (Stage == 0)
		{
			int32 Count = 0;
			for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It && Count < 2; ++It)
			{
				if (APlayerController* Player = It->Get(); Player && Player->GetPawn()) Players[Count++] = Player;
			}
			if (Count != 2) return true;
		}
		FWorldChunkStreamingStats Stats[2];
		for (int32 Index = 0; Index < 2; ++Index)
		{
			APlayerController* Player = Players[Index].Get();
			UWorldChunkStreamingComponent* Streaming = Player
				? Player->FindComponentByClass<UWorldChunkStreamingComponent>() : nullptr;
			if (!Streaming || !Player->GetPawn()) return Stage == 0 ? true : Fail(TEXT("client disconnected"));
			Stats[Index] = Streaming->GetStreamingStats();
			if (!Stats[Index].bActivationCoreReady) return Stage == 0 ? true : Fail(TEXT("activation gate regressed"));
		}
		if (Now >= NextReportAt)
		{
			UE_LOG(LogTemp, Display, TEXT("ChunkStreamingProbe: stage=%d A=%d/%d center=(%d,%d,%d) B=%d/%d center=(%d,%d,%d) resident=%d"),
				Stage, Stats[0].AcknowledgedChunkCount, Stats[0].OfferedChunkCount,
				Stats[0].InterestCenter.X, Stats[0].InterestCenter.Y, Stats[0].InterestCenter.Z,
				Stats[1].AcknowledgedChunkCount, Stats[1].OfferedChunkCount,
				Stats[1].InterestCenter.X, Stats[1].InterestCenter.Y, Stats[1].InterestCenter.Z,
				Storage->GetRuntimeStats().ResidentChunkCount);
			NextReportAt = Now + 5.0;
		}
		if (Stage == 0)
		{
			if (!Storage->TryGetMostPopulatedChunk(RetainedChunk) || !Storage->IsChunkResident(RetainedChunk)) return true;
			for (int32 Index = 0; Index < 2; ++Index) Origins[Index] = Players[Index]->GetPawn()->GetActorLocation();
			Stage = 1;
			StageStartedAt = Now;
		}
		else if (Stage == 1 && Now - StageStartedAt >= 10.0)
		{
			APawn* Pawn = Players[1]->GetPawn();
			if (!Pawn->TeleportTo(Origins[1] + FVector(1600000.0, 0.0, 100.0), Pawn->GetActorRotation(), false, true))
				return Fail(TEXT("first teleport rejected"));
			Stage = 2;
			StageStartedAt = Now;
		}
		else if (Stage == 2 && Now - StageStartedAt >= 4.0)
		{
			if (Stats[1].InterestCenter.X - Stats[0].InterestCenter.X < 150) return Fail(TEXT("second interest did not move"));
			if (!Storage->IsChunkResident(RetainedChunk)) return Fail(TEXT("remaining player lost shared residency"));
			UE_LOG(LogTemp, Display, TEXT("ChunkStreamingProbe: remaining player retained shared chunk."));
			APawn* Pawn = Players[0]->GetPawn();
			if (!Pawn->TeleportTo(Origins[0] + FVector(1600000.0, 0.0, 100.0), Pawn->GetActorRotation(), false, true))
				return Fail(TEXT("second teleport rejected"));
			Stage = 3;
			StageStartedAt = Now;
		}
		else if (Stage == 3 && Now - StageStartedAt >= 4.0)
		{
			if (Storage->IsChunkResident(RetainedChunk))
			{
				return Now - StageStartedAt < 20.0 ? true : Fail(TEXT("last interest did not evict origin"));
			}
			UE_LOG(LogTemp, Display, TEXT("ChunkStreamingProbe: last player departure evicted origin."));
			for (int32 Index = 0; Index < 2; ++Index)
			{
				APawn* Pawn = Players[Index]->GetPawn();
				if (!Pawn->TeleportTo(Origins[Index], Pawn->GetActorRotation(), false, true)) return Fail(TEXT("return rejected"));
			}
			Stage = 4;
			StageStartedAt = Now;
		}
		else if (Stage == 4 && Now - StageStartedAt >= 4.0)
		{
			for (int32 Index = 0; Index < 2; ++Index)
			{
				if (Stats[Index].InterestCenter != FWorldChunkCoord::FromWorldLocation(Origins[Index])
					|| Stats[Index].ActivationCoreAcknowledgedChunkCount != Stats[Index].ActivationCoreChunkCount)
					return true;
			}
			UE_LOG(LogTemp, Display, TEXT("ChunkStreamingProbe: return baseline ready; checking the complete interest set."));
			Stage = 5;
			StageStartedAt = Now;
		}
		else if (Stage == 5)
		{
			for (int32 Index = 0; Index < 2; ++Index)
			{
				if (Stats[Index].OfferedChunkCount == 0 ||
					Stats[Index].AcknowledgedChunkCount != Stats[Index].OfferedChunkCount) return true;
			}
			UE_LOG(LogTemp, Display, TEXT("ChunkStreamingProbe: PASS two clients, independent subscriptions, retention and complete return snapshots."));
			return false;
		}
		return true;
	}
};

FAutoConsoleCommand StartWorldChunkStreamingNetworkProbe(
	TEXT("ElementSandbox.Test.ChunkStreaming"),
	TEXT("Isolated server only: validate two-client chunk subscriptions, teleport, retention and return."),
	FConsoleCommandDelegate::CreateLambda([]
	{
		const TSharedRef<FWorldChunkStreamingNetworkProbe> Probe = MakeShared<FWorldChunkStreamingNetworkProbe>();
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Probe](float DeltaSeconds) { return Probe->Tick(DeltaSeconds); }));
	}));
}

#endif
