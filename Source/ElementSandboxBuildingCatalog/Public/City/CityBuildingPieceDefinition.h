#pragma once

#include "CoreMinimal.h"
#include "Definition/BuildingDefinition.h"

#include "CityBuildingPieceDefinition.generated.h"

class UMaterialInterface;
class UStaticMesh;

/** 离线种子配方映射到的普通 Building Definition 种类。 */
UENUM()
enum class ECityBuildingPieceKind : uint8
{
	SolidBox,
	DecorativeBox,
	SolidSphere,
	DecorativeSphere,
	Door
};

/** 返回非 Door 城市部件的稳定共享 Definition 顺序。 */
ELEMENTSANDBOXBUILDINGCATALOG_API TConstArrayView<ECityBuildingPieceKind> GetDefaultCityPrimitivePieceKinds();

/** 返回城市配方当前使用的中性 Surface Profile 顺序。 */
ELEMENTSANDBOXBUILDINGCATALOG_API TConstArrayView<FName> GetDefaultCityPieceSurfaceProfileIds();

/** 返回 Kind+Surface 共享 primitive Building Definition 的稳定网络 ID；无效组合返回 NAME_None。 */
ELEMENTSANDBOXBUILDINGCATALOG_API FName GetCityBuildingPieceDefinitionId(
	ECityBuildingPieceKind Kind,
	FName SurfaceProfileId);

/**
 * 城市配方部件共用的单 Mesh Building Definition。
 *
 * Entity Transform 直接承载配方部件的位置、旋转和尺寸，因此每个实例只有一个
 * Mesh Part；Solid 类型另有一个同 Transform 的 Simple Collision 代理。所有类型
 * 燃烧资格由 Catalog 显式声明；Cold 实体不分配 FireState，渲染仍按相同
 * Mesh/Material 跨 Entity 合批。
 */
UCLASS(NotBlueprintable)
class ELEMENTSANDBOXBUILDINGCATALOG_API UCityBuildingPieceDefinition final : public UBuildingDefinition
{
	GENERATED_BODY()

public:
	static constexpr int32 BurnAmountCustomDataIndex = 0;
	static constexpr int32 CustomDataFloatCount = 1;

	UCityBuildingPieceDefinition();
	bool Initialize(ECityBuildingPieceKind InKind, FName InSurfaceProfileId);

	ECityBuildingPieceKind GetPieceKind() const { return PieceKind; }
	FName GetSurfaceProfileId() const { return SurfaceProfileId; }
	int64 GetEstimatedTriangleCountPerInstance() const;

protected:
	virtual bool ConfigureEntity(FBuildEntityRegistry& Registry, FBuildEntityHandle Entity) const override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> CubeMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> SphereMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> BurnableMaterial = nullptr;

	UPROPERTY(VisibleAnywhere, Category = "Settlement")
	ECityBuildingPieceKind PieceKind = ECityBuildingPieceKind::SolidBox;

	UPROPERTY(VisibleAnywhere, Category = "Settlement")
	FName SurfaceProfileId = NAME_None;

};
