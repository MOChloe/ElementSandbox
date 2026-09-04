#pragma once

#include "CoreMinimal.h"
#include "Entity/WorldObjectFragment.h"
#include "Entity/WorldEntityId.h"
#include "Entity/WorldObjectTypes.h"
#include "Shape/WorldObjectShapeTypes.h"
#include "UObject/StrongObjectPtr.h"

#include "WorldObjectCoreFragments.generated.h"

class UWorldObjectDefinition;

USTRUCT()
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectTransformFragment final
	: public FWorldObjectFragment
{
	GENERATED_BODY()

	FTransform WorldTransform = FTransform::Identity;
	uint64 Revision = 1;
};

USTRUCT()
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectDefinitionFragment final
	: public FWorldObjectFragment
{
	GENERATED_BODY()

	TStrongObjectPtr<const UWorldObjectDefinition> Definition;
};

USTRUCT()
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectMotionFragment final
	: public FWorldObjectFragment
{
	GENERATED_BODY()

	EWorldObjectMotionState State = EWorldObjectMotionState::Dormant;
};

USTRUCT()
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectWorldIdentityFragment final
	: public FWorldObjectFragment
{
	GENERATED_BODY()

	FWorldEntityId WorldEntityId;
	uint32 StateRevision = 1;
};

/** 该实例独有的交互/空间查询 Bounds；存在时覆盖 Definition 的共享包络。 */
USTRUCT()
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectInstanceInteractionBoundsFragment final
	: public FWorldObjectFragment
{
	GENERATED_BODY()

	FBox InteractionLocalBounds = FBox(ForceInit);
	uint64 Revision = 1;
};

/** 实例覆盖几何的显式纯值 Shape；不得由 Worker 从表现资产或 Chaos 反推。 */
USTRUCT()
struct ELEMENTSANDBOXWORLDOBJECTS_API FWorldObjectInstanceShapeFragment final
	: public FWorldObjectFragment
{
	GENERATED_BODY()

	FWorldObjectShapeDefinition ShapeGeometry;
	uint64 Revision = 1;
};
