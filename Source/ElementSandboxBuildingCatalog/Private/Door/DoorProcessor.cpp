#include "Door/DoorProcessor.h"

#include "BuildingWorldSubsystem.h"
#include "Definition/BuildingDefinition.h"
#include "Door/DoorStateFragment.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildPartTransformFragment.h"
#include "Entity/BuildDefinitionFragment.h"
#include "ElementSandboxBuilding.h"
#include "GameFramework/GameStateBase.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"

namespace
{
	constexpr float DoorTransitionDuration = 0.8f;
	// Door 的正面与默认玩家出生侧均为本地 -X；向 -90 度开启可让门扇
	// 扫向本地 +X，避免默认交互路径把玩家和 CameraBoom 卷入摆动轨迹。
	constexpr float DoorOpenAngleDegrees = -90.0f;
	const FVector DoorHingeLocalLocation(0.0, -45.0, 0.0);
	constexpr int32 MovingDoorPartIds[] = {0, 4, 5, 6};

	double ResolveDoorServerTimeSeconds(
		const FBuildProcessorContext& Context)
	{
		const UWorld* World = Context.BuildingSubsystem.GetWorld();
		if (World && World->GetNetMode() == NM_Client)
		{
			if (const AGameStateBase* GameState = World->GetGameState())
			{
				const double ServerTimeSeconds =
					GameState->GetServerWorldTimeSeconds();
				if (FMath::IsFinite(ServerTimeSeconds))
				{
					return ServerTimeSeconds;
				}
			}
		}
		return Context.WorldTimeSeconds;
	}
}

FBuildDoorProcessor::FBuildDoorProcessor(
	const bool bInAcceptAuthorityInteractions,
	FBuildDoorStateChangedDelegate InStateChanged)
	: FBuildProcessor(TEXT("Door"))
	, StateChanged(MoveTemp(InStateChanged))
	, bAcceptAuthorityInteractions(bInAcceptAuthorityInteractions)
{
}

bool FBuildDoorProcessor::RequestInteraction(const FBuildEntityHandle Entity)
{
	check(IsInGameThread());
	if (!bAcceptAuthorityInteractions
		|| !Entity.IsSet()
		|| ActiveEntities.Contains(Entity))
	{
		return false;
	}

	ActiveEntities.Add(Entity);
	FDoorWork& Work = ActiveWork.AddDefaulted_GetRef();
	Work.Entity = Entity;
	if (RequestExecution())
	{
		return true;
	}

	ActiveWork.Pop(EAllowShrinking::No);
	ActiveEntities.Remove(Entity);
	return false;
}

bool FBuildDoorProcessor::NotifyRestoredState(const FBuildEntityHandle Entity)
{
	check(IsInGameThread());
	if (!Entity.IsSet())
	{
		return false;
	}
	if (ActiveEntities.Contains(Entity))
	{
		return RequestExecution();
	}

	ActiveEntities.Add(Entity);
	FDoorWork& Work = ActiveWork.AddDefaulted_GetRef();
	Work.Entity = Entity;
	Work.bInteractionPending = false;
	if (RequestExecution())
	{
		return true;
	}

	ActiveWork.Pop(EAllowShrinking::No);
	ActiveEntities.Remove(Entity);
	return false;
}

EBuildProcessorRunResult FBuildDoorProcessor::Execute(
	FBuildProcessorContext& Context)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_Door_Execute);
	CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, DoorExecute);
	const double CurrentServerTimeSeconds = ResolveDoorServerTimeSeconds(Context);
	if (!FMath::IsFinite(CurrentServerTimeSeconds))
	{
		return EBuildProcessorRunResult::Failed;
	}

	bool bFailed = false;
	for (int32 WorkIndex = ActiveWork.Num() - 1; WorkIndex >= 0; --WorkIndex)
	{
		bool bFinished = false;
		if (!AdvanceDoor(
				Context.BuildingSubsystem,
				ActiveWork[WorkIndex],
				CurrentServerTimeSeconds,
				bFinished))
		{
			bFailed = true;
			continue;
		}
		if (bFinished)
		{
			ActiveEntities.Remove(ActiveWork[WorkIndex].Entity);
			ActiveWork.RemoveAtSwap(WorkIndex, 1, EAllowShrinking::No);
		}
	}

	if (bFailed)
	{
		return EBuildProcessorRunResult::Failed;
	}
	return ActiveWork.IsEmpty()
		? EBuildProcessorRunResult::Done
		: EBuildProcessorRunResult::RetryNextFrame;
}

