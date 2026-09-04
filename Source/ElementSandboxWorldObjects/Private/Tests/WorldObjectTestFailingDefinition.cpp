#include "Tests/WorldObjectTestFailingDefinition.h"

UWorldObjectTestFailingDefinition::UWorldObjectTestFailingDefinition()
{
	DefinitionId = TEXT("TestFailingConfigure");
	SpatialClass = EWorldObjectSpatialClass::Portable;
	InteractionLocalBounds = FBox(FVector(-10.0), FVector(10.0));
	ShapeGeometry = FWorldObjectShapeDefinition::MakeObbFromBounds(InteractionLocalBounds);
}

bool UWorldObjectTestFailingDefinition::ConfigureEntity(
	FWorldObjectEntityRegistry&,
	FWorldObjectEntityHandle) const
{
	return false;
}
