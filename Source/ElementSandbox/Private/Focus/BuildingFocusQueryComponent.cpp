#include "Focus/BuildingFocusQueryComponent.h"

#include "BuildingWorldSubsystem.h"
#include "ElementSandboxBuilding.h"
#include "Characters/ElementSandboxCharacter.h"
#include "Definition/BuildingDefinition.h"
#include "Door/DoorInteractionResolver.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildPartTransformFragment.h"
#include "Entity/BuildDefinitionFragment.h"
#include "Entity/BuildTransformFragment.h"
#include "Focus/BuildingFocusHandler.h"
#include "Focus/BuildingFocusTarget.h"
#include "Focus/FocusHostComponent.h"
#include "Game/ElementSandboxPlayerController.h"
#include "GameFramework/PlayerController.h"
#include "Spatial/BuildSpatialIndex.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"

namespace
{
	constexpr double MinimumDismantleFocusPartHalfExtentCentimeters = 18.0;
	// Local Box 旋转到世界轴后，单轴 18cm 扩张的保守 AABB 外沿不超过 18 * sqrt(3)。
	constexpr double DismantleFocusBroadphasePaddingCentimeters = 32.0;

	bool RaycastLocalBounds(
		const FBox& LocalBounds,
		const FTransform& WorldTransform,
		const FVector& WorldOrigin,
		const FVector& WorldUnitDirection,
		const double MaxDistance,
		const double MinimumWorldHalfExtent,
		double& OutDistance)
	{
		OutDistance = 0.0;
		if (LocalBounds.IsValid == 0
			|| LocalBounds.ContainsNaN()
			|| WorldTransform.ContainsNaN())
		{
			return false;
		}

		const FVector Scale = WorldTransform.GetScale3D();
		if (FMath::IsNearlyZero(Scale.X)
			|| FMath::IsNearlyZero(Scale.Y)
			|| FMath::IsNearlyZero(Scale.Z))
		{
			return false;
		}
		FBox FocusBounds = LocalBounds;
		if (MinimumWorldHalfExtent > 0.0)
		{
			// 只放宽拆除锤的命中包围盒，不改变 Building 空间索引、交互距离或服务器裁决。
			// 这样细杆、火焰等小 Mesh Part 不要求像素级瞄准，高亮仍投影真实 Mesh。
			const FVector AbsoluteScale = Scale.GetAbs();
			const FVector LocalExtent = LocalBounds.GetExtent();
			const FVector MinimumLocalExtent(
				MinimumWorldHalfExtent / AbsoluteScale.X,
				MinimumWorldHalfExtent / AbsoluteScale.Y,
				MinimumWorldHalfExtent / AbsoluteScale.Z);
			const FVector FocusExtent(
				FMath::Max(LocalExtent.X, MinimumLocalExtent.X),
				FMath::Max(LocalExtent.Y, MinimumLocalExtent.Y),
				FMath::Max(LocalExtent.Z, MinimumLocalExtent.Z));
			FocusBounds = FBox::BuildAABB(LocalBounds.GetCenter(), FocusExtent);
		}

		const FVector WorldEnd = WorldOrigin + WorldUnitDirection * MaxDistance;
		const FVector LocalOrigin = WorldTransform.InverseTransformPosition(WorldOrigin);
		const FVector LocalEnd = WorldTransform.InverseTransformPosition(WorldEnd);
		const FVector LocalDelta = LocalEnd - LocalOrigin;
		double EntryAlpha = 0.0;
		double ExitAlpha = 1.0;
		constexpr double ParallelTolerance = 1.0e-12;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double AxisOrigin = LocalOrigin[Axis];
			const double AxisDelta = LocalDelta[Axis];
			if (FMath::Abs(AxisDelta) <= ParallelTolerance)
			{
				if (AxisOrigin < FocusBounds.Min[Axis]
					|| AxisOrigin > FocusBounds.Max[Axis])
				{
					return false;
				}
				continue;
			}

			double NearAlpha = (FocusBounds.Min[Axis] - AxisOrigin) / AxisDelta;
			double FarAlpha = (FocusBounds.Max[Axis] - AxisOrigin) / AxisDelta;
			if (NearAlpha > FarAlpha)
			{
				Swap(NearAlpha, FarAlpha);
			}

			EntryAlpha = FMath::Max(EntryAlpha, NearAlpha);
			ExitAlpha = FMath::Min(ExitAlpha, FarAlpha);
			if (EntryAlpha > ExitAlpha)
			{
				return false;
			}
		}

