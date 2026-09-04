#if WITH_DEV_AUTOMATION_TESTS

#include "Entity/BuildEntityRegistry.h"
#include "Misc/AutomationTest.h"
#include "Rendering/BuildRenderDirtySet.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildRenderDirtySetCoalescesEntityChangesTest,
	"ElementSandbox.Building.Rendering.DirtySetCoalescesEntityChanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildRenderDirtySetCoalescesEntityChangesTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	const FBuildEntityHandle First = Registry.CreateEntity();
	const FBuildEntityHandle Second = Registry.CreateEntity();
	const FBuildEntityHandle Third = Registry.CreateEntity();
	FBuildRenderDirtySet DirtySet;
	const int32 FirstParts[] = {4, 5, 4};
	const int32 ThirdParts[] = {1};

	TestFalse(TEXT("无效 Handle 不会进入表现脏集合"),
		DirtySet.MarkPartsDirty({}, FirstParts));
	TestTrue(TEXT("首次 PartSet 标记成功"),
		DirtySet.MarkPartsDirty(First, FirstParts));
	TestTrue(TEXT("另一个 Entity 的 Rebuild 标记成功"),
		DirtySet.MarkRebuild(Second));
	TestTrue(TEXT("重复 PartId 被幂等去重"),
		DirtySet.MarkPartsDirty(First, FirstParts));
	TestTrue(TEXT("AllParts 可以覆盖既有 PartSet"),
		DirtySet.MarkAllPartsDirty(First));
	TestTrue(TEXT("Rebuild 可以覆盖既有 AllParts"),
		DirtySet.MarkRebuild(First));
	TestTrue(TEXT("Rebuild 后的 PartSet 不会降低工作类别"),
		DirtySet.MarkPartsDirty(First, FirstParts));
	TestTrue(TEXT("第三个 Entity 可以继续加入"),
		DirtySet.MarkPartsDirty(Third, ThirdParts));

	const TConstArrayView<FBuildRenderDirtyEntry> Entries = DirtySet.GetEntries();
	TestEqual(TEXT("重复标记按 Entity 合并"), Entries.Num(), 3);
	if (Entries.Num() == 3)
	{
		TestTrue(TEXT("合并后保持 First 首次出现顺序"), Entries[0].Entity == First);
		TestTrue(TEXT("First 最终只需要 Rebuild"),
			Entries[0].Mode == EBuildRenderDirtyMode::Rebuild);
		TestTrue(TEXT("Second 保持第二个出现"), Entries[1].Entity == Second);
		TestTrue(TEXT("Second 保持 Rebuild"),
			Entries[1].Mode == EBuildRenderDirtyMode::Rebuild);
		TestTrue(TEXT("Third 保持第三个出现"), Entries[2].Entity == Third);
		TestTrue(TEXT("Third 只需要 PartSet"),
			Entries[2].Mode == EBuildRenderDirtyMode::PartSet);
		TestEqual(TEXT("PartSet 保存唯一 PartId"), Entries[2].PartIds.Num(), 1);
		TestEqual(TEXT("PartSet 保存正确 PartId"), Entries[2].PartIds[0], 1);
	}

	TestTrue(TEXT("Entity 可以在表现消费前先被销毁"), Registry.DestroyEntity(First));
	TestTrue(TEXT("已销毁但身份完整的 Handle 仍可标记删除所需 Rebuild"),
		DirtySet.MarkRebuild(First));
	TestEqual(TEXT("删除标记继续合并到原 Entity"), DirtySet.Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildRenderDirtySetClearAllBoundaryTest,
	"ElementSandbox.Building.Rendering.DirtySetClearAllBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildRenderDirtySetClearAllBoundaryTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	const FBuildEntityHandle BeforeReset = Registry.CreateEntity();
	FBuildRenderDirtySet DirtySet;
	const int32 PartIds[] = {0};

	TestTrue(TEXT("Clear-All 前可记录 Entity"),
		DirtySet.MarkPartsDirty(BeforeReset, PartIds));
	DirtySet.RequestClearAll();
	TestTrue(TEXT("记录全量表现清理请求"), DirtySet.IsClearAllRequested());
	TestEqual(TEXT("Clear-All 使此前逐 Entity 标记作废"), DirtySet.Num(), 0);
	TestFalse(TEXT("只有 Clear-All 请求时集合仍有待处理工作"), DirtySet.IsEmpty());

	Registry.Reset();
	const FBuildEntityHandle AfterReset = Registry.CreateEntity();
	TestTrue(TEXT("Clear-All 后的新 Entity 可以加入同一批次"),
		DirtySet.MarkRebuild(AfterReset));

	const TConstArrayView<FBuildRenderDirtyEntry> Entries = DirtySet.GetEntries();
	TestEqual(TEXT("只保留 Reset 后的新 Entity"), Entries.Num(), 1);
	if (Entries.Num() == 1)
	{
		TestTrue(TEXT("Reset 后记录使用新 Handle"), Entries[0].Entity == AfterReset);
		TestTrue(TEXT("Reset 后新 Entity 需要 Rebuild"),
			Entries[0].Mode == EBuildRenderDirtyMode::Rebuild);
	}

	DirtySet.Clear();
	TestFalse(TEXT("消费清理后不再请求 Clear-All"), DirtySet.IsClearAllRequested());
	TestEqual(TEXT("消费清理后没有逐 Entity 标记"), DirtySet.Num(), 0);
	TestTrue(TEXT("消费清理后集合为空"), DirtySet.IsEmpty());
	return true;
}

#endif
