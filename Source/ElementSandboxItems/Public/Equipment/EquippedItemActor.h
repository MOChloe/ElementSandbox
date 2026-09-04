#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "EquippedItemActor.generated.h"

class USceneComponent;
class UStaticMeshComponent;

/** 道具 UObject 在世界中的复制投影；基类不提供攻击、光照或元素行为。 */
UCLASS()
class ELEMENTSANDBOXITEMS_API AEquippedItemActor : public AActor
{
	GENERATED_BODY()

public:
	AEquippedItemActor();

protected:
	UPROPERTY(VisibleAnywhere, Category="Item")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category="Item")
	TObjectPtr<UStaticMeshComponent> ItemMesh;

	UStaticMeshComponent* GetItemMesh() const { return ItemMesh; }
};
