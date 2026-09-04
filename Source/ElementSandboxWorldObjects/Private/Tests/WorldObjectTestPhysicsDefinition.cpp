#include "Tests/WorldObjectTestPhysicsDefinition.h"

#include "Entity/WorldObjectEntityRegistry.h"
#include "Entity/WorldObjectPhysicsTypes.h"

UWorldObjectTestPhysicsDefinition::UWorldObjectTestPhysicsDefinition()
{
	DefinitionId = TEXT("Test.Physics");
	SpatialClass = EWorldObjectSpatialClass::Portable;
	InteractionLocalBounds = FBox(FVector(-10.0), FVector(10.0));
	ShapeGeometry = FWorldObjectShapeDefinition::MakeObbFromBounds(InteractionLocalBounds);
}

bool UWorldObjectTestPhysicsDefinition::ConfigureEntity(
	FWorldObjectEntityRegistry& Registry,
	const FWorldObjectEntityHandle Entity) const
{
	return Registry.AddFragment(Entity, FWorldObjectPhysicsBodyFragment());
}
