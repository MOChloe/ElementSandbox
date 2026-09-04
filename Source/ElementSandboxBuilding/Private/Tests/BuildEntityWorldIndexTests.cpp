#if WITH_DEV_AUTOMATION_TESTS

#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildEntityWorldIndex.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDevice.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildEntityWorldIndexHashDistributionTest,
	"ElementSandbox.Building.Storage.IdentityIndexBucketDistribution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildEntityWorldIndexHashDistributionTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	FBuildEntityWorldIndex Index;
	constexpr uint64 EntityCount = 65536;
	TArray<FBuildEntityHandle> Handles;
	Handles.Reserve(EntityCount);
	for (uint64 Value = 1; Value <= EntityCount; ++Value)
	{
		const FBuildEntityHandle Entity = Registry.CreateEntity();
		Handles.Add(Entity);
		Index.Add(FWorldEntityId(Value), Entity);
	}
	TestEqual(TEXT("身份索引记录全部实体"), Index.Num(), int32(EntityCount));
	bool bAllResolved = true;
	for (uint64 Value = 1; Value <= EntityCount; ++Value)
	{
		const FBuildEntityHandle* Entity = Index.Find(FWorldEntityId(Value));
		bAllResolved &= Entity && *Entity == Handles[int32(Value - 1)];
	}
	TestTrue(TEXT("分片查找保持完整 WorldEntityId 到 Handle 的映射"), bAllResolved);
	TestNull(TEXT("不存在的身份不会误命中"), Index.Find(FWorldEntityId(EntityCount + 1)));

	// 读取 UE 容器实际桶链，不用时间阈值，也不复制待测哈希算法。
	int32 MaximumChain = 0;
	int32 OccupiedBuckets = 0;
	for (auto& Shard : Index.Shards)
	{
		FStringOutputDevice Dump;
		Dump.SetAutoEmitLineTerminator(true);
		Shard.Dump(Dump);
		TArray<FString> Lines;
		Dump.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			FString Count;
			if (Line.Contains(TEXT("Hash[")) && Line.Split(TEXT(" = "), nullptr, &Count))
			{
				const int32 BucketSize = FCString::Atoi(*Count);
				MaximumChain = FMath::Max(MaximumChain, BucketSize);
				OccupiedBuckets += BucketSize > 0 ? 1 : 0;
			}
		}
	}
	AddInfo(FString::Printf(TEXT("Identity index: %llu keys, %d occupied buckets, maximum chain %d."),
		EntityCount, OccupiedBuckets, MaximumChain));
	TestTrue(TEXT("连续身份不能因分片和桶复用低位而集中成长链"), MaximumChain <= 4);
	TestTrue(TEXT("实际桶分布覆盖至少四分之一的实体数量"), OccupiedBuckets >= int32(EntityCount / 4));

	TestEqual(TEXT("删除精确身份"), Index.Remove(FWorldEntityId(256)), 1);
	TestNull(TEXT("已删除身份不再可查"), Index.Find(FWorldEntityId(256)));
	const FWorldEntityId HighId((uint64(1) << 48) + 256);
	Index.Add(HighId, Handles[255]);
	TestTrue(TEXT("完整 64 位身份可写入和查找"), Index.Contains(HighId));
	TestNull(TEXT("高位不同的身份不与旧身份混淆"), Index.Find(FWorldEntityId(256)));
	TestEqual(TEXT("删除后补入保持正确计数"), Index.Num(), int32(EntityCount));
	return true;
}

#endif