		OutDistance = EntryAlpha * MaxDistance;
		return FMath::IsFinite(OutDistance);
	}
}

UBuildingFocusQueryComponent::UBuildingFocusQueryComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	Handler = CreateDefaultSubobject<UBuildingFocusHandler>(TEXT("BuildingFocusHandler"));
}

void UBuildingFocusQueryComponent::BeginPlay()
{
	Super::BeginPlay();

	APlayerController* PlayerController = Cast<APlayerController>(GetOwner());
	if (!PlayerController
		|| !PlayerController->IsLocalController()
		|| GetNetMode() == NM_DedicatedServer
		|| !IsValid(Handler))
	{
		return;
	}

	if (UFocusHostComponent* FocusHost =
		PlayerController->FindComponentByClass<UFocusHostComponent>())
	{
		RegistrationHandle = FocusHost->RegisterQuery(
			*this,
			FFocusQueryDelegate::CreateUObject(this, &UBuildingFocusQueryComponent::RunQuery),
			*Handler);
	}
}

void UBuildingFocusQueryComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (RegistrationHandle.IsSet())
	{
		if (UFocusHostComponent* FocusHost = GetOwner()
			? GetOwner()->FindComponentByClass<UFocusHostComponent>()
			: nullptr)
		{
			FocusHost->UnregisterQuery(RegistrationHandle);
		}
		RegistrationHandle = {};
	}

	Super::EndPlay(EndPlayReason);
}

