#include "Definition/BuildingDefinition.h"

#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildDefinitionFragment.h"
#include "Entity/BuildPartTransformFragment.h"
#include "Entity/BuildTransformFragment.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "Spatial/BuildSpatialEntry.h"

using namespace UE::ElementSandbox::Building::Private;

namespace
{
	/**
	 * FKAggregateGeom::CalcAABB collapses Sphere/Box/Capsule scale to the
	 * smallest axis. Chaos does not: it builds boxes with FKBoxElem::GetFinalScaled
	 * and therefore supports the non-uniform transforms used by Building parts.
	 * Calculate the same conservative bounds as the shapes that Chaos actually
	 * creates, otherwise a long wall can be physically beside the player while
	 * its activation bounds remain around the entity origin.
	 */
	FBox CalculateScaledSimpleCollisionBounds(
		const FKAggregateGeom& Geometry,
		const FTransform& WorldTransform)
	{
		const FVector Scale3D = WorldTransform.GetScale3D();
		FTransform RigidWorldTransform = WorldTransform;
		RigidWorldTransform.RemoveScaling();

		FBox Bounds(ForceInit);
		for (const FKSphereElem& Element : Geometry.SphereElems)
		{
			Bounds += Element.GetFinalScaled(Scale3D, FTransform::Identity)
				.CalcAABB(RigidWorldTransform, 1.0f);
		}
		for (const FKBoxElem& Element : Geometry.BoxElems)
		{
			Bounds += Element.GetFinalScaled(Scale3D, FTransform::Identity)
				.CalcAABB(RigidWorldTransform, 1.0f);
		}
		for (const FKSphylElem& Element : Geometry.SphylElems)
		{
			Bounds += Element.GetFinalScaled(Scale3D, FTransform::Identity)
				.CalcAABB(RigidWorldTransform, 1.0f);
		}
		for (const FKConvexElem& Element : Geometry.ConvexElems)
		{
			Bounds += Element.CalcAABB(RigidWorldTransform, Scale3D);
		}
		for (const FKTaperedCapsuleElem& Element : Geometry.TaperedCapsuleElems)
		{
			Bounds += Element.GetFinalScaled(Scale3D, FTransform::Identity)
				.CalcAABB(RigidWorldTransform, 1.0f);
		}
		for (const FKLevelSetElem& Element : Geometry.LevelSetElems)
		{
			Bounds += Element.CalcAABB(RigidWorldTransform, Scale3D);
		}
		for (const FKSkinnedLevelSetElem& Element : Geometry.SkinnedLevelSetElems)
		{
			Bounds += Element.CalcAABB(RigidWorldTransform, Scale3D);
		}
		for (const FKMLLevelSetElem& Element : Geometry.MLLevelSetElems)
		{
			Bounds += Element.CalcAABB(RigidWorldTransform, Scale3D);
		}
		for (const FKSkinnedTriangleMeshElem& Element : Geometry.SkinnedTriangleMeshElems)
		{
			Bounds += Element.CalcAABB(RigidWorldTransform, Scale3D);
		}
		return Bounds;
	}
}

FBuildEntityHandle UBuildingDefinition::CreateEntity(
	FBuildEntityRegistry& Registry,
	const FTransform& InitialWorldTransform) const
{
	if (!HasValidCollisionDefinition())
	{
		return {};
	}

	const FBuildEntityHandle Entity = Registry.CreateEntity();
	if (!Entity.IsSet())
	{
		return {};
	}

	FBuildTransformFragment TransformFragment;
	TransformFragment.WorldTransform = InitialWorldTransform;
	TransformFragment.CommittedWorldTransform = InitialWorldTransform;
	TransformFragment.Revision = 1;

	FBuildDefinitionFragment DefinitionFragment;
	DefinitionFragment.Definition.Reset(this);
	const bool bConfigured = Registry.AddFragment(Entity, TransformFragment)
		&& Registry.AddFragment(Entity, DefinitionFragment);
	if (!bConfigured || !ConfigureEntity(Registry, Entity))
	{
		Registry.DestroyEntity(Entity);
		return {};
	}

	if (FBuildPartTransformFragment* PartTransforms =
		Registry.FindMutableFragment<FBuildPartTransformFragment>(Entity))
	{
		if (PartTransforms->LocalTransforms.Num() != MeshParts.Num())
		{
			Registry.DestroyEntity(Entity);
			return {};
		}
		PartTransforms->CommittedLocalTransforms = PartTransforms->LocalTransforms;
		PartTransforms->Revisions.Init(1, MeshParts.Num());
	}

	return Entity;
}

