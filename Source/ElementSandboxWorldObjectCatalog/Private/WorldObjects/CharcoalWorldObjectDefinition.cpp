#include "WorldObjects/CharcoalWorldObjectDefinition.h"

#include "Entity/WorldObjectEntityRegistry.h"
#include "Entity/WorldObjectPhysicsTypes.h"

UCharcoalWorldObjectDefinition::UCharcoalWorldObjectDefinition()
{
	DefinitionId = CharcoalWorldObjectDefinitionId;
	SpatialClass = EWorldObjectSpatialClass::Portable;
	InteractionLocalBounds = FBox(FVector(-16.0, -8.0, -6.5), FVector(16.0, 8.0, 6.5));
	SurfaceProfileId = TEXT("Surface.WorldObject.Charcoal");
	ShapeGeometry = FWorldObjectShapeDefinition::MakeObbFromBounds(InteractionLocalBounds);
	Destruction = {};
}

bool UCharcoalWorldObjectDefinition::ConfigureEntity(
	FWorldObjectEntityRegistry& Registry,
	const FWorldObjectEntityHandle Entity) const
{
	return Registry.AddFragment(Entity, FWorldObjectPhysicsBodyFragment());
}
