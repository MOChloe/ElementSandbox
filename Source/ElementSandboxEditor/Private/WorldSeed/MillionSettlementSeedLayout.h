#pragma once

#include "CoreMinimal.h"

#include "Entity/WorldEntityId.h"
#include "WorldSeed/MillionBuildingRecipe.h"

class UObject;

namespace UE::ElementSandbox::WorldSeed
{
	static constexpr int32 DefaultSeed = 0x13572468;
	static constexpr int32 LayoutRows = 1000;
	static constexpr int32 LayoutColumns = 1000;
	static constexpr int32 CompleteStructureCount = LayoutRows * LayoutColumns;
	static constexpr int64 ExpectedRecipeBuildingEntityCount = 29254200;
	static constexpr int32 MountedTorchFixturesPerStructure = 2;
	static constexpr int64 ExpectedMountedTorchBuildingEntityCount =
		static_cast<int64>(CompleteStructureCount) * MountedTorchFixturesPerStructure;
	static constexpr int64 ExpectedBuildingEntityCount =
		ExpectedRecipeBuildingEntityCount + ExpectedMountedTorchBuildingEntityCount;
	static constexpr int64 ExpectedTreeWorldObjectCount = CompleteStructureCount;
	static constexpr int64 ExpectedTotalWorldEntityCount =
		ExpectedBuildingEntityCount + ExpectedTreeWorldObjectCount;
	/** 6.2m 基础网格在正式世界中放大三倍，最终高度约 18.6m。 */
	static constexpr double SettlementTreeScaleMultiplier = 3.0;
	static constexpr double SettlementTreeMinimumUniformScale = 0.85 * SettlementTreeScaleMultiplier;
	static constexpr double SettlementTreeMaximumUniformScale = 1.15 * SettlementTreeScaleMultiplier;
	/** 树布局事实变化时推进，确保客户端 Chunk Cache 不复用旧 Transform。 */
	static constexpr uint32 SettlementTreeLayoutRevision = 2;
	/** Building 配方伴生实体变化时推进，确保客户端 Chunk Cache 不复用旧布局。 */
	static constexpr uint32 SettlementBuildingLayoutRevision = 2;

	enum class ESettlementTreeMode : uint8
	{
		None,
		OnePerStructure
	};

	bool BuildRecipeCatalog(UObject* Outer, TArray<UCityBuildingRecipe*>& OutRecipes, FString& OutError);
	uint8 ResolveArchetypeIndex(uint32 LayoutIndex, int32 Seed = DefaultSeed);
	bool BuildArchetypeAssignments(
		int32 StructureCount,
		const TArray<UCityBuildingRecipe*>& Recipes,
		int32 Seed,
		TArray<uint8>& OutAssignments,
		int64& OutEntityCount,
		FString& OutError);
	FTransform ResolveStructureTransform(uint32 LayoutIndex, int32 Seed = DefaultSeed);
	/** 返回道路侧的确定性树 Transform；树的局部 -X 朝向道路。 */
	FTransform ResolveSettlementTreeTransform(
		uint32 LayoutIndex,
		const FVector2D& FootprintCentimeters,
		int32 Seed = DefaultSeed);
	FWorldEntityId ResolveSettlementTreeEntityId(
		uint32 StructureIndex,
		int64 BuildingEntityCount = ExpectedBuildingEntityCount);
	FWorldEntityId ResolveSettlementMountedTorchEntityId(
		uint32 StructureIndex,
		uint8 FixtureIndex,
		int64 RecipeBuildingEntityCount = ExpectedRecipeBuildingEntityCount);
	bool TryParseTreeMode(const FString& Value, ESettlementTreeMode& OutMode);
	const TCHAR* LexToString(ESettlementTreeMode Mode);
	int64 CountRecipeBuildingEntities(
		int32 StructureCount,
		const TArray<UCityBuildingRecipe*>& Recipes,
		TArray<int64>* OutStructureCountByArchetype = nullptr,
		int32 Seed = DefaultSeed);
		/** 不同布局规模、TreeMode 或树布局 Revision 是不同世界，避免客户端 Chunk Cache 串用。 */
		FGuid MakeWorldId(int32 Seed, int32 StructureCount, ESettlementTreeMode TreeMode);
	}