bool FBuildDoorProcessor::AdvanceDoor(
	UBuildingWorldSubsystem& BuildingSubsystem,
	FDoorWork& Work,
	const double CurrentServerTimeSeconds,
	bool& bOutFinished)
{
	bOutFinished = false;
	FBuildEntityRegistry& Registry = BuildingSubsystem.GetRegistry();
	if (!Registry.IsAlive(Work.Entity))
	{
		bOutFinished = true;
		return true;
	}

	FBuildDoorStateFragment* DoorState =
		Registry.FindMutableFragment<FBuildDoorStateFragment>(Work.Entity);
	if (!DoorState)
	{
		bOutFinished = true;
		return true;
	}

	if (Work.bInteractionPending)
	{
		if (DoorState->State != EBuildDoorState::Closed
			&& DoorState->State != EBuildDoorState::Open)
		{
			bOutFinished = true;
			return true;
		}
		if (!BuildingSubsystem.SetPresentationMotionActive(Work.Entity, true))
		{
			return false;
		}
		Work.bMotionActive = true;

		DoorState->State = DoorState->State == EBuildDoorState::Closed
			? EBuildDoorState::Opening
			: EBuildDoorState::Closing;
		DoorState->TransitionStartServerTimeSeconds = CurrentServerTimeSeconds;
		Work.bInteractionPending = false;
		Work.bStateChangePending = true;
	}

	const bool bTransitioning = DoorState->State == EBuildDoorState::Opening
		|| DoorState->State == EBuildDoorState::Closing;
	if (bTransitioning && !Work.bMotionActive)
	{
		if (!BuildingSubsystem.SetPresentationMotionActive(Work.Entity, true))
		{
			return false;
		}
		Work.bMotionActive = true;
	}

	if (!bTransitioning)
	{
		const float StableOpenFraction = DoorState->State == EBuildDoorState::Open
			? 1.0f
			: 0.0f;
		if (!ApplyDoorPartTransforms(
				BuildingSubsystem,
				Work.Entity,
				StableOpenFraction))
		{
			return false;
		}
		if (Work.bStateChangePending)
		{
			StateChanged.ExecuteIfBound(Work.Entity);
			Work.bStateChangePending = false;
		}
		if (Work.bMotionActive
			&& !BuildingSubsystem.SetPresentationMotionActive(Work.Entity, false))
		{
			return false;
		}
		Work.bMotionActive = false;
		bOutFinished = true;
		return true;
	}

	const FBuildDoorStateFragment PreviousState = *DoorState;
	const double ElapsedSeconds = FMath::Max(
		CurrentServerTimeSeconds - DoorState->TransitionStartServerTimeSeconds,
		0.0);
	const float TransitionFraction = FMath::Clamp(
		static_cast<float>(ElapsedSeconds / DoorTransitionDuration),
		0.0f,
		1.0f);
	const float OpenFraction = DoorState->State == EBuildDoorState::Opening
		? TransitionFraction
		: 1.0f - TransitionFraction;
	bool bTransitionFinished = false;
	if (DoorState->State == EBuildDoorState::Opening
		&& TransitionFraction >= 1.0f)
	{
		DoorState->State = EBuildDoorState::Open;
		DoorState->TransitionStartServerTimeSeconds = 0.0;
		bTransitionFinished = true;
		Work.bStateChangePending = true;
	}
	else if (DoorState->State == EBuildDoorState::Closing
		&& TransitionFraction >= 1.0f)
	{
		DoorState->State = EBuildDoorState::Closed;
		DoorState->TransitionStartServerTimeSeconds = 0.0;
		bTransitionFinished = true;
		Work.bStateChangePending = true;
	}

	FBuildPartTransformFragment* PartTransforms =
		Registry.FindMutableFragment<FBuildPartTransformFragment>(Work.Entity);
	TArray<FTransform, TInlineAllocator<UE_ARRAY_COUNT(MovingDoorPartIds)>>
		PreviousMovingPartTransforms;
	if (PartTransforms)
	{
		PreviousMovingPartTransforms.Reserve(UE_ARRAY_COUNT(MovingDoorPartIds));
		for (const int32 PartId : MovingDoorPartIds)
		{
			if (PartTransforms->LocalTransforms.IsValidIndex(PartId))
			{
				PreviousMovingPartTransforms.Add(
					PartTransforms->LocalTransforms[PartId]);
			}
		}
	}
	if (!ApplyDoorPartTransforms(
			BuildingSubsystem,
			Work.Entity,
			OpenFraction))
	{
		*DoorState = PreviousState;
		if (PartTransforms
			&& PreviousMovingPartTransforms.Num()
				== UE_ARRAY_COUNT(MovingDoorPartIds))
		{
			for (int32 MovingIndex = 0;
				MovingIndex < UE_ARRAY_COUNT(MovingDoorPartIds);
				++MovingIndex)
			{
				PartTransforms->LocalTransforms[MovingDoorPartIds[MovingIndex]] =
					PreviousMovingPartTransforms[MovingIndex];
			}
		}
		return false;
	}
	if (Work.bStateChangePending)
	{
		StateChanged.ExecuteIfBound(Work.Entity);
		Work.bStateChangePending = false;
	}

	if (bTransitionFinished)
	{
		if (!BuildingSubsystem.SetPresentationMotionActive(Work.Entity, false))
		{
			return false;
		}
		Work.bMotionActive = false;
	}
	bOutFinished = bTransitionFinished;
	return true;
}