bool UBuildingDefinition::TryCalculateWorldBounds(
	const FTransform& WorldTransform,
	FBox& OutWorldBounds) const
{
	return TryCalculateWorldBounds(WorldTransform, {}, OutWorldBounds);
}

bool UBuildingDefinition::TryCalculateCurrentVisualWorldBounds(
	const FTransform& WorldTransform,
	const TConstArrayView<FTransform> PartLocalTransforms,
	FBox& OutWorldBounds) const
{
	OutWorldBounds = FBox(ForceInit);
	if (WorldTransform.ContainsNaN()
		|| (!PartLocalTransforms.IsEmpty()
			&& PartLocalTransforms.Num() != MeshParts.Num()))
	{
		return false;
	}

	bool bHasBounds = false;
	for (int32 PartId = 0; PartId < MeshParts.Num(); ++PartId)
	{
		const FBuildMeshPartDefinition& Part = MeshParts[PartId];
		if (!Part.Mesh)
		{
			continue;
		}
		const FTransform& PartLocalTransform = PartLocalTransforms.IsEmpty()
			? Part.LocalTransform
			: PartLocalTransforms[PartId];
		if (PartLocalTransform.ContainsNaN())
		{
			OutWorldBounds = FBox(ForceInit);
			return false;
		}
		const FBox LocalBounds = Part.Mesh->GetBoundingBox();
		const FBox PartWorldBounds = LocalBounds.TransformBy(
			PartLocalTransform * WorldTransform);
		if (!IsValidSpatialBounds(LocalBounds)
			|| !IsValidSpatialBounds(PartWorldBounds))
		{
			OutWorldBounds = FBox(ForceInit);
			return false;
		}
		OutWorldBounds += PartWorldBounds;
		bHasBounds = true;
	}
	return bHasBounds && IsValidSpatialBounds(OutWorldBounds);
}

bool UBuildingDefinition::TryCalculateWorldBounds(
	const FTransform& WorldTransform,
	const TConstArrayView<FTransform> PartLocalTransforms,
	FBox& OutWorldBounds) const
{
	OutWorldBounds = FBox(ForceInit);
	if (WorldTransform.ContainsNaN()
		|| (!PartLocalTransforms.IsEmpty()
			&& PartLocalTransforms.Num() != MeshParts.Num()))
	{
		return false;
	}

	bool bHasBounds = false;
	for (int32 PartId = 0; PartId < MeshParts.Num(); ++PartId)
	{
		const FBuildMeshPartDefinition& Part = MeshParts[PartId];
		if (!Part.Mesh)
		{
			continue;
		}

		const FTransform& PartLocalTransform = PartLocalTransforms.IsEmpty()
			? Part.LocalTransform
			: PartLocalTransforms[PartId];
		if (PartLocalTransform.ContainsNaN())
		{
			OutWorldBounds = FBox(ForceInit);
			return false;
		}

		const FBox LocalBounds = Part.Mesh->GetBoundingBox();
		const FTransform PartWorldTransform = PartLocalTransform * WorldTransform;
		const FBox PartWorldBounds = LocalBounds.TransformBy(PartWorldTransform);
		if (!IsValidSpatialBounds(LocalBounds)
			|| !IsValidSpatialBounds(PartWorldBounds))
		{
			OutWorldBounds = FBox(ForceInit);
			return false;
		}

		OutWorldBounds += PartWorldBounds;
		bHasBounds = true;
	}

	for (int32 CollisionPartId = 0;
		CollisionPartId < CollisionParts.Num();
		++CollisionPartId)
	{
		FBox CollisionWorldBounds(ForceInit);
		if (!TryCalculateCollisionPartWorldBounds(
			CollisionPartId,
			WorldTransform,
			PartLocalTransforms,
			CollisionWorldBounds))
		{
			OutWorldBounds = FBox(ForceInit);
			return false;
		}

		OutWorldBounds += CollisionWorldBounds;
		bHasBounds = true;
	}

	return bHasBounds && IsValidSpatialBounds(OutWorldBounds);
}

