#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"

#include "TorchDefinition.generated.h"

/** 同一火把内容在不同所有权与生命周期下可选择的运行形态。 */
UENUM(BlueprintType)
enum class ETorchForm : uint8
{
	InventoryItem,
	Equipped,
	MountedBuilding,
	PortableWorldObject
};

/** 一个火把形态到对应领域 DefinitionId 的稳定配置。 */
USTRUCT(BlueprintType)
struct ELEMENTSANDBOXBUILDINGCATALOG_API FTorchFormBinding final
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category="Torch")
	ETorchForm Form = ETorchForm::MountedBuilding;

	UPROPERTY(EditDefaultsOnly, Category="Torch")
	FName DefinitionId = NAME_None;
};

/**
 * 火把内容原型，而不是某个领域中的已放置实例。
 *
 * Forms 只登记已经存在的生产形态；跨域转换读取同一份配置并显式搬运允许保留的
 * Gameplay 数据，不共享任一领域的本地 Handle。
 */
UCLASS(BlueprintType, NotBlueprintable)
class ELEMENTSANDBOXBUILDINGCATALOG_API UTorchDefinition final : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UTorchDefinition();

	bool IsValid(FString* OutError = nullptr) const;
	bool TryResolveForm(ETorchForm Form, FName& OutDefinitionId) const;

	UPROPERTY(EditDefaultsOnly, Category="Torch")
	FName ContentId = NAME_None;

	UPROPERTY(EditDefaultsOnly, Category="Torch")
	TArray<FTorchFormBinding> Forms;
};

/** C++ 默认火把内容当前已经实现的挂墙 Building 形态。 */
ELEMENTSANDBOXBUILDINGCATALOG_API FName GetMountedTorchBuildingDefinitionId();
