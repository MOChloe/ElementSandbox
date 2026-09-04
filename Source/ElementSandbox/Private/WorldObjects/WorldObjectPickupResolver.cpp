#include "WorldObjects/WorldObjectPickupResolver.h"

#include "BuildingWorldSubsystem.h"
#include "Definition/WorldObjectDefinition.h"
#include "Engine/World.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "GameFramework/Pawn.h"
#include "Item/ItemDefinition.h"
#include "Placement/BuildPlacementTypes.h"
#include "Projection/WorldObjectPhysicsProxyActor.h"
#include "Projection/WorldObjectProxyComponent.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldObjects/WorldObjectItemCatalogSubsystem.h"

namespace UE::ElementSandbox
{
	bool FWorldObjectPickupResolution::IsValid() const
	{
		return ::IsValid(ItemDefinition) && Quantity > 0
			&& InteractionLocalBounds.IsValid && !InteractionLocalBounds.ContainsNaN()
			&& InteractionLocalBounds.GetExtent().GetMin() >= 0.0
			&& !WorldTransform.ContainsNaN() && WorldTransform.GetRotation().IsNormalized()
			&& WorldTransform.GetScale3D().GetAbs().GetMin() > UE_SMALL_NUMBER;
	}

	bool FWorldObjectPickupResolution::RaycastInteractionBounds(
		const FVector& Origin, const FVector& Direction, const double MaxDistance, double& OutDistance) const
	{
		OutDistance = 0.0;
		if (!IsValid() || Origin.ContainsNaN() || Direction.ContainsNaN()
			|| !FMath::IsFinite(MaxDistance) || MaxDistance < 0.0) return false;
		const FVector UnitDirection = Direction.GetSafeNormal();
		if (UnitDirection.IsNearlyZero()) return false;
		const FVector LocalOrigin = WorldTransform.InverseTransformPosition(Origin);
		// 不能再次归一化 LocalDirection；保留缩放后，参数 t 仍是世界空间厘米。
		const FVector LocalDirection = WorldTransform.InverseTransformVector(UnitDirection);
		double Entry = 0.0, Exit = MaxDistance;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (FMath::Abs(LocalDirection[Axis]) <= 1.0e-12)
			{
				if (LocalOrigin[Axis] < InteractionLocalBounds.Min[Axis]
					|| LocalOrigin[Axis] > InteractionLocalBounds.Max[Axis]) return false;
				continue;
			}
			double Near = (InteractionLocalBounds.Min[Axis] - LocalOrigin[Axis]) / LocalDirection[Axis];
			double Far = (InteractionLocalBounds.Max[Axis] - LocalOrigin[Axis]) / LocalDirection[Axis];
			if (Near > Far) Swap(Near, Far);
			Entry = FMath::Max(Entry, Near);
			Exit = FMath::Min(Exit, Far);
			if (Entry > Exit) return false;
		}
		OutDistance = Entry;
		return true;
	}

	double FWorldObjectPickupResolution::ComputeSquaredInteractionDistance(const FVector& WorldPoint) const
	{
		if (!IsValid() || WorldPoint.ContainsNaN()) return TNumericLimits<double>::Max();
		return FVector::DistSquared(WorldPoint, ClosestInteractionPoint(WorldPoint));
	}

	FVector FWorldObjectPickupResolution::ClosestInteractionPoint(const FVector& WorldPoint) const
	{
		const FVector LocalPoint = WorldTransform.InverseTransformPosition(WorldPoint);
		return WorldTransform.TransformPosition(InteractionLocalBounds.GetClosestPointTo(LocalPoint));
	}

	bool TryResolveWorldObjectPickup(
		const UWorldObjectWorldSubsystem& WorldObjects,
		const FWorldObjectEntityHandle Entity,
		const UWorldObjectItemCatalogSubsystem& Catalog,
		FWorldObjectPickupResolution& OutResolution)
	{
		OutResolution = {};
		const FWorldObjectEntityRegistry& Registry = WorldObjects.GetRegistry();
		if (!Registry.IsAlive(Entity))
		{
			return false;
		}

		const FWorldObjectDefinitionFragment* Definition =
			Registry.FindFragment<FWorldObjectDefinitionFragment>(Entity);
		const FWorldObjectTransformFragment* Transform = Registry.FindFragment<FWorldObjectTransformFragment>(Entity);
		const FWorldObjectMotionFragment* Motion = Registry.FindFragment<FWorldObjectMotionFragment>(Entity);
		const FWorldObjectInstanceInteractionBoundsFragment* InstanceBounds =
			Registry.FindFragment<FWorldObjectInstanceInteractionBoundsFragment>(Entity);
		if (!Definition || !Definition->Definition.IsValid() || !Transform || !Motion
			|| Definition->Definition->SpatialClass != EWorldObjectSpatialClass::Portable
			|| (Motion->State != EWorldObjectMotionState::Dormant
				&& Motion->State != EWorldObjectMotionState::Physics)) return false;
		OutResolution.ItemDefinition = Catalog.FindItemDefinition(Definition->Definition.Get());
		OutResolution.Quantity = OutResolution.ItemDefinition ? 1 : 0;
		OutResolution.InteractionLocalBounds = InstanceBounds
			? InstanceBounds->InteractionLocalBounds : Definition->Definition->InteractionLocalBounds;
		OutResolution.WorldTransform = Transform->WorldTransform;
		const UWorldObjectProxyComponent* Proxy = WorldObjects.GetProxy(Entity);
		AActor* Actor = Proxy ? Proxy->GetOwner() : nullptr;
		OutResolution.ProjectionActor = Actor;
		if (Motion->State == EWorldObjectMotionState::Physics && IsValid(Actor))
		{
			if (const auto* PhysicsActor = Cast<AWorldObjectPhysicsProxyActor>(Actor))
			{
				// 自动代理原点是碰撞 Box 中心；只有完整接管后的客户端代理才覆盖 ECS。
				if (Actor->HasAuthority() || PhysicsActor->HasClientPhysicsProjection())
					OutResolution.WorldTransform = PhysicsActor->GetWorldObjectTransform();
			}
			else
			{
				OutResolution.WorldTransform = Actor->GetActorTransform();
			}
		}
		return OutResolution.IsValid();
	}

	bool HasClearWorldObjectPickupPath(const UWorld& World, const APawn& Pawn,
		const FWorldObjectPickupResolution& Pickup, const FVector& Origin)
	{
		if (!Pickup.IsValid() || Origin.ContainsNaN()) return false;
		const FVector Surface = Pickup.ClosestInteractionPoint(Origin);
		const double Length = FVector::Distance(Origin, Surface);
		if (Length <= 1.0) return true;
		// 终点留在物件表面前，避免贴地表面的数值误差把地面视为中途遮挡。
		const FVector End = Surface - (Surface - Origin) / Length;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(WorldObjectPickupVisibility), false, &Pawn);
		Params.AddIgnoredActor(Pickup.ProjectionActor.Get());
		TArray<AActor*> AttachedActors;
		Pawn.GetAttachedActors(AttachedActors, true, true);
		Params.AddIgnoredActors(AttachedActors);
		if (const auto* Buildings = World.GetSubsystem<UBuildingWorldSubsystem>())
		{
			FBuildPlacementSurfaceHit Hit;
			return !Buildings->QueryPlacementSurface(Origin, End, Params, Hit);
		}
		FCollisionObjectQueryParams Objects;
		Objects.AddObjectTypesToQuery(ECC_WorldStatic);
		Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
		FHitResult Hit;
		return !World.LineTraceSingleByObjectType(Hit, Origin, End, Objects, Params);
	}
}
