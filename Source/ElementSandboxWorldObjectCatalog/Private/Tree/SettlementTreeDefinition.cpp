#include "Tree/SettlementTreeDefinition.h"

#include "Tree/SettlementTreeTypes.h"
#include "WorldObjects/WoodBlockWorldObjectDefinition.h"

USettlementTreeDefinition::USettlementTreeDefinition()
{
	DefinitionId = SettlementTreeDefinitionId;
	SpatialClass = EWorldObjectSpatialClass::PermanentStatic;
	InteractionLocalBounds = FBox(FVector(-220.0, -220.0, 0.0), FVector(220.0, 220.0, 620.0));
	SurfaceProfileId = TEXT("Surface.WorldObject.TreeWood");
	ShapeGeometry = FWorldObjectShapeDefinition::MakeObbFromBounds(InteractionLocalBounds);
	Destruction.MaxDurability = 100.0f;
	Destruction.ProductClass = UWoodBlockWorldObjectDefinition::StaticClass();
}
