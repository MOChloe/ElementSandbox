#include "MeteorWorldSubsystem.h"
#include "Meteor/MeteorProfileSettings.h"

#if !UE_BUILD_SHIPPING

#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Network/WorldChunkStreamingComponent.h"

using namespace UE::ElementSandbox::Meteor;

namespace
{
		bool TryScheduleMeteorProfileStrikeAtAuthorityTime(
			UWorld* World,
			const double ImpactTimeSeconds,
			const bool bReportNotReady)
		{
			APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
			APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
			UMeteorWorldSubsystem* Meteor = World ? World->GetSubsystem<UMeteorWorldSubsystem>() : nullptr;
			if (!World || World->GetNetMode() == NM_Client || !Pawn || !Meteor || Meteor->HasActiveBurst())
			{
				if (bReportNotReady)
				{
					UE_LOG(LogTemp, Error,
						TEXT("Meteor.ProfileStrike 需要已就绪的 Authority World、Player Pawn 和空闲 Meteor Runtime。"));
				}
				return false;
			}

		FVector ImpactLocation;
			if (!Meteor->TryGetMapImpactLocation(
				Pawn->GetActorLocation(), Pawn->GetActorForwardVector(), ImpactLocation))
			{
				if (bReportNotReady)
				{
					UE_LOG(LogTemp, Error,
						TEXT("Meteor.ProfileStrike 找不到角色前方带宿主的 Resident 场景；未改变世界。"));
				}
				return false;
			}

			const double DelaySeconds = ImpactTimeSeconds - World->GetTimeSeconds();
			if (DelaySeconds < Profile::MinStrikeDelaySeconds)
			{
				if (bReportNotReady)
				{
					UE_LOG(LogTemp, Error,
						TEXT("Meteor.ProfileStrike 的 Authority 撞击时刻已经过去；未改变世界。"));
				}
				return false;
			}

			FMeteorBurstId BurstId;
			if (!Meteor->ScheduleStrike(ImpactLocation, ImpactTimeSeconds, BurstId))
			{
				if (bReportNotReady)
				{
					UE_LOG(LogTemp, Error, TEXT("Meteor.ProfileStrike 排程失败。"));
				}
				return false;
			}
			UE_LOG(LogTemp, Display,
				TEXT("MeteorProfileScenario：Burst=%llu，AuthorityTime=%.2fs，ImpactDelay=%.2fs，Impact=(%.0f,%.0f,%.0f)。"),
			BurstId.Value,
			ImpactTimeSeconds,
			DelaySeconds,
				ImpactLocation.X,
				ImpactLocation.Y,
				ImpactLocation.Z);
			return true;
		}

		bool TryScheduleMeteorProfileStrike(
			UWorld* World,
			const double DelaySeconds,
			const bool bReportNotReady)
		{
			const double NowSeconds = World ? World->GetTimeSeconds() : 0.0;
			return TryScheduleMeteorProfileStrikeAtAuthorityTime(
				World, NowSeconds + DelaySeconds, bReportNotReady);
		}

		void ScheduleMeteorProfileStrike(const TArray<FString>& Args, UWorld* World)
		{
			double DelaySeconds = 2.0;
			if (!Args.IsEmpty())
			{
				DelaySeconds = UE::ElementSandbox::Meteor::Profile::SanitizeStrikeDelaySeconds(
					FCString::Atod(*Args[0]));
			}
			TryScheduleMeteorProfileStrike(World, DelaySeconds, true);
		}

		/**
		 * 性能采样使用隐藏 Dedicated Server + 可见 Client。Server Child 在命令行
		 * 收到参数后等待 Player Pawn 和 Residency 就绪，再只排程一次。这条路径不修改
		 * Shipping Gameplay，也不会让客户端越权触发销毁。
		 */
		class FMeteorProfileAutoStrike final
		{
		public:
			FMeteorProfileAutoStrike()
			{
				double ParsedDelaySeconds = 0.0;
				double ParsedAuthorityImpactTimeSeconds = 0.0;
				const bool bEligibleProcess = FParse::Param(
					FCommandLine::Get(), TEXT("ElementSandboxLocalServerChild"))
					|| FParse::Param(FCommandLine::Get(), TEXT("ElementSandboxNoLocalServer"));
				const bool bHasAuthorityImpactTime = FParse::Value(
					FCommandLine::Get(),
					TEXT("MeteorProfileAuthorityImpactTime="),
					ParsedAuthorityImpactTimeSeconds);
				const bool bHasDelay = FParse::Value(
					FCommandLine::Get(), TEXT("MeteorProfileStrikeDelay="), ParsedDelaySeconds);
				if (!bEligibleProcess || (!bHasAuthorityImpactTime && !bHasDelay))
				{
					return;
				}
				FParse::Value(FCommandLine::Get(), TEXT("MeteorProfileWarmupSeconds="), WarmupSeconds);
				WarmupSeconds = FMath::Max(0, WarmupSeconds);
				if (bHasAuthorityImpactTime)
				{
					AuthorityImpactTimeSeconds = Profile::SanitizeAuthorityImpactTimeSeconds(
						ParsedAuthorityImpactTimeSeconds);
				}
				else
				{
					DelaySeconds = Profile::SanitizeStrikeDelaySeconds(ParsedDelaySeconds);
				}
				WorldTickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(
					this, &FMeteorProfileAutoStrike::HandleWorldPostActorTick);
			}

