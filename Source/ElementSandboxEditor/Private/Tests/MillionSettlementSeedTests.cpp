#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "WorldSeed/MillionSettlementSeedLayout.h"

#include "Algo/Accumulate.h"
#include "WorldSeed/MillionBuildingRecipe.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMillionSettlementSeedDeterminismTest,
	"ElementSandbox.WorldStorage.Seed.MillionSettlementDeterminism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMillionSettlementSeedDeterminismTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::ElementSandbox::WorldSeed;

	TArray<UCityBuildingRecipe*> Recipes;
	FString Error;
	TestTrue(TEXT("可建立恰好 12 套离线配方"), BuildRecipeCatalog(GetTransientPackage(), Recipes, Error));
	if (Recipes.Num() != 12)
	{
		AddError(Error);
		return false;
	}
	TArray<int64> FirstHistogram;
	TArray<int64> SecondHistogram;
	const int64 FirstCount = CountRecipeBuildingEntities(CompleteStructureCount, Recipes, &FirstHistogram);
	const int64 SecondCount = CountRecipeBuildingEntities(CompleteStructureCount, Recipes, &SecondHistogram);
	for (int32 Index = 0; Index < Recipes.Num(); ++Index)
	{
		AddInfo(FString::Printf(TEXT("Archetype[%d]: Structures=%lld Pieces=%d"),
			Index, FirstHistogram[Index], Recipes[Index]->GetPieceEntityCount()));
	}
	TestEqual(TEXT("100 万完整结构展开后的普通 Building Entity 数"),
		FirstCount, ExpectedRecipeBuildingEntityCount);
	TestEqual(TEXT("每栋两个挂墙火把精确追加 200 万 Building Entity"),
		ExpectedMountedTorchBuildingEntityCount, 2000000ll);
	TestEqual(TEXT("普通结构部件与挂墙火把合计 Building Entity 数"),
		ExpectedBuildingEntityCount, 31254200ll);
	TestEqual(TEXT("重复计算保持完全一致"), SecondCount, FirstCount);
	TestTrue(TEXT("配方分布直方图确定"), FirstHistogram == SecondHistogram);
	TestEqual(TEXT("直方图覆盖全部完整结构"),
		Algo::Accumulate(FirstHistogram, int64{0}), static_cast<int64>(CompleteStructureCount));
		const FGuid TreeWorldId = MakeWorldId(
			DefaultSeed, CompleteStructureCount, ESettlementTreeMode::OnePerStructure);
		TestEqual(TEXT("同一 Settlement 配置的 WorldId 稳定"), TreeWorldId,
			MakeWorldId(DefaultSeed, CompleteStructureCount, ESettlementTreeMode::OnePerStructure));
		TestNotEqual(TEXT("不同 Seed 的 WorldId 不同"), TreeWorldId,
			MakeWorldId(DefaultSeed + 1, CompleteStructureCount, ESettlementTreeMode::OnePerStructure));
		TestNotEqual(TEXT("TreeMode=None 基线不与正式树世界共用 Chunk Cache"), TreeWorldId,
			MakeWorldId(DefaultSeed, CompleteStructureCount, ESettlementTreeMode::None));
		TestNotEqual(TEXT("不同结构数量不共用 Chunk Cache"), TreeWorldId,
			MakeWorldId(DefaultSeed, CompleteStructureCount - 1, ESettlementTreeMode::OnePerStructure));
	const FVector2D Footprint(2400.0, 3600.0);
	const FTransform FirstTree = ResolveSettlementTreeTransform(0, Footprint, DefaultSeed);
	const FTransform SameTree = ResolveSettlementTreeTransform(0, Footprint, DefaultSeed);
	const FTransform OtherTree = ResolveSettlementTreeTransform(1, Footprint, DefaultSeed);
	TestTrue(TEXT("树位置、Yaw 与 Scale 对同一 Seed 完全确定"), FirstTree.Equals(SameTree));
	TestFalse(TEXT("相邻结构的树散列结果不同"), FirstTree.Equals(OtherTree));
	TestTrue(TEXT("树 Uniform Scale 为原规格三倍且位于批准区间"),
		FirstTree.GetScale3D().GetMin()
			>= SettlementTreeMinimumUniformScale - UE_KINDA_SMALL_NUMBER
		&& FirstTree.GetScale3D().GetMax()
			<= SettlementTreeMaximumUniformScale + UE_KINDA_SMALL_NUMBER
		&& FMath::IsNearlyEqual(FirstTree.GetScale3D().X, FirstTree.GetScale3D().Y)
		&& FMath::IsNearlyEqual(FirstTree.GetScale3D().Y, FirstTree.GetScale3D().Z));
	TestTrue(TEXT("批准区间中心对应三倍基础尺寸"),
		FMath::IsNearlyEqual(
			(SettlementTreeMinimumUniformScale + SettlementTreeMaximumUniformScale) * 0.5,
			SettlementTreeScaleMultiplier));
	for (const UCityBuildingRecipe* Recipe : Recipes)
	{
		TestEqual(TEXT("每种完整结构明确提供两个挂墙火把插槽"),
			Recipe ? Recipe->GetMountedTorchEntityCount() : 0,
			MountedTorchFixturesPerStructure);
	}
	TestEqual(TEXT("正式种子第一盏挂墙火把紧接普通结构部件 ID"),
		ResolveSettlementMountedTorchEntityId(0, 0).GetValue(), 29254201ull);
	TestEqual(TEXT("正式种子最后一盏挂墙火把 ID"),
		ResolveSettlementMountedTorchEntityId(
			CompleteStructureCount - 1,
			MountedTorchFixturesPerStructure - 1).GetValue(),
		31254200ull);
	TestEqual(TEXT("正式种子第一棵树紧接全部 Building ID"),
		ResolveSettlementTreeEntityId(0).GetValue(), 31254201ull);
	TestEqual(TEXT("正式种子最后一棵树 ID"),
		ResolveSettlementTreeEntityId(CompleteStructureCount - 1).GetValue(), 32254200ull);
	TestEqual(TEXT("正式种子 NextEntityId"), ExpectedTotalWorldEntityCount + 1, 32254201ll);
	return true;
}

#endif
