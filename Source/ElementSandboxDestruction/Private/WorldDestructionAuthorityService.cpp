#include "WorldDestructionAuthorityService.h"

#include "BuildingWorldSubsystem.h"
#include "Definition/BuildingDefinition.h"
#include "Definition/WorldObjectDefinition.h"
#include "Definition/WorldDestructionDefinition.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Entity/BuildDamageFragment.h"
#include "Entity/BuildDefinitionFragment.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildPartTransformFragment.h"
#include "Entity/BuildTransformFragment.h"
#include "Entity/BuildWorldIdentityFragment.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectDamageFragment.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "PhysicsWorldObjectProductSink.h"
#include "Spatial/BuildSpatialIndex.h"
#include "Spatial/WorldObjectSpatialIndex.h"
#include "WorldObjectWorldSubsystem.h"

namespace UE::ElementSandbox::Destruction
{
namespace
{
	bool RaycastLocalBounds(
		const FBox& LocalBounds,
		const FTransform& WorldTransform,
		const FVector& WorldOrigin,
		const FVector& WorldUnitDirection,
		const double MaxDistance,
		double& OutDistance)
	{
		OutDistance = 0.0;
		if (LocalBounds.IsValid == 0 || LocalBounds.ContainsNaN()
			|| WorldTransform.ContainsNaN() || MaxDistance <= 0.0)
		{
			return false;
		}
		const FVector Scale = WorldTransform.GetScale3D();
		if (FMath::IsNearlyZero(Scale.X) || FMath::IsNearlyZero(Scale.Y)
			|| FMath::IsNearlyZero(Scale.Z))
		{
			return false;
		}

		const FVector WorldEnd = WorldOrigin + WorldUnitDirection * MaxDistance;
		const FVector LocalOrigin = WorldTransform.InverseTransformPosition(WorldOrigin);
		const FVector LocalDelta = WorldTransform.InverseTransformPosition(WorldEnd) - LocalOrigin;
		double EntryAlpha = 0.0;
		double ExitAlpha = 1.0;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (FMath::Abs(LocalDelta[Axis]) <= 1.0e-12)
			{
				if (LocalOrigin[Axis] < LocalBounds.Min[Axis]
					|| LocalOrigin[Axis] > LocalBounds.Max[Axis])
				{
					return false;
				}
				continue;
			}
			double NearAlpha = (LocalBounds.Min[Axis] - LocalOrigin[Axis]) / LocalDelta[Axis];
			double FarAlpha = (LocalBounds.Max[Axis] - LocalOrigin[Axis]) / LocalDelta[Axis];
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

	uint32 NextRevision(const uint32 Current)
	{
		return Current == MAX_uint32 ? 1 : Current + 1;
	}

	bool ApplyBuildingRequest(
		UWorld& World,
		UBuildingWorldSubsystem& Buildings,
		const FWorldDestructionRequest& Request)
	{
		FBuildEntityRegistry& Registry = Buildings.GetRegistry();
		const FWorldDestructionTarget& Target = Request.Target;
		if (!Registry.IsAlive(Target.Building))
		{
			return false;
		}
		const FBuildDefinitionFragment* DefinitionFragment =
			Registry.FindFragment<FBuildDefinitionFragment>(Target.Building);
		const FBuildWorldIdentityFragment* Identity =
			Registry.FindFragment<FBuildWorldIdentityFragment>(Target.Building);
		const UBuildingDefinition* Definition = DefinitionFragment
			? DefinitionFragment->Definition.Get()
			: nullptr;
		if (!Definition || !Definition->Destruction.IsEnabled()
			|| !Definition->Destruction.IsValid() || !Identity
			|| Identity->WorldEntityId != Target.WorldEntityId
			|| Identity->StateRevision != Target.SourceRevision)
		{
			return false;
		}

		FBuildDamageFragment* DamageFragment =
			Registry.FindMutableFragment<FBuildDamageFragment>(Target.Building);
		const bool bAdded = DamageFragment == nullptr;
		if (bAdded)
		{
			if (!Registry.AddFragment(Target.Building, FBuildDamageFragment()))
			{
				return false;
			}
			DamageFragment = Registry.FindMutableFragment<FBuildDamageFragment>(Target.Building);
		}
		check(DamageFragment);
		const FBuildDamageFragment Previous = *DamageFragment;
		DamageFragment->AccumulatedDamage = Request.DamageMode == EWorldDestructionDamageMode::ExhaustDurability
			? Definition->Destruction.MaxDurability
			: DamageFragment->AccumulatedDamage + Request.Damage;
		DamageFragment->DestructionRevision = NextRevision(DamageFragment->DestructionRevision);
		if (DamageFragment->AccumulatedDamage + UE_KINDA_SMALL_NUMBER
			< Definition->Destruction.MaxDurability)
		{
			return true;
		}

		FBox SourceBounds(ForceInit);
		if (!Buildings.GetSpatialIndex().TryGetBounds(Target.Building, SourceBounds))
		{
			*DamageFragment = Previous;
			if (bAdded)
			{
				Registry.RemoveFragment<FBuildDamageFragment>(Target.Building);
			}
			return false;
		}
		FWorldDestructionProductBatch Batch;
		Batch.Target = Target;
		Batch.SourceId = Identity->WorldEntityId;
		Batch.DestructionRevision = DamageFragment->DestructionRevision;
		Batch.SourceBounds = SourceBounds;
		Batch.Definition = &Definition->Destruction;
		Batch.LaunchContext = Request.LaunchContext.IsSet() ? &Request.LaunchContext.GetValue() : nullptr;
		if (!FWorldDestructionAuthorityService::TryConvertResolvedSource(
			World,
			Batch,
			[&Buildings, Entity = Target.Building]() { return Buildings.DestroyEntity(Entity); },
			[&Buildings, Entity = Target.Building]() { return Buildings.IsEntityAlive(Entity); },
			Request.ProductSink))
		{
			DamageFragment = Registry.FindMutableFragment<FBuildDamageFragment>(Target.Building);
			if (DamageFragment)
			{
				*DamageFragment = Previous;
				if (bAdded)
				{
					Registry.RemoveFragment<FBuildDamageFragment>(Target.Building);
				}
			}
			return false;
		}
		return true;
	}

	bool ApplyWorldObjectRequest(
		UWorld& World,
		UWorldObjectWorldSubsystem& WorldObjects,
		const FWorldDestructionRequest& Request)
	{
		FWorldObjectEntityRegistry& Registry = WorldObjects.GetRegistry();
		const FWorldDestructionTarget& Target = Request.Target;
		if (!Registry.IsAlive(Target.WorldObject))
		{
			return false;
		}
		const FWorldObjectDefinitionFragment* DefinitionFragment =
			Registry.FindFragment<FWorldObjectDefinitionFragment>(Target.WorldObject);
		const FWorldObjectWorldIdentityFragment* Identity =
			Registry.FindFragment<FWorldObjectWorldIdentityFragment>(Target.WorldObject);
		const UWorldObjectDefinition* Definition = DefinitionFragment
			? DefinitionFragment->Definition.Get()
			: nullptr;
		if (!Definition || !Definition->Destruction.IsEnabled()
			|| !Definition->Destruction.IsValid() || !Identity
			|| Identity->WorldEntityId != Target.WorldEntityId
			|| Identity->StateRevision != Target.SourceRevision)
		{
			return false;
		}

		FWorldObjectDamageFragment* DamageFragment =
			Registry.FindMutableFragment<FWorldObjectDamageFragment>(Target.WorldObject);
		const bool bAdded = DamageFragment == nullptr;
		if (bAdded)
		{
			if (!Registry.AddFragment(Target.WorldObject, FWorldObjectDamageFragment()))
			{
				return false;
			}
			DamageFragment = Registry.FindMutableFragment<FWorldObjectDamageFragment>(Target.WorldObject);
		}
		check(DamageFragment);
		const FWorldObjectDamageFragment Previous = *DamageFragment;
		DamageFragment->AccumulatedDamage = Request.DamageMode == EWorldDestructionDamageMode::ExhaustDurability
			? Definition->Destruction.MaxDurability
			: DamageFragment->AccumulatedDamage + Request.Damage;
		DamageFragment->DestructionRevision = NextRevision(DamageFragment->DestructionRevision);
		if (DamageFragment->AccumulatedDamage + UE_KINDA_SMALL_NUMBER
			< Definition->Destruction.MaxDurability)
		{
			return true;
		}

		FBox SourceBounds(ForceInit);
		if (!WorldObjects.GetSpatialIndex().TryGetBounds(Target.WorldObject, SourceBounds))
		{
			*DamageFragment = Previous;
			if (bAdded)
			{
				Registry.RemoveFragment<FWorldObjectDamageFragment>(Target.WorldObject);
			}
			return false;
		}
		FWorldDestructionProductBatch Batch;
		Batch.Target = Target;
		Batch.SourceId = Identity->WorldEntityId;
		Batch.DestructionRevision = DamageFragment->DestructionRevision;
		Batch.SourceBounds = SourceBounds;
		Batch.Definition = &Definition->Destruction;
		Batch.LaunchContext = Request.LaunchContext.IsSet() ? &Request.LaunchContext.GetValue() : nullptr;
		if (!FWorldDestructionAuthorityService::TryConvertResolvedSource(
			World,
			Batch,
			[&WorldObjects, Entity = Target.WorldObject]() { return WorldObjects.DestroyEntity(Entity); },
			[&WorldObjects, Entity = Target.WorldObject]() { return WorldObjects.IsEntityAlive(Entity); },
			Request.ProductSink))
		{
			DamageFragment = Registry.FindMutableFragment<FWorldObjectDamageFragment>(Target.WorldObject);
			if (DamageFragment)
			{
				*DamageFragment = Previous;
				if (bAdded)
				{
					Registry.RemoveFragment<FWorldObjectDamageFragment>(Target.WorldObject);
				}
			}
			return false;
		}
		return true;
	}
}

bool FWorldDestructionAuthorityService::TryResolveNearestTarget(
	UWorld& World,
	const FVector& ViewOrigin,
	const FVector& UnitDirection,
	const FVector& InstigatorLocation,
	const double FocusDistance,
	FWorldDestructionTarget& OutTarget)
{
	OutTarget = {};
	if (World.GetNetMode() == NM_Client || ViewOrigin.ContainsNaN()
		|| UnitDirection.ContainsNaN() || UnitDirection.IsNearlyZero()
		|| InstigatorLocation.ContainsNaN() || !FMath::IsFinite(FocusDistance)
		|| FocusDistance <= 0.0)
	{
		return false;
	}
	const FVector Direction = UnitDirection.GetSafeNormal();
	const double MaxRayDistance = FVector::Distance(ViewOrigin, InstigatorLocation) + FocusDistance;
	const double FocusDistanceSquared = FMath::Square(FocusDistance);
	double BestDistance = MaxRayDistance + 1.0;

	if (UBuildingWorldSubsystem* Buildings = World.GetSubsystem<UBuildingWorldSubsystem>())
	{
		FBuildSpatialQueryScratch Scratch;
		TArray<FBuildSpatialRayHit> Hits;
		Buildings->GetSpatialIndex().QueryRay(ViewOrigin, Direction, MaxRayDistance, Scratch, Hits);
		const FBuildEntityRegistry& Registry = Buildings->GetRegistry();
		for (const FBuildSpatialRayHit& Hit : Hits)
		{
			if (Hit.Distance > BestDistance) break;
			const FBuildDefinitionFragment* DefinitionFragment = Registry.FindFragment<FBuildDefinitionFragment>(Hit.Entity);
			const FBuildTransformFragment* Transform = Registry.FindFragment<FBuildTransformFragment>(Hit.Entity);
			const FBuildPartTransformFragment* PartTransforms = Registry.FindFragment<FBuildPartTransformFragment>(Hit.Entity);
			const FBuildWorldIdentityFragment* Identity = Registry.FindFragment<FBuildWorldIdentityFragment>(Hit.Entity);
			const UBuildingDefinition* Definition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
			if (!Definition || !Transform || !Identity || !Identity->WorldEntityId.IsSet()) continue;
			double EntityDistance = MaxRayDistance + 1.0;
			for (int32 PartId = 0; PartId < Definition->MeshParts.Num(); ++PartId)
			{
				const FBuildMeshPartDefinition& Part = Definition->MeshParts[PartId];
				if (!Part.Mesh) continue;
				const FTransform& LocalTransform = PartTransforms && PartTransforms->LocalTransforms.IsValidIndex(PartId)
					? PartTransforms->LocalTransforms[PartId] : Part.LocalTransform;
				double PartDistance = 0.0;
				if (RaycastLocalBounds(Part.Mesh->GetBoundingBox(), LocalTransform * Transform->WorldTransform,
					ViewOrigin, Direction, MaxRayDistance, PartDistance))
				{
					EntityDistance = FMath::Min(EntityDistance, PartDistance);
				}
			}
			if (EntityDistance >= BestDistance
				|| FVector::DistSquared(InstigatorLocation, ViewOrigin + Direction * EntityDistance) > FocusDistanceSquared)
			{
				continue;
			}
			BestDistance = EntityDistance;
			OutTarget = {};
			if (Definition->Destruction.IsEnabled())
			{
				OutTarget.Domain = EWorldDestructionTargetDomain::Building;
				OutTarget.Building = Hit.Entity;
				OutTarget.WorldEntityId = Identity->WorldEntityId;
				OutTarget.SourceRevision = Identity->StateRevision;
				OutTarget.Distance = EntityDistance;
			}
		}
	}

	if (UWorldObjectWorldSubsystem* WorldObjects = World.GetSubsystem<UWorldObjectWorldSubsystem>())
	{
		FWorldObjectSpatialQueryScratch Scratch;
		TArray<FWorldObjectSpatialRayHit> Hits;
		WorldObjects->QueryRay(ViewOrigin, Direction, MaxRayDistance, Scratch, Hits);
		const FWorldObjectEntityRegistry& Registry = WorldObjects->GetRegistry();
		for (const FWorldObjectSpatialRayHit& Hit : Hits)
		{
			if (Hit.Distance >= BestDistance) break;
			const FWorldObjectDefinitionFragment* DefinitionFragment = Registry.FindFragment<FWorldObjectDefinitionFragment>(Hit.Entity);
			const FWorldObjectTransformFragment* Transform = Registry.FindFragment<FWorldObjectTransformFragment>(Hit.Entity);
			const FWorldObjectWorldIdentityFragment* Identity = Registry.FindFragment<FWorldObjectWorldIdentityFragment>(Hit.Entity);
			const FWorldObjectInstanceInteractionBoundsFragment* InstanceBounds = Registry.FindFragment<FWorldObjectInstanceInteractionBoundsFragment>(Hit.Entity);
			const UWorldObjectDefinition* Definition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
			if (!Definition || !Transform || !Identity || !Identity->WorldEntityId.IsSet()) continue;
			double EntityDistance = 0.0;
			const FBox& Bounds = InstanceBounds ? InstanceBounds->InteractionLocalBounds : Definition->InteractionLocalBounds;
			if (!RaycastLocalBounds(Bounds, Transform->WorldTransform, ViewOrigin, Direction, MaxRayDistance, EntityDistance)
				|| EntityDistance >= BestDistance
				|| FVector::DistSquared(InstigatorLocation, ViewOrigin + Direction * EntityDistance) > FocusDistanceSquared)
			{
				continue;
			}
			BestDistance = EntityDistance;
			OutTarget = {};
			if (Definition->Destruction.IsEnabled())
			{
				OutTarget.Domain = EWorldDestructionTargetDomain::WorldObject;
				OutTarget.WorldObject = Hit.Entity;
				OutTarget.WorldEntityId = Identity->WorldEntityId;
				OutTarget.SourceRevision = Identity->StateRevision;
				OutTarget.Distance = EntityDistance;
			}
		}
	}
	return OutTarget.IsSet();
}

bool FWorldDestructionAuthorityService::TryApplyRequest(
	UWorld& World,
	const FWorldDestructionRequest& Request)
{
	if (World.GetNetMode() == NM_Client || !Request.IsValid())
	{
		return false;
	}
	if (Request.Target.Domain == EWorldDestructionTargetDomain::Building)
	{
		UBuildingWorldSubsystem* Buildings = World.GetSubsystem<UBuildingWorldSubsystem>();
		return Buildings && ApplyBuildingRequest(World, *Buildings, Request);
	}
	UWorldObjectWorldSubsystem* WorldObjects = World.GetSubsystem<UWorldObjectWorldSubsystem>();
	return Request.Target.Domain == EWorldDestructionTargetDomain::WorldObject
		&& WorldObjects && ApplyWorldObjectRequest(World, *WorldObjects, Request);
}

bool FWorldDestructionAuthorityService::TryConvertResolvedSource(
	UWorld& World,
	const FWorldDestructionProductBatch& Batch,
	const TFunctionRef<bool()> DestroySource,
	const TFunctionRef<bool()> IsSourceAlive,
	IWorldDestructionProductSink* ProductSink)
{
	if (World.GetNetMode() == NM_Client || !Batch.IsValid())
	{
		return false;
	}
	UWorldObjectWorldSubsystem* WorldObjects = World.GetSubsystem<UWorldObjectWorldSubsystem>();
	if (!WorldObjects)
	{
		return false;
	}
	FPhysicsWorldObjectProductSink DefaultSink(*WorldObjects);
	IWorldDestructionProductSink& Sink = ProductSink ? *ProductSink : DefaultSink;
	if (!Sink.Prepare(Batch))
	{
		return false;
	}
	if (!DestroySource() && IsSourceAlive())
	{
		Sink.Rollback();
		return false;
	}
	Sink.Commit();
	return true;
}
}
