#include "WorldObjects/WoodBlockWorldObjectDefinition.h"

#include "Entity/WorldObjectEntityRegistry.h"
#include "Entity/WorldObjectPhysicsTypes.h"

UWoodBlockWorldObjectDefinition::UWoodBlockWorldObjectDefinition()
{
	DefinitionId = WoodBlockWorldObjectDefinitionId;
	SpatialClass = EWorldObjectSpatialClass::Portable;
	InteractionLocalBounds = FBox(FVector(-70.0, -24.0, -20.0), FVector(70.0, 24.0, 20.0));
	SurfaceProfileId = TEXT("Surface.WorldObject.WoodBlock");
	ShapeGeometry = FWorldObjectShapeDefinition::MakeObbFromBounds(InteractionLocalBounds);
	Destruction = {};
}

bool UWoodBlockWorldObjectDefinition::ConfigureEntity(
	FWorldObjectEntityRegistry& Registry,
	const FWorldObjectEntityHandle Entity) const
{
	// 陨石落地直接创建 Dormant，不经过 Physics Sink；木块自身必须声明可接触唤醒的策略。
	FWorldObjectPhysicsBodyFragment PhysicsBody;
	PhysicsBody.CollisionPolicy = EWorldObjectPhysicsCollisionPolicy::LooseDebris;
	return Registry.AddFragment(Entity, PhysicsBody);
}
