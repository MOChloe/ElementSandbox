#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Runtime/ElementFireShardedMap.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FElementFireShardedMapScaleTest,
	"ElementSandbox.Element.Fire.Storage.ShardedMapPreservesLargeHostDirectory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElementFireShardedMapScaleTest::RunTest(const FString& Parameters)
{
	TElementFireShardedMap<uint32, FString> Directory;
	constexpr uint32 EntryCount = 200000;
	for (uint32 Index = 0; Index < EntryCount; ++Index)
	{
		Directory.Add(Index, FString::Printf(TEXT("Host-%u"), Index));
	}
	TestEqual(TEXT("所有分片累计条目数正确"), Directory.Num(), static_cast<int32>(EntryCount));

	for (uint32 Index = 0; Index < EntryCount; Index += 997)
	{
		const FString* Value = Directory.Find(Index);
		TestTrue(TEXT("跨分片查找保持值稳定"), Value && *Value == FString::Printf(TEXT("Host-%u"), Index));
	}

	bool bAllRemovalsHit = true;
	for (uint32 Index = 0; Index < EntryCount; Index += 2)
	{
		bAllRemovalsHit &= Directory.Remove(Index) == 1;
	}
	TestTrue(TEXT("所有删除都精确命中一个分片条目"), bAllRemovalsHit);
	TestEqual(TEXT("删除后累计条目数正确"), Directory.Num(), static_cast<int32>(EntryCount / 2));
	TestTrue(TEXT("保留的奇数条目仍可查找"), Directory.Contains(EntryCount - 1));
	return true;
}

#endif
