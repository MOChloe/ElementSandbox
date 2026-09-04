#include "Tests/WorldObjectGameplayTestTypes.h"

#include "Entity/WorldObjectEntityRegistry.h"
#include "Entity/WorldObjectPhysicsTypes.h"

UWorldObjectGameplayTestPhysicsDefinition::
UWorldObjectGameplayTestPhysicsDefinition()
{
	DefinitionId = TEXT("Test.WorldObject.GameplayPhysics");
	SpatialClass = EWorldObjectSpatialClass::Portable;
	InteractionLocalBounds = FBox(FVector(-10.0), FVector(10.0));
	ShapeGeometry = FWorldObjectShapeDefinition::MakeObbFromBounds(InteractionLocalBounds);
}

bool UWorldObjectGameplayTestPhysicsDefinition::ConfigureEntity(
	FWorldObjectEntityRegistry& Registry,
	const FWorldObjectEntityHandle Entity) const
{
	return Registry.AddFragment(Entity, FWorldObjectPhysicsBodyFragment{});
}
