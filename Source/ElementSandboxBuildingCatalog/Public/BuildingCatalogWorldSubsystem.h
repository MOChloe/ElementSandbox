#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildEntityHandle.h"
#include "Processing/BuildProcessor.h"
#include "Subsystems/WorldSubsystem.h"

#include "BuildingCatalogWorldSubsystem.generated.h"

enum class ECityBuildingPieceKind : uint8;

class FBuildDoorProcessor;
class FBuildingCatalogPersistenceExtension;
class IBuildingPersistenceExtension;
class UBuildingWorldSubsystem;
class UBuildingDefinition;
class UCityBuildingPieceDefinition;
class UDoorBuildingDefinition;
class UFirePileBuildingDefinition;
class UTorchFixtureBuildingDefinition;
class UWoodBuildingDefinition;

/**
 * 每 World 的 Building Catalog 装配边界。
 *
 * Subsystem 自身不 Tick；Authority World 在这里注册 Door 权威 Processor，
 * Client World 注册同算法的只读投影 Processor，并由稀疏 DoorState Host 驱动。
 */
UCLASS()
class ELEMENTSANDBOXBUILDINGCATALOG_API UBuildingCatalogWorldSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool RequestDoorInteraction(FBuildEntityHandle Entity);
	bool HasAuthorityDoorProcessor() const;
	UBuildingDefinition* FindBuildingDefinition(FName DefinitionId) const;
	/** 将一个非 Door 配方部件按 Kind+Surface 解析到跨全部结构共享的单 Mesh Definition。 */
	UCityBuildingPieceDefinition* GetCityBuildingPieceDefinition(
		ECityBuildingPieceKind Kind,
		FName SurfaceProfileId) const;
	/** 聚落原型门洞共用的七部件确定性伴生门 Definition。 */
	UDoorBuildingDefinition* GetSettlementDoorDefinition() const { return SettlementDoorDefinition; }
	/** 低成本观测 Door Processor 是否在静止规模下被误唤醒。 */
	bool TryGetDoorProcessorStats(FBuildProcessorStats& OutStats) const;

protected:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

private:
	void HandleAuthorityDoorStateChanged(FBuildEntityHandle Entity);

	TWeakObjectPtr<UBuildingWorldSubsystem> BuildingSubsystem;
	FBuildProcessorRegistrationHandle DoorRegistration;
	FBuildDoorProcessor* DoorProcessor = nullptr;

	TSharedPtr<IBuildingPersistenceExtension> PersistenceExtension;

	UPROPERTY(Transient)
	TObjectPtr<UDoorBuildingDefinition> DoorDefinition = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UDoorBuildingDefinition> SettlementDoorDefinition = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UFirePileBuildingDefinition> FirePileDefinition = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UTorchFixtureBuildingDefinition> MountedTorchDefinition = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UWoodBuildingDefinition> WoodWallDefinition = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UWoodBuildingDefinition> WoodFloorDefinition = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UWoodBuildingDefinition> WoodPillarDefinition = nullptr;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UCityBuildingPieceDefinition>> CityBuildingPieceDefinitions;

	friend FBuildingCatalogPersistenceExtension;
};