void UBuildingFocusQueryComponent::RunQuery(
	const FFocusQueryContext& Context,
	TArray<FFocusQueryHit>& OutHits) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Building_Focus_Query);
	CSV_SCOPED_TIMING_STAT(ElementSandboxBuilding, FocusQuery);
	if (!Context.IsValid())
	{
		return;
	}

	const AElementSandboxPlayerController* PlayerController =
		Cast<AElementSandboxPlayerController>(GetOwner());
	const AElementSandboxCharacter* Character = PlayerController
		? Cast<AElementSandboxCharacter>(PlayerController->GetPawn())
		: nullptr;
	const double FocusDistance = Character ? Character->GetFocusDistance() : 0.0;
	if (!Character || FocusDistance <= 0.0)
	{
		return;
	}

	UWorld* World = GetWorld();
	const UBuildingWorldSubsystem* BuildingSubsystem = World
		? World->GetSubsystem<UBuildingWorldSubsystem>()
		: nullptr;
	if (!BuildingSubsystem)
	{
		return;
	}

	const FVector UnitDirection = Context.ViewDirection.GetSafeNormal();
	const FVector CharacterLocation = Character->GetActorLocation();
	const bool bDemolitionToolSelected = PlayerController->IsDemolitionToolSelected();
	const double MaxRayDistance =
		FVector::Distance(Context.ViewOrigin, CharacterLocation) + FocusDistance;
	if (bDemolitionToolSelected)
	{
		// 精确 Ray Broadphase 会先漏掉火把细杆等小目标，后续放宽 Part Bounds 也无从生效。
		// 拆除锤只在有限视线段周围取保守候选，最终顺序仍由下方逐 Part 窄相距离决定。
		FBox QueryBounds(ForceInit);
		QueryBounds += Context.ViewOrigin;
		QueryBounds += Context.ViewOrigin + UnitDirection * MaxRayDistance;
		QueryBounds = QueryBounds.ExpandBy(DismantleFocusBroadphasePaddingCentimeters);
		BuildingSubsystem->GetSpatialIndex().QueryOverlaps(
			QueryBounds,
			SpatialQueryScratch,
			BroadphaseOverlapEntities);
		BroadphaseHits.Reset(BroadphaseOverlapEntities.Num());
		for (const FBuildEntityHandle Entity : BroadphaseOverlapEntities)
		{
			BroadphaseHits.Add({Entity, 0.0});
		}
	}
	else
	{
		BuildingSubsystem->GetSpatialIndex().QueryRay(
			Context.ViewOrigin,
			UnitDirection,
			MaxRayDistance,
			SpatialQueryScratch,
			BroadphaseHits);
	}

	const FBuildEntityRegistry& Registry = BuildingSubsystem->GetRegistry();
	const double FocusDistanceSquared = FMath::Square(FocusDistance);
	double BestDistance = MaxRayDistance + 1.0;
	FBuildingFocusTarget BestTarget;
	for (const FBuildSpatialRayHit& BroadphaseHit : BroadphaseHits)
	{
		if (BroadphaseHit.Distance > BestDistance)
		{
			break;
		}

		const FBuildTransformFragment* TransformFragment =
			Registry.FindFragment<FBuildTransformFragment>(BroadphaseHit.Entity);
		const FBuildDefinitionFragment* DefinitionFragment =
			Registry.FindFragment<FBuildDefinitionFragment>(BroadphaseHit.Entity);
		const FBuildPartTransformFragment* PartTransforms =
			Registry.FindFragment<FBuildPartTransformFragment>(BroadphaseHit.Entity);
		const UBuildingDefinition* Definition = DefinitionFragment
			? DefinitionFragment->Definition.Get()
			: nullptr;
		if (!TransformFragment
			|| !Definition
			|| (PartTransforms
				&& PartTransforms->LocalTransforms.Num() != Definition->MeshParts.Num()))
		{
			continue;
		}

		EBuildDoorInteractionIntent InteractionIntent = EBuildDoorInteractionIntent::None;
		const bool bDoorInteractable = TryResolveBuildDoorInteraction(
			Registry,
			BroadphaseHit.Entity,
			InteractionIntent);
		// 拆除锤模式必须保留无返还映射的前景 Building，Handler 才能明确告诉玩家
		// “不可拆除”，而不是让射线穿过它或表现成左键完全失效。
		if (!bDoorInteractable && !bDemolitionToolSelected)
		{
			continue;
		}

		for (int32 PartId = 0; PartId < Definition->MeshParts.Num(); ++PartId)
		{
			const FBuildMeshPartDefinition& Part = Definition->MeshParts[PartId];
			if (!Part.Mesh)
			{
				continue;
			}

			double PartDistance = 0.0;
			const FTransform& PartLocalTransform = PartTransforms
				? PartTransforms->LocalTransforms[PartId]
				: Part.LocalTransform;
			const FTransform PartWorldTransform =
				PartLocalTransform * TransformFragment->WorldTransform;
			if (RaycastLocalBounds(
				Part.Mesh->GetBoundingBox(),
				PartWorldTransform,
				Context.ViewOrigin,
				UnitDirection,
				MaxRayDistance,
				bDemolitionToolSelected
					? MinimumDismantleFocusPartHalfExtentCentimeters
					: 0.0,
				PartDistance)
				&& PartDistance < BestDistance
				&& FVector::DistSquared(
					CharacterLocation,
					Context.ViewOrigin + UnitDirection * PartDistance)
					< FocusDistanceSquared)
			{
				BestDistance = PartDistance;
				BestTarget.Entity = BroadphaseHit.Entity;
				BestTarget.PartId = PartId;
			}
		}
	}

	if (!BestTarget.IsValid())
	{
		return;
	}

	FFocusQueryHit& FocusHit = OutHits.AddDefaulted_GetRef();
	FocusHit.HitDistance = BestDistance;
	FocusHit.HitLocation = Context.ViewOrigin + UnitDirection * BestDistance;
	FocusHit.Target = FInstancedStruct::Make<FBuildingFocusTarget>(BestTarget);
}