bool UBuildingDefinition::HasValidCollisionDefinition() const
{
	for (const FBuildCollisionPartDefinition& Part : CollisionParts)
	{
		if (!Part.CollisionMesh || Part.LocalTransform.ContainsNaN())
		{
			return false;
		}

		if (Part.DrivenMeshPartId != INDEX_NONE
			&& !MeshParts.IsValidIndex(Part.DrivenMeshPartId))
		{
			return false;
		}

		const UBodySetup* BodySetup = Part.CollisionMesh->GetBodySetup();
		const FKAggregateGeom* Geometry = BodySetup ? &BodySetup->AggGeom : nullptr;
		const int32 SupportedShapeCount = Geometry
			? Geometry->SphereElems.Num()
				+ Geometry->BoxElems.Num()
				+ Geometry->SphylElems.Num()
				+ Geometry->ConvexElems.Num()
			: 0;
		if (!BodySetup
			|| SupportedShapeCount == 0
			|| SupportedShapeCount != BodySetup->AggGeom.GetElementCount()
			|| BodySetup->CollisionTraceFlag == CTF_UseComplexAsSimple)
		{
			return false;
		}

		FCollisionResponseTemplate ProfileTemplate;
		if (!UCollisionProfile::Get()->GetProfileTemplate(
			Part.GetEffectiveCollisionProfileName(),
			ProfileTemplate))
		{
			return false;
		}
	}

	return true;
}

bool UBuildingDefinition::TryCalculateCollisionPartWorldTransform(
	const int32 CollisionPartId,
	const FTransform& WorldTransform,
	const TConstArrayView<FTransform> PartLocalTransforms,
	FTransform& OutPartWorldTransform) const
{
	OutPartWorldTransform = FTransform::Identity;
	if (!CollisionParts.IsValidIndex(CollisionPartId)
		|| WorldTransform.ContainsNaN()
		|| (!PartLocalTransforms.IsEmpty()
			&& PartLocalTransforms.Num() != MeshParts.Num()))
	{
		return false;
	}

	const FBuildCollisionPartDefinition& CollisionPart =
		CollisionParts[CollisionPartId];
	if (!CollisionPart.CollisionMesh || CollisionPart.LocalTransform.ContainsNaN())
	{
		return false;
	}

	FTransform ParentTransform = WorldTransform;
	if (CollisionPart.DrivenMeshPartId != INDEX_NONE)
	{
		if (!MeshParts.IsValidIndex(CollisionPart.DrivenMeshPartId))
		{
			return false;
		}

		const FTransform& MeshPartLocalTransform = PartLocalTransforms.IsEmpty()
			? MeshParts[CollisionPart.DrivenMeshPartId].LocalTransform
			: PartLocalTransforms[CollisionPart.DrivenMeshPartId];
		if (MeshPartLocalTransform.ContainsNaN())
		{
			return false;
		}

		ParentTransform = MeshPartLocalTransform * WorldTransform;
	}

	OutPartWorldTransform = CollisionPart.LocalTransform * ParentTransform;
	return !OutPartWorldTransform.ContainsNaN();
}

bool UBuildingDefinition::TryCalculateCollisionPartWorldBounds(
	const int32 CollisionPartId,
	const FTransform& WorldTransform,
	const TConstArrayView<FTransform> PartLocalTransforms,
	FBox& OutPartWorldBounds) const
{
	OutPartWorldBounds = FBox(ForceInit);
	FTransform PartWorldTransform;
	if (!TryCalculateCollisionPartWorldTransform(
		CollisionPartId,
		WorldTransform,
		PartLocalTransforms,
		PartWorldTransform))
	{
		return false;
	}

	const UStaticMesh* CollisionMesh = CollisionParts[CollisionPartId].CollisionMesh;
	const UBodySetup* BodySetup = CollisionMesh ? CollisionMesh->GetBodySetup() : nullptr;
	if (!BodySetup || BodySetup->AggGeom.GetElementCount() == 0)
	{
		return false;
	}

	OutPartWorldBounds = CalculateScaledSimpleCollisionBounds(
		BodySetup->AggGeom,
		PartWorldTransform);
	return IsValidSpatialBounds(OutPartWorldBounds);
}

bool UBuildingDefinition::DoPartTransformChangesAffectSpatialBounds(
	const TConstArrayView<int32> PartIds) const
{
	return !PartIds.IsEmpty();
}
