#if WITH_DEV_AUTOMATION_TESTS
#include "MeteorClientWorldSubsystem.h"
#include "WorldObjects/WoodProductPresentationWorldSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "UnrealClient.h"
#include "Widgets/SWindow.h"

using namespace UE::ElementSandbox::Meteor;
namespace
{
	/** 显式离线验收：由 UE 自身截图并执行窗口生命周期，记录真实最小化时长；不参与正常游戏。 */
	class FMeteorRenderValidationProbe final
	{
	public:
		FMeteorRenderValidationProbe()
		{
			if (FParse::Value(FCommandLine::Get(), TEXT("MeteorRenderValidationRoot="), Output))
			{
				FParse::Value(FCommandLine::Get(), TEXT("MeteorRenderMinimizeSeconds="), MinimizeSeconds);
				bTestOcclusion = FParse::Param(FCommandLine::Get(), TEXT("MeteorRenderOcclusion"));
				TickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FMeteorRenderValidationProbe::Tick);
			}
		}
		~FMeteorRenderValidationProbe() { FWorldDelegates::OnWorldPostActorTick.Remove(TickHandle); }
	private:
		void Capture(const TCHAR* Name)
		{
			FScreenshotRequest::RequestScreenshot(FPaths::Combine(Output, FString(Name) + TEXT(".png")), false, false);
			UE_LOG(LogTemp, Display, TEXT("MeteorRenderValidation: CAPTURE %s"), Name);
		}
		void Finish(UWorld& World, bool bWindowPassed)
		{
			const auto* Wood = World.GetSubsystem<UWoodProductPresentationWorldSubsystem>();
			const bool bPassed = bWindowPassed && Wood->GetInstanceCount() == PreparedCount
				&& Wood->GetInstanceRemoveCount() == 0 && Wood->GetTransformUpdateCount() == 0;
			const FString Result = FString::Printf(TEXT("{\"passed\":%s,\"minimized_seconds\":%.3f,\"instances\":%d,\"adds\":%llu,\"removes\":%llu,\"transform_updates\":%llu,\"window_active_after_restore\":%s}"),
				bPassed ? TEXT("true") : TEXT("false"), MinimizedDuration, Wood->GetInstanceCount(),
				Wood->GetInstanceAddCount(), Wood->GetInstanceRemoveCount(), Wood->GetTransformUpdateCount(),
				GEngine->GameViewport->GetWindow()->IsActive() ? TEXT("true") : TEXT("false"));
			FFileHelper::SaveStringToFile(Result, *FPaths::Combine(Output, TEXT("Result.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogTemp, Display, TEXT("MeteorRenderValidation: %s %s"), bPassed ? TEXT("PASS") : TEXT("FAIL"), *Result);
			bDone = true;
			FPlatformMisc::RequestExit(false);
		}
		void Tick(UWorld* World, ELevelTick, float)
		{
			if (bDone || !World || !World->IsGameWorld() || World->GetNetMode() != NM_Standalone || !GEngine->GameViewport) return;
			if (!ObservedWorld.IsValid())
			{
				ObservedWorld = World;
				IFileManager::Get().MakeDirectory(*Output, true);
				World->GetSubsystem<UMeteorClientWorldSubsystem>()->OnPresentationChanges().AddLambda(
					[this](TConstArrayView<FMeteorClientPresentationLane> Changes)
					{
						for (const auto& Change : Changes)
						{
							if (Change.State == EMeteorClientDebrisState::Prepared) ++PreparedCount;
							if (Change.State == EMeteorClientDebrisState::Flying)
							{
								StartTime = Change.LocalStartTimeSeconds;
								EndTime = FMath::Max(EndTime, StartTime + Change.ImpactDurationSeconds + Change.SettlingDurationSeconds);
							}
						}
					});
			}
			if (ObservedWorld != World) return;
			const double Now = World->GetTimeSeconds(), Wall = FPlatformTime::Seconds();
			const auto Window = GEngine->GameViewport->GetWindow();
			if (!Window) return;
			if (WindowStage == 1)
			{
				if (Window->IsWindowMinimized()) { MinimizedAt = Wall; WindowStage = 2; UE_LOG(LogTemp, Display, TEXT("MeteorRenderValidation: MINIMIZED wall=%.3f"), Wall); }
				else if (Wall - RequestedAt > 5) Finish(*World, false);
				return;
			}
			if (WindowStage == 2)
			{
				if (!Window->IsWindowMinimized()) { MinimizedDuration = Wall - MinimizedAt; Finish(*World, false); return; }
				if (Wall - MinimizedAt >= MinimizeSeconds)
				{
					MinimizedDuration = Wall - MinimizedAt;
					Window->Restore(); Window->BringToFront(true); RequestedAt = Wall; WindowStage = 3;
					UE_LOG(LogTemp, Display, TEXT("MeteorRenderValidation: RESTORE_REQUEST duration=%.3f"), MinimizedDuration);
				}
				return;
			}
			if (WindowStage == 3)
			{
				if (Wall - RequestedAt >= 3 && !Window->IsWindowMinimized())
				{
					Capture(TEXT("09_AfterRestore")); RequestedAt = Wall; WindowStage = 4;
				}
				else if (Wall - RequestedAt > 10) Finish(*World, false);
				return;
			}
			if (WindowStage == 4)
			{
				if (Wall - RequestedAt >= 3)
					Finish(*World, IFileManager::Get().FileSize(*FPaths::Combine(Output, TEXT("09_AfterRestore.png"))) > 0);
				return;
			}
			if (!bCapturedPrepared && PreparedCount && World->GetSubsystem<UWoodProductPresentationWorldSubsystem>()->GetInstanceCount() == PreparedCount)
			{
				Capture(TEXT("01_Prepared")); bCapturedPrepared = true;
			}
			if (StartTime < 0 || EndTime <= StartTime) return;
			auto* Controller = World->GetFirstPlayerController();
			AActor* Camera = Controller ? Controller->GetViewTarget() : nullptr;
			if (!Camera) return;
			if (!bCameraSaved)
			{
				CameraTransform = Camera->GetActorTransform(); bCameraSaved = true;
				if (bTestOcclusion)
				{
					// 固定远景基准：遮住最终落点，保留飞行最高点的视线，检查 WPO 包围盒的遮挡剔除。
					Occluder = World->SpawnActor<AStaticMeshActor>();
					Occluder->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
					Occluder->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
					const FVector CameraPosition = CameraTransform.GetLocation();
					Occluder->SetActorLocationAndRotation(FVector(CameraPosition.X * .5, CameraPosition.Y * .5, 15000),
						FRotator(0, (-CameraPosition).Rotation().Yaw, 0));
					Occluder->SetActorScale3D(FVector(20, 700, 300));
					Occluder->GetStaticMeshComponent()->SetCastShadow(false);
					Occluder->SetActorEnableCollision(false);
				}
			}
			const double Alpha = (Now - StartTime) / (EndTime - StartTime);
			if (Occluder.IsValid() && !bCapturedOcclusion && Alpha >= .46)
			{
				Capture(TEXT("03a_OccludedRestPositions")); bCapturedOcclusion = true;
			}
			if (Occluder.IsValid() && !Occluder->IsHidden() && Alpha >= .50) Occluder->SetActorHiddenInGame(true);
			if (ImageStage == 0 && Alpha >= .10) { Capture(TEXT("02_Flight")); ++ImageStage; }
			else if (ImageStage == 1 && Alpha >= .28)
			{
				Controller->ConsoleCommand(TEXT("r.BufferVisualizationTarget Velocity"));
				Controller->ConsoleCommand(TEXT("viewmode VisualizeBuffer")); ++ImageStage;
			}
			else if (ImageStage == 2 && Alpha >= .35) { Capture(TEXT("03_Velocity")); ++ImageStage; }
			else if (ImageStage == 3 && Alpha >= .42) { Controller->ConsoleCommand(TEXT("viewmode lit")); ++ImageStage; }
			else if (ImageStage == 4 && Alpha >= .52)
			{
				Camera->SetActorRotation(CameraTransform.Rotator() + FRotator(0, 110, 0)); ++ImageStage;
			}
			else if (ImageStage == 5 && Alpha >= .57) { Capture(TEXT("04_LookAway")); ++ImageStage; }
			else if (ImageStage == 6 && Alpha >= .60) { Camera->SetActorTransform(CameraTransform); ++ImageStage; }
			else if (ImageStage == 7 && Alpha >= .68) { Capture(TEXT("05_ReturnToFlight")); ++ImageStage; }
			else if (ImageStage == 8 && Alpha >= .91) { Capture(TEXT("06_Settling")); ++ImageStage; }
			else if (ImageStage == 9 && Now > EndTime + 2) { Capture(TEXT("07_Landed")); ++ImageStage; }
			else if (ImageStage == 10 && Now > EndTime + 3)
			{
				World->GetWorldSettings()->SetTimeDilation(1);
				Capture(TEXT("08_BeforeMinimize")); RequestedAt = Wall; ++ImageStage;
			}
			else if (ImageStage == 11 && Wall - RequestedAt > 2)
			{
				Window->Minimize(); RequestedAt = Wall; WindowStage = 1;
			}
		}
		FDelegateHandle TickHandle;
			TWeakObjectPtr<UWorld> ObservedWorld;
			TWeakObjectPtr<AStaticMeshActor> Occluder;
		FString Output;
		FTransform CameraTransform;
		double StartTime = -1, EndTime = 0, RequestedAt = 0, MinimizedAt = 0, MinimizedDuration = 0, MinimizeSeconds = 605;
		int32 PreparedCount = 0, ImageStage = 0, WindowStage = 0;
			bool bDone = false, bCameraSaved = false, bCapturedPrepared = false;
			bool bTestOcclusion = false, bCapturedOcclusion = false;
	};
	FMeteorRenderValidationProbe GRenderValidationProbe;
}
#endif
