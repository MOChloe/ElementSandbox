#pragma once

#include "City/CityBuildingPieceDefinition.h"
#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "MillionBuildingRecipe.generated.h"

/** 聚落生成器可选择的完整结构配方。结构本身不是 Building Entity。 */
UENUM()
enum class ECityBuildingArchetype : uint8
{
	TimberCottage,
	StoneCottage,
	Farmhouse,
	Longhouse,
	Barn,
	Smithy,
	Tavern,
	MarketHall,
	Watchtower,
	PalisadeGate,
	Windmill,
	VillageShrine
};

/** 完整结构配方中的一个独立可交互、可燃烧、可拥有生命周期的建筑部件。 */
USTRUCT()
struct FCityBuildingPieceRecipe final
{
	GENERATED_BODY()

	UPROPERTY()
	ECityBuildingPieceKind Kind = ECityBuildingPieceKind::SolidBox;

	/** 中性内容表面标签；不表示可燃、可碰撞或任何 Element 资格。 */
	UPROPERTY()
	FName SurfaceProfileId = NAME_None;

	/** 相对完整结构地面原点的 Transform；最终直接成为该部件 Entity 的 World Transform。 */
	UPROPERTY()
	FTransform LocalTransform = FTransform::Identity;

	bool IsValid() const;
	bool IsDoor() const { return Kind == ECityBuildingPieceKind::Door; }
};

/** 返回当前正式生成的十二种完整结构配方，数组顺序是跨端确定性顺序。 */
TConstArrayView<ECityBuildingArchetype> GetDefaultCityBuildingArchetypes();

/** 完整结构配方的稳定标识；它不是 UBuildingDefinition::DefinitionId。 */
FName GetCityBuildingRecipeId(ECityBuildingArchetype Archetype);

/**
 * 一栋完整结构的纯生成配方。
 *
 * 此类型只编译进 ElementSandboxEditor，运行时模块看不到配方。Parts 中每一项都会
 * 展开成独立 Building Entity；门项复用 Settlement.Door，其余项映射到共享 Definition。
 */
UCLASS(NotBlueprintable)
class UCityBuildingRecipe final : public UObject
{
	GENERATED_BODY()

public:
	bool Initialize(ECityBuildingArchetype InArchetype);

	ECityBuildingArchetype GetArchetype() const { return Archetype; }
	FName GetRecipeId() const { return RecipeId; }
	const FText& GetDisplayName() const { return DisplayName; }
	const FVector2D& GetNominalFootprintCentimeters() const { return NominalFootprintCentimeters; }
	double GetNominalHeightCentimeters() const { return NominalHeightCentimeters; }
	TConstArrayView<FCityBuildingPieceRecipe> GetPieces() const { return Pieces; }
	/** 配方正立面上的挂墙火把插槽；每项会展开成独立 Building 形态。 */
	TConstArrayView<FTransform> GetMountedTorchLocalTransforms() const { return MountedTorchLocalTransforms; }
	int32 GetPieceEntityCount() const { return Pieces.Num(); }
	int32 GetMountedTorchEntityCount() const { return MountedTorchLocalTransforms.Num(); }
	int32 GetDoorEntityCount() const;
	/** 离线验证所有部件可追溯到地面，且每个火把插槽落在已接地构件表面。 */
	bool ValidateAssemblyGeometry(FString* OutError = nullptr) const;

private:
	UPROPERTY(VisibleAnywhere, Category = "Settlement")
	FName RecipeId = NAME_None;

	UPROPERTY(VisibleAnywhere, Category = "Settlement")
	ECityBuildingArchetype Archetype = ECityBuildingArchetype::TimberCottage;

	UPROPERTY(VisibleAnywhere, Category = "Settlement")
	FText DisplayName;

	UPROPERTY(VisibleAnywhere, Category = "Settlement")
	FVector2D NominalFootprintCentimeters = FVector2D::ZeroVector;

	UPROPERTY(VisibleAnywhere, Category = "Settlement")
	double NominalHeightCentimeters = 0.0;

	UPROPERTY(VisibleAnywhere, Category = "Settlement")
	TArray<FCityBuildingPieceRecipe> Pieces;

	UPROPERTY(VisibleAnywhere, Category = "Settlement")
	TArray<FTransform> MountedTorchLocalTransforms;
};
