#if WITH_DEV_AUTOMATION_TESTS

#include "Entity/BuildEntityRegistry.h"
#include "Misc/AutomationTest.h"
#include "Storage/BuildEntitySparseCollections.h"
#include "Tests/BuildEntityTestTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildEntityHandleLifecycleTest,
	"ElementSandbox.Building.Entity.HandleLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildEntityHandleLifecycleTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	const FBuildEntityHandle Original = Registry.CreateEntity();
	TestTrue(TEXT("新 Handle 具有完整身份"), Original.IsSet());
	TestTrue(TEXT("创建后 Entity 存活"), Registry.IsAlive(Original));
	TestEqual(TEXT("Registry 记录一个 Entity"), Registry.GetEntityCount(), 1);

	TestTrue(TEXT("第一次销毁成功"), Registry.DestroyEntity(Original));
	TestFalse(TEXT("销毁后的 Handle 不再存活"), Registry.IsAlive(Original));
	TestFalse(TEXT("重复销毁被拒绝"), Registry.DestroyEntity(Original));
	TestEqual(TEXT("销毁后 Registry 为空"), Registry.GetEntityCount(), 0);

	const FBuildEntityHandle ReusedSlot = Registry.CreateEntity();
	TestEqual(TEXT("空闲 Slot 被复用"), ReusedSlot.GetIndex(), Original.GetIndex());
	TestTrue(TEXT("复用 Slot 后 Generation 推进"),
		ReusedSlot.GetGeneration() != Original.GetGeneration());
	TestFalse(TEXT("旧 Handle 不会命中新 Entity"), Registry.IsAlive(Original));
	TestTrue(TEXT("复用后新 Handle 存活"), Registry.IsAlive(ReusedSlot));

	FBuildEntityRegistry OtherRegistry;
	const FBuildEntityHandle OtherEntity = OtherRegistry.CreateEntity();
	TestTrue(TEXT("不同 Registry 身份不同"),
		OtherEntity.GetRegistryId() != ReusedSlot.GetRegistryId());
	TestFalse(TEXT("跨 Registry Handle 被拒绝"), Registry.IsAlive(OtherEntity));
	TestFalse(TEXT("跨 Registry 销毁被拒绝"), Registry.DestroyEntity(OtherEntity));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildFragmentLifecycleTest,
	"ElementSandbox.Building.Entity.FragmentLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildFragmentLifecycleTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	const FBuildEntityHandle Entity = Registry.CreateEntity();

	FBuildTestValueFragment ValueFragment;
	ValueFragment.Value = 7;
	ValueFragment.Label = TEXT("Initial");
	TestTrue(TEXT("添加值 Fragment"), Registry.AddFragment(Entity, ValueFragment));
	TestFalse(TEXT("同类型 Fragment 不能重复添加"), Registry.AddFragment(Entity, ValueFragment));
	TestEqual(TEXT("值 Pool 只有一行"), Registry.GetFragmentCount<FBuildTestValueFragment>(), 1);

	const FBuildTestValueFragment* StoredValue = Registry.FindFragment<FBuildTestValueFragment>(Entity);
	TestNotNull(TEXT("可通过 Entity 查询 Fragment"), StoredValue);
	if (StoredValue)
	{
		TestEqual(TEXT("复制数值字段"), StoredValue->Value, 7);
		TestEqual(TEXT("复制非平凡 FString 字段"), StoredValue->Label, FString(TEXT("Initial")));
	}

	FBuildTestValueFragment* MutableValue = Registry.FindMutableFragment<FBuildTestValueFragment>(Entity);
	TestNotNull(TEXT("可取得可写 Fragment"), MutableValue);
	if (MutableValue)
	{
		MutableValue->Value = 11;
		MutableValue->Label = TEXT("Changed");
	}

	StoredValue = Registry.FindFragment<FBuildTestValueFragment>(Entity);
	TestNotNull(TEXT("修改后 Fragment 仍存在"), StoredValue);
	if (StoredValue)
	{
		TestEqual(TEXT("可写修改已保存"), StoredValue->Value, 11);
		TestEqual(TEXT("非平凡字段修改已保存"), StoredValue->Label, FString(TEXT("Changed")));
	}

	FBuildTestTransformFragment TransformFragment;
	TransformFragment.Location = FVector(100.0, 200.0, 300.0);
	TestTrue(TEXT("同一 Entity 可添加第二种 Fragment"),
		Registry.AddFragment(Entity, TransformFragment));
	TestTrue(TEXT("可查询第二种 Fragment"), Registry.HasFragment<FBuildTestTransformFragment>(Entity));
	TestTrue(TEXT("移除指定 Fragment"), Registry.RemoveFragment<FBuildTestTransformFragment>(Entity));
	TestFalse(TEXT("重复移除被拒绝"), Registry.RemoveFragment<FBuildTestTransformFragment>(Entity));

	TestTrue(TEXT("销毁 Entity"), Registry.DestroyEntity(Entity));
	TestEqual(TEXT("销毁会级联移除值 Fragment"),
		Registry.GetFragmentCount<FBuildTestValueFragment>(), 0);
	TestNull(TEXT("旧 Handle 无法查询 Fragment"),
		Registry.FindFragment<FBuildTestValueFragment>(Entity));
	TestFalse(TEXT("旧 Handle 无法添加 Fragment"), Registry.AddFragment(Entity, ValueFragment));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildFragmentReserveKeepsAddressTest,
	"ElementSandbox.Building.Entity.FragmentReserveKeepsAddress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildFragmentReserveKeepsAddressTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	TestTrue(TEXT("预留初始 Fragment 容量"),
		Registry.ReserveFragmentCapacity<FBuildTestValueFragment>(4));
	const FBuildEntityHandle Entity = Registry.CreateEntity();
	FBuildTestValueFragment Fragment;
	Fragment.Value = 37;
	Fragment.Label = TEXT("StableAddress");
	TestTrue(TEXT("写入非平凡 Fragment"), Registry.AddFragment(Entity, Fragment));

	const TBuildFragmentPoolView<FBuildTestValueFragment> Before =
		Registry.GetFragmentPoolView<FBuildTestValueFragment>();
	const FBuildTestValueFragment* BeforeFragment = Before.Get(0);
	const FBuildEntityHandle* BeforeEntities = Before.Entities.GetData();
	TestTrue(TEXT("扩容前连续视图有效"),
		Before.IsValid() && BeforeFragment && BeforeEntities);

	TestTrue(TEXT("跨 65,536 行容量边界只提交新页"),
		Registry.ReserveFragmentCapacity<FBuildTestValueFragment>(65536));
	const TBuildFragmentPoolView<FBuildTestValueFragment> After =
		Registry.GetFragmentPoolView<FBuildTestValueFragment>();
	TestTrue(TEXT("Reserve 不搬迁 Fragment 数据首地址"),
		After.Get(0) == BeforeFragment);
	TestTrue(TEXT("Reserve 不搬迁 Dense Entity 行首地址"),
		After.Entities.GetData() == BeforeEntities);
	TestTrue(TEXT("Reserve 后非平凡字段和值保持不变"),
		After.Get(0)
		&& After.Get(0)->Value == 37
		&& After.Get(0)->Label == TEXT("StableAddress"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildFragmentRejectsObjectReferenceTest,
	"ElementSandbox.Building.Entity.RejectsObjectReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildFragmentRejectsObjectReferenceTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	const FBuildEntityHandle Entity = Registry.CreateEntity();
	FBuildTestObjectReferenceFragment Fragment;

	AddExpectedError(
		TEXT("contains a GC-tracked UObject reference"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	TestFalse(TEXT("Pool 拒绝需要反射 GC 扫描的 UObject 引用"),
		Registry.AddFragment(Entity, Fragment));
	TestEqual(TEXT("拒绝后没有创建 Fragment 行"),
		Registry.GetFragmentCount<FBuildTestObjectReferenceFragment>(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildFragmentPoolSwapRemovalTest,
	"ElementSandbox.Building.Entity.FragmentPoolSwapRemoval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildFragmentPoolSwapRemovalTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	TArray<FBuildEntityHandle> Entities;
	Entities.Reserve(9);

	for (int32 Index = 0; Index < 9; ++Index)
	{
		const FBuildEntityHandle Entity = Registry.CreateEntity();
		Entities.Add(Entity);

		FBuildTestValueFragment Fragment;
		Fragment.Value = Index;
		Fragment.Label = FString::Printf(TEXT("Entity-%d"), Index);
		TestTrue(FString::Printf(TEXT("添加第 %d 行"), Index), Registry.AddFragment(Entity, Fragment));
	}

	TestEqual(TEXT("扩容后保留九行"), Registry.GetFragmentCount<FBuildTestValueFragment>(), 9);
	TestTrue(TEXT("销毁 Pool 中间 Entity"), Registry.DestroyEntity(Entities[3]));
	TestEqual(TEXT("中间删除后只移除一行"), Registry.GetFragmentCount<FBuildTestValueFragment>(), 8);
	TestFalse(TEXT("被删除 Entity 不再存活"), Registry.IsAlive(Entities[3]));

	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		if (Index == 3)
		{
			continue;
		}

		const FBuildTestValueFragment* Fragment =
			Registry.FindFragment<FBuildTestValueFragment>(Entities[Index]);
		TestNotNull(FString::Printf(TEXT("Swap-Remove 后仍可找到 Entity %d"), Index), Fragment);
		if (Fragment)
		{
			TestEqual(FString::Printf(TEXT("Entity %d 数值未串行"), Index), Fragment->Value, Index);
			TestEqual(FString::Printf(TEXT("Entity %d 字符串未串行"), Index),
				Fragment->Label, FString::Printf(TEXT("Entity-%d"), Index));
		}
	}

	const FBuildEntityHandle RecycledEntity = Registry.CreateEntity();
	TestEqual(TEXT("销毁后复用相同 Entity Slot"),
		RecycledEntity.GetIndex(), Entities[3].GetIndex());
	TestTrue(TEXT("复用 Slot 时 Generation 前进"),
		RecycledEntity.GetGeneration() != Entities[3].GetGeneration());
	FBuildTestValueFragment RecycledFragment;
	RecycledFragment.Value = 99;
	TestTrue(TEXT("复用 Slot 可写入新 Fragment"),
		Registry.AddFragment(RecycledEntity, RecycledFragment));
	TestNull(TEXT("旧 Generation 不会误命中新 Dense Row"),
		Registry.FindFragment<FBuildTestValueFragment>(Entities[3]));
	const FBuildTestValueFragment* FoundRecycled =
		Registry.FindFragment<FBuildTestValueFragment>(RecycledEntity);
	TestTrue(TEXT("新 Generation 命中自己的 Dense Row"),
		FoundRecycled && FoundRecycled->Value == 99);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildEntityRegistryResetTest,
	"ElementSandbox.Building.Entity.RegistryReset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildEntityRegistryResetTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	TArray<FBuildEntityHandle> OldHandles;
	OldHandles.Reserve(64);

	for (int32 Index = 0; Index < 64; ++Index)
	{
		const FBuildEntityHandle Entity = Registry.CreateEntity();
		OldHandles.Add(Entity);

		FBuildTestValueFragment Fragment;
		Fragment.Value = Index;
		TestTrue(FString::Printf(TEXT("Reset 前添加第 %d 个 Fragment"), Index),
			Registry.AddFragment(Entity, Fragment));
	}

	Registry.Reset();
	TestEqual(TEXT("Reset 清空 Entity"), Registry.GetEntityCount(), 0);
	TestEqual(TEXT("Reset 清空 Fragment"), Registry.GetFragmentCount<FBuildTestValueFragment>(), 0);
	for (const FBuildEntityHandle Handle : OldHandles)
	{
		TestFalse(TEXT("Reset 使旧 Handle 失效"), Registry.IsAlive(Handle));
	}

	const FBuildEntityHandle NewEntity = Registry.CreateEntity();
	TestEqual(TEXT("Reset 后可复用第一个 Slot"), NewEntity.GetIndex(), 0);
	TestTrue(TEXT("Reset 后 Handle 使用新 Generation"),
		NewEntity.GetGeneration() != OldHandles[0].GetGeneration());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildFragmentEntitySnapshotTest,
	"ElementSandbox.Building.Entity.FragmentEntitySnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildFragmentEntitySnapshotTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	const FBuildEntityHandle First = Registry.CreateEntity();
	const FBuildEntityHandle Second = Registry.CreateEntity();
	const FBuildEntityHandle Third = Registry.CreateEntity();

	FBuildTestValueFragment ValueFragment;
	TestTrue(TEXT("第一个 Entity 加入值 Pool"), Registry.AddFragment(First, ValueFragment));
	TestTrue(TEXT("第三个 Entity 加入值 Pool"), Registry.AddFragment(Third, ValueFragment));

	TArray<FBuildEntityHandle> Entities;
	Registry.GetEntitiesWithFragment<FBuildTestValueFragment>(Entities);
	TestEqual(TEXT("快照只复制目标 Fragment Pool"), Entities.Num(), 2);
	TestTrue(TEXT("快照包含第一个 Entity"), Entities.Contains(First));
	TestTrue(TEXT("快照包含第三个 Entity"), Entities.Contains(Third));
	TestFalse(TEXT("快照不包含没有该 Fragment 的 Entity"), Entities.Contains(Second));

	TestTrue(TEXT("移除第一个 Entity 的值 Fragment"),
		Registry.RemoveFragment<FBuildTestValueFragment>(First));
	Registry.GetEntitiesWithFragment<FBuildTestValueFragment>(Entities);
	TestEqual(TEXT("再次查询会替换旧快照内容"), Entities.Num(), 1);
	TestTrue(TEXT("Swap-Remove 后快照保留第三个 Entity"), Entities[0] == Third);

	Registry.GetEntitiesWithFragment<FBuildTestTransformFragment>(Entities);
	TestTrue(TEXT("不存在的 Fragment Pool 返回空快照"), Entities.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildFragmentPagedSparseIndexTest,
	"ElementSandbox.Building.Entity.FragmentPagedSparseIndex",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildFragmentPagedSparseIndexTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	TArray<FBuildEntityHandle> Entities;
	Entities.Reserve(1025);
	for (int32 Index = 0; Index < 1025; ++Index)
	{
		Entities.Add(Registry.CreateEntity());
	}

	const FBuildEntityHandle HighSlotEntity = Entities.Last();
	FBuildTestValueFragment Fragment;
	Fragment.Value = 1024;
	TestTrue(TEXT("稀有 Fragment 可直接写入高编号 Slot"),
		Registry.AddFragment(HighSlotEntity, Fragment));
	const SIZE_T AllocatedWithSparsePage = Registry.GetEstimatedAllocatedSize();
	TestTrue(TEXT("高 Slot 分页索引可精确查询"),
		Registry.FindFragment<FBuildTestValueFragment>(HighSlotEntity)
			&& Registry.FindFragment<FBuildTestValueFragment>(HighSlotEntity)->Value == 1024);

	TestTrue(TEXT("移除高 Slot 的唯一 Fragment"),
		Registry.RemoveFragment<FBuildTestValueFragment>(HighSlotEntity));
	const SIZE_T AllocatedAfterPageRelease = Registry.GetEstimatedAllocatedSize();
	TestTrue(TEXT("页内清空后立即释放 256 Slot 索引页"),
		AllocatedAfterPageRelease < AllocatedWithSparsePage);

	const FBuildEntityHandle RecycledHighSlot = [&Registry, &Entities]()
	{
		const FBuildEntityHandle OldHandle = Entities.Last();
		Registry.DestroyEntity(OldHandle);
		return Registry.CreateEntity();
	}();
	TestEqual(TEXT("复用分页索引对应的高编号 Slot"),
		RecycledHighSlot.GetIndex(), HighSlotEntity.GetIndex());
	TestTrue(TEXT("复用 Slot 使用新 Generation"),
		RecycledHighSlot.GetGeneration() != HighSlotEntity.GetGeneration());
	TestTrue(TEXT("释放后重新按需创建索引页"),
		Registry.AddFragment(RecycledHighSlot, Fragment));
	TestNull(TEXT("旧 Generation 不会命中新页中的 Dense Row"),
		Registry.FindFragment<FBuildTestValueFragment>(HighSlotEntity));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildEntitySparseCollectionsTest,
	"ElementSandbox.Building.Entity.SparseCollectionsKeepAddress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildEntitySparseCollectionsTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	TBuildEntitySparseMap<int32> Values;
	FBuildEntitySparseSet Entities;
	TArray<FBuildEntityHandle> Handles;
	constexpr int32 EntityCount = 65537;
	Handles.Reserve(EntityCount);

	const FBuildEntityHandle First = Registry.CreateEntity();
	Handles.Add(First);
	int32* FirstValue = &Values.FindOrAdd(First);
	*FirstValue = 17;
	TestTrue(TEXT("首个 Entity 写入 Sparse Set"), Entities.Add(First));
	for (int32 Index = 1; Index < EntityCount; ++Index)
	{
		const FBuildEntityHandle Entity = Registry.CreateEntity();
		Handles.Add(Entity);
		Values.FindOrAdd(Entity) = Index;
		Entities.Add(Entity);
	}

	TestEqual(TEXT("跨 65,536 行后 Sparse Map 数量完整"), Values.Num(), EntityCount);
	TestEqual(TEXT("跨 65,536 行后 Sparse Set 数量完整"), Entities.Num(), EntityCount);
	TestTrue(TEXT("Dense TArray 增长不搬迁首个 Value"), Values.Find(First) == FirstValue);
	TestEqual(TEXT("首个 Value 内容保持"), *Values.Find(First), 17);
	TestEqual(TEXT("末行可按 Entity Slot 直接查询"), *Values.Find(Handles.Last()), EntityCount - 1);

	const FBuildEntityHandle Removed = Handles[EntityCount / 2];
	TestEqual(TEXT("Sparse Map Swap-Remove 一行"), Values.Remove(Removed), 1);
	TestEqual(TEXT("Sparse Set Swap-Remove 一行"), Entities.Remove(Removed), 1);
	TestNull(TEXT("被移除 Handle 不再命中 Map"), Values.Find(Removed));
	TestFalse(TEXT("被移除 Handle 不再命中 Set"), Entities.Contains(Removed));
	TestEqual(TEXT("Swap-Remove 后末行映射仍正确"), *Values.Find(Handles.Last()), EntityCount - 1);

	TestEqual(TEXT("移除首代 Handle"), Values.Remove(First), 1);
	TestEqual(TEXT("移除首代 Set Handle"), Entities.Remove(First), 1);
	TestTrue(TEXT("销毁首代 Registry Entity"), Registry.DestroyEntity(First));
	const FBuildEntityHandle Reused = Registry.CreateEntity();
	TestEqual(TEXT("Registry 复用同一 Slot"), Reused.GetIndex(), First.GetIndex());
	int32& ReusedValue = Values.FindOrAdd(Reused);
	TestEqual(TEXT("Swap-Remove 后复用的 Dense 行会值初始化"), ReusedValue, 0);
	ReusedValue = 99;
	Entities.Add(Reused);
	TestNull(TEXT("旧 Generation 不命中新 Sparse Map 行"), Values.Find(First));
	TestFalse(TEXT("旧 Generation 不命中新 Sparse Set 行"), Entities.Contains(First));
	TestEqual(TEXT("新 Generation 可命中 Map"), *Values.Find(Reused), 99);
	TestTrue(TEXT("新 Generation 可命中 Set"), Entities.Contains(Reused));
	return true;
}

#endif
