#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

namespace
{
/** 仅由隔离测试服务器显式启动；重复跨越 100m Chunk 边界，客户端保持正常渲染配置。 */
struct FRenderChunkTraversalProbe final
{
	double StartedAt = FPlatformTime::Seconds();
	double NextStepAt = 0.0;
	FVector Origin = FVector::ZeroVector;
	int32 Step = 0;
	bool bHasOrigin = false;

	bool Tick(float)
	{
		const double Now = FPlatformTime::Seconds();
		if (!GEngine || Now - StartedAt > 180.0)
		{
			UE_LOG(LogTemp, Display, TEXT("RenderTraversal: completed, transitions=%d."), Step);
			return false;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World || !World->IsGameWorld() || World->GetNetMode() != NM_DedicatedServer)
			{
				continue;
			}
			for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			{
				APlayerController* Controller = It->Get();
				APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
				if (!Pawn) continue;
				if (!bHasOrigin)
				{
					Origin = Pawn->GetActorLocation();
					Origin.X = (FMath::FloorToDouble(Origin.X / 10000.0) + 1.0) * 10000.0;
					bHasOrigin = true;
					NextStepAt = Now + 20.0;
				}
				if (Now >= NextStepAt && Step < 24)
				{
					const FVector Target = Origin + FVector((Step % 2) ? -500.0 : 500.0, 0.0, 100.0);
					const bool bMoved = Pawn->TeleportTo(Target, Pawn->GetActorRotation(), false, true);
					UE_LOG(LogTemp, Display, TEXT("RenderTraversal: step=%d moved=%d position=%s."),
						Step, bMoved ? 1 : 0, *Pawn->GetActorLocation().ToString());
					++Step;
					NextStepAt = Now + 5.0;
				}
				return true;
			}
		}
		return true;
	}
};

FAutoConsoleCommand StartRenderChunkTraversalProbe(
	TEXT("ElementSandbox.Test.TraverseChunks"),
	TEXT("Isolated test server only: traverse a chunk boundary repeatedly for rendering crash diagnosis."),
	FConsoleCommandDelegate::CreateLambda([]
	{
		const TSharedRef<FRenderChunkTraversalProbe> Probe = MakeShared<FRenderChunkTraversalProbe>();
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Probe](float DeltaSeconds) { return Probe->Tick(DeltaSeconds); }));
	}));
}

#endif