			~FMeteorProfileAutoStrike()
			{
				if (WorldTickHandle.IsValid())
				{
					FWorldDelegates::OnWorldPostActorTick.Remove(WorldTickHandle);
				}
			}

		private:
			void HandleWorldPostActorTick(UWorld* World, ELevelTick, float)
			{
				if (bScheduled || !World || !World->IsGameWorld() || World->GetNetMode() == NM_Client)
				{
					return;
				}
				const double NowSeconds = World->GetTimeSeconds();
				if (NowSeconds < NextAttemptSeconds)
				{
					return;
				}
				NextAttemptSeconds = NowSeconds + 0.25;
				APlayerController* Controller = World->GetFirstPlayerController();
				if (!Controller || !Controller->GetPawn())
				{
					AuthorityReadySinceSeconds = -1.0;
					return;
				}
				const UWorldChunkStreamingComponent* Streaming =
					Controller->FindComponentByClass<UWorldChunkStreamingComponent>();
				const FWorldChunkStreamingStats StreamingStats = Streaming
					? Streaming->GetStreamingStats() : FWorldChunkStreamingStats{};
				const bool bActivationCoreStable = StreamingStats.bActivationCoreReady
					&& StreamingStats.ActivationCoreChunkCount > 0
					&& StreamingStats.ActivationCoreAcknowledgedChunkCount
						== StreamingStats.ActivationCoreChunkCount
					&& StreamingStats.ActivationCoreAuthorityReadyChunkCount
						== StreamingStats.ActivationCoreChunkCount;
				if (!bActivationCoreStable)
				{
					AuthorityReadySinceSeconds = -1.0;
					return;
				}
				if (AuthorityReadySinceSeconds < 0.0)
				{
					AuthorityReadySinceSeconds = NowSeconds;
					UE_LOG(LogTemp, Display,
						TEXT("Meteor Profile Activation Core 已完整 ACK；至少预热 %d 秒，并等待当前客户端兴趣范围完整 ACK。"),
						WarmupSeconds);
					return;
				}
				if (NowSeconds - AuthorityReadySinceSeconds < WarmupSeconds)
				{
					return;
				}
				// 完整 ACK 表示客户端已经收齐当前地图；Authority 后台装填远处区块的队列不属于这个条件。
				if (StreamingStats.OfferedChunkCount == 0
					|| StreamingStats.AcknowledgedChunkCount != StreamingStats.OfferedChunkCount
					|| StreamingStats.PendingChunkCount != 0)
				{
					return;
				}
				UE_LOG(LogTemp, Display,
					TEXT("Meteor Profile 完整兴趣范围已就绪：Offer=%d ACK=%d ResidentChunk=%d ResidentEntity=%d Warmup=%.2fs。"),
					StreamingStats.OfferedChunkCount,
					StreamingStats.AcknowledgedChunkCount,
					StreamingStats.AuthorityResidentChunkCount,
					StreamingStats.AuthorityResidentEntityCount,
					NowSeconds - AuthorityReadySinceSeconds);
				if (AuthorityImpactTimeSeconds > 0.0
					&& AuthorityImpactTimeSeconds - NowSeconds < Profile::MinStrikeDelaySeconds)
				{
					UE_LOG(LogTemp, Error,
						TEXT("Meteor Profile 在固定 Authority 撞击时刻后才就绪；为避免不公平采样，本轮不触发。"));
					bScheduled = true;
					return;
				}
				bScheduled = AuthorityImpactTimeSeconds > 0.0
					? TryScheduleMeteorProfileStrikeAtAuthorityTime(
						World, AuthorityImpactTimeSeconds, false)
					: TryScheduleMeteorProfileStrike(World, DelaySeconds, false);
				if (bScheduled)
				{
					UE_LOG(LogTemp, Display,
						TEXT("Meteor Profile 自动触发已在 Authority 就绪后完成排程。"));
				}
			}

			FDelegateHandle WorldTickHandle;
			double DelaySeconds = 2.0;
			double AuthorityImpactTimeSeconds = -1.0;
				double NextAttemptSeconds = 0.0;
				double AuthorityReadySinceSeconds = -1.0;
				int32 WarmupSeconds = 20;
			bool bScheduled = false;
		};

		FAutoConsoleCommandWithWorldAndArgs GMeteorProfileStrikeCommand(
		TEXT("Meteor.ProfileStrike"),
			TEXT("Schedule a deterministic debris profiling strike in the current resident scene. Args: [impact delay seconds]."),
			FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ScheduleMeteorProfileStrike));

		FMeteorProfileAutoStrike GMeteorProfileAutoStrike;
	}

#endif