FTransform FBuildDoorProcessor::CalculateDoorMotion(const float OpenFraction)
{
	const float EasedOpenFraction = FMath::InterpEaseInOut(
		0.0f,
		1.0f,
		FMath::Clamp(OpenFraction, 0.0f, 1.0f),
		2.0f);
	const FQuat Rotation(
		FVector::UpVector,
		FMath::DegreesToRadians(DoorOpenAngleDegrees * EasedOpenFraction));
	return FTransform(
		Rotation,
		DoorHingeLocalLocation - Rotation.RotateVector(DoorHingeLocalLocation));
}

bool FBuildDoorProcessor::ApplyDoorPartTransforms(
	UBuildingWorldSubsystem& BuildingSubsystem,
	const FBuildEntityHandle Entity,
	const float OpenFraction)
{
	FBuildEntityRegistry& Registry = BuildingSubsystem.GetRegistry();
	FBuildPartTransformFragment* PartTransforms =
		Registry.FindMutableFragment<FBuildPartTransformFragment>(Entity);
	const FBuildDefinitionFragment* DefinitionFragment =
		Registry.FindFragment<FBuildDefinitionFragment>(Entity);
	const UBuildingDefinition* Definition = DefinitionFragment
		? DefinitionFragment->Definition.Get()
		: nullptr;
	if (!PartTransforms
		|| !Definition
		|| PartTransforms->LocalTransforms.Num() != Definition->MeshParts.Num())
	{
		return false;
	}

	const FTransform DoorMotion = CalculateDoorMotion(OpenFraction);
	for (const int32 PartId : MovingDoorPartIds)
	{
		if (!Definition->MeshParts.IsValidIndex(PartId))
		{
			return false;
		}
		PartTransforms->LocalTransforms[PartId] =
			Definition->MeshParts[PartId].LocalTransform * DoorMotion;
	}

	return BuildingSubsystem.CommitPartTransformChange(Entity, MovingDoorPartIds);
}
