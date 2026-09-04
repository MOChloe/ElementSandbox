#if WITH_DEV_AUTOMATION_TESTS
#include "AbilitySystem/ElementAbilitySystemComponent.h"
#include "Abilities/MeteorStrikeGameplayAbility.h"
#include "Game/ElementSandboxPlayerState.h"
#include "Inventory/InventoryComponent.h"
#include "MeteorClientWorldSubsystem.h"
#include "Network/WorldChunkStreamingComponent.h"
#include "Tags/ElementGameplayTags.h"
#include "WorldObjects/WoodProductPresentationWorldSubsystem.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "UObject/UObjectIterator.h"

namespace
{
	/** 显式验收参数驱动真实客户端快捷栏/GAS 输入；只允许在调用者隔离的测试存档运行。 */
	class FMeteorGameplayRegressionProbe final
	{
	public:
		FMeteorGameplayRegressionProbe()
		{
			if (!FParse::Value(FCommandLine::Get(), TEXT("MeteorGameplayRegressionRoot="), Output)) return;
			FParse::Value(FCommandLine::Get(), TEXT("MeteorGameplayRegressionResident="), MinimumResident);
			FParse::Value(FCommandLine::Get(), TEXT("MeteorGameplayRegressionWarmup="), Warmup);
			if (!Output.IsEmpty()) IFileManager::Get().MakeDirectory(*Output, true);
			TickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FMeteorGameplayRegressionProbe::Tick);
		}
		~FMeteorGameplayRegressionProbe() { FWorldDelegates::OnWorldPostActorTick.Remove(TickHandle); }
	private:
		void Capture(const FString& Name)
		{
			FScreenshotRequest::RequestScreenshot(FPaths::Combine(Output, Name + TEXT(".png")), false, false);
		}
		void Tick(UWorld* World, ELevelTick, float)
		{
			if (bDone || !World || !World->IsGameWorld() || World->GetNetMode() != NM_Client) return;
			auto* Controller = World->GetFirstPlayerController();
			auto* State = Controller ? Controller->GetPlayerState<AElementSandboxPlayerState>() : nullptr;
			auto* Inventory = State ? State->GetInventoryComponent() : nullptr;
			auto* Ability = State ? State->GetElementAbilitySystemComponent() : nullptr;
			auto* Stream = Controller ? Controller->FindComponentByClass<UWorldChunkStreamingComponent>() : nullptr;
			if (!Inventory || !Ability || !Stream || !Controller->GetPawn()) return;
			const auto Stats = Stream->GetStreamingStats();
			const double Now = World->GetTimeSeconds();
			if (SelectedAt < 0)
			{
				if (Now < Warmup || !Stats.bActivationCoreReady || Stats.AuthorityResidentEntityCount < MinimumResident)
				{
					if (Now >= NextSample)
					{
						UE_LOG(LogTemp, Display, TEXT("MeteorGameplayRegression: waiting resident=%d chunks=%d loads=%d inject=%d time=%.1f"),
							Stats.AuthorityResidentEntityCount, Stats.AuthorityResidentChunkCount,
							Stats.AuthorityPendingLoadCount, Stats.AuthorityPendingInjectionCount, Now);
						NextSample = Now + 5;
					}
					return;
				}
				Inventory->SelectQuickbarSlot(7);
				SelectedAt = Now;
				Capture(TEXT("00_BeforeStrike"));
				UE_LOG(LogTemp, Display, TEXT("MeteorGameplayRegression: READY resident=%d chunks=%d time=%.3f"),
					Stats.AuthorityResidentEntityCount, Stats.AuthorityResidentChunkCount, Now);
				Csv = TEXT("elapsed,instances,visible_instances,hidden_instances,groups,visible_groups,authority_resident\n");
				return;
			}
			if (StartedAt < 0)
			{
				if (Now < SelectedAt + 1) return;
				bool bHasMeteor = false;
				for (const auto& Spec : Ability->GetActivatableAbilities())
					bHasMeteor |= Spec.Ability && Spec.Ability->IsA<UMeteorStrikeGameplayAbility>();
				if (!bHasMeteor) return;
				Ability->AbilityInputTagPressed(ElementSandboxGameplayTags::Input_Use_Primary);
				Ability->AbilityInputTagReleased(ElementSandboxGameplayTags::Input_Use_Primary);
				StartedAt = Now;
				NextSample = 0;
				UE_LOG(LogTemp, Display, TEXT("MeteorGameplayRegression: INPUT time=%.3f"), Now);
				return;
			}
			const double Elapsed = Now - StartedAt;
			if (Now >= NextSample)
			{
				int32 Visible = 0, Hidden = 0, Groups = 0, VisibleGroups = 0;
				for (TObjectIterator<UHierarchicalInstancedStaticMeshComponent> It; It; ++It)
				{
					if (It->GetWorld() != World || !It->IsRegistered() || !It->GetStaticMesh()
						|| It->GetStaticMesh()->GetFName() != TEXT("SM_WoodBlock")) continue;
					++Groups;
					if (It->IsVisible()) { Visible += It->GetInstanceCount(); ++VisibleGroups; }
					else Hidden += It->GetInstanceCount();
				}
				const auto* Wood = World->GetSubsystem<UWoodProductPresentationWorldSubsystem>();
				Csv += FString::Printf(TEXT("%.3f,%d,%d,%d,%d,%d,%d\n"), Elapsed,
					Wood->GetInstanceCount(), Visible, Hidden, Groups, VisibleGroups, Stats.AuthorityResidentEntityCount);
				UE_LOG(LogTemp, Display, TEXT("MeteorGameplayRegression: t=%.2f native=%d visible=%d hidden=%d groups=%d"),
					Elapsed, Wood->GetInstanceCount(), Visible, Hidden, Groups);
				NextSample = Now + 1;
				FFileHelper::SaveStringToFile(Csv, *FPaths::Combine(Output, TEXT("Presentation.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			}
			constexpr double CaptureSeconds[] = {3, 6.1, 7, 9, 14, 26, 66, 116};
			if (CaptureIndex < UE_ARRAY_COUNT(CaptureSeconds) && Elapsed >= CaptureSeconds[CaptureIndex])
			{
				Capture(FString::Printf(TEXT("%02d_AfterInput_%.1fs"), CaptureIndex + 1, CaptureSeconds[CaptureIndex]));
				++CaptureIndex;
			}
			if (Elapsed >= 120)
			{
				bDone = true;
				UE_LOG(LogTemp, Display, TEXT("MeteorGameplayRegression: CAPTURE COMPLETE; screenshots require visual inspection."));
				FPlatformMisc::RequestExit(false);
			}
		}
		FDelegateHandle TickHandle;
		FString Output, Csv;
		int32 MinimumResident = 1300000, CaptureIndex = 0;
		double Warmup = 60, SelectedAt = -1, StartedAt = -1, NextSample = 0;
		bool bDone = false;
	};
	FMeteorGameplayRegressionProbe GGameplayRegressionProbe;
}
#endif
