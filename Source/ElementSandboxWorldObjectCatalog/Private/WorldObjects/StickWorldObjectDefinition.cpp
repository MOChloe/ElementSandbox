#include "WorldObjects/StickWorldObjectDefinition.h"

#include "Entity/WorldObjectEntityRegistry.h"

UStickWorldObjectDefinition::UStickWorldObjectDefinition()
{
	DefinitionId = TEXT("Stick");
	SpatialClass = EWorldObjectSpatialClass::Portable;
	// Interaction bounds remain intentionally wider than the visible cylinder.
	InteractionLocalBounds = FBox(FVector(-8.0, -8.0, -38.0), FVector(8.0, 8.0, 38.0));
	SurfaceProfileId = TEXT("Surface.WorldObject.StickWood");
	ShapeGeometry.Kind = EWorldObjectShapeKind::Capsule;
	ShapeGeometry.Center = FVector::ZeroVector;
	ShapeGeometry.CapsuleAxis = FVector::UpVector;
	ShapeGeometry.Radius = 3.0;
	ShapeGeometry.CapsuleSegmentHalfLength = 35.0;
	ShapeGeometry.TemplateRevision = 1;
}

bool UStickWorldObjectDefinition::ConfigureEntity(
	FWorldObjectEntityRegistry& Registry,
	const FWorldObjectEntityHandle Entity) const
{
	return Registry.IsAlive(Entity);
}
