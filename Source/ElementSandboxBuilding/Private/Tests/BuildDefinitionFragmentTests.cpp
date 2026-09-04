#if WITH_DEV_AUTOMATION_TESTS

#include "Definition/BuildMeshPartDefinition.h"
#include "Definition/BuildCollisionPartDefinition.h"
#include "Engine/StaticMesh.h"
#include "Entity/BuildDefinitionFragment.h"
#include "Entity/BuildEntityRegistry.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BoxElem.h"
#include "Tests/BuildEntityTestTypes.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildDefinitionFragmentSharesMultiMeshDefinitionTest,
	"ElementSandbox.Building.Definition.SharesMultiMeshDefinition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildDefinitionFragmentSharesMultiMeshDefinitionTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	UBuildTestDefinition* Definition = NewObject<UBuildTestDefinition>();
	UStaticMesh* FrameMesh = NewObject<UStaticMesh>();
	UStaticMesh* LeafMesh = NewObject<UStaticMesh>();
	UStaticMesh* CollisionMesh = NewObject<UStaticMesh>();
	CollisionMesh->CreateBodySetup();
	CollisionMesh->GetBodySetup()->AggGeom.BoxElems.Add(FKBoxElem(100.0f));

	FBuildMeshPartDefinition FramePart;
	FramePart.Mesh = FrameMesh;
	FramePart.LocalTransform = FTransform(FVector(10.0, 0.0, 0.0));
	Definition->MeshParts.Add(FramePart);

	FBuildMeshPartDefinition LeafPart;
	LeafPart.Mesh = LeafMesh;
	LeafPart.LocalTransform = FTransform(FVector(-10.0, 0.0, 0.0));
	Definition->MeshParts.Add(LeafPart);

	FBuildCollisionPartDefinition CollisionPart;
	CollisionPart.CollisionMesh = CollisionMesh;
	Definition->CollisionParts.Add(CollisionPart);

	const FBuildEntityHandle FirstEntity = Definition->CreateEntity(
		Registry,
		FTransform::Identity);
	const FBuildEntityHandle SecondEntity = Definition->CreateEntity(
		Registry,
		FTransform(FVector(100.0, 0.0, 0.0)));
	TestTrue(TEXT("同一 Definition 可以创建两个 Entity"),
		Registry.IsAlive(FirstEntity) && Registry.IsAlive(SecondEntity));

	const FBuildDefinitionFragment* FirstDefinitionFragment =
		Registry.FindFragment<FBuildDefinitionFragment>(FirstEntity);
	const FBuildDefinitionFragment* SecondDefinitionFragment =
		Registry.FindFragment<FBuildDefinitionFragment>(SecondEntity);
	TestNotNull(TEXT("第一个 Entity 具有 Definition Fragment"), FirstDefinitionFragment);
	TestNotNull(TEXT("第二个 Entity 具有 Definition Fragment"), SecondDefinitionFragment);
	if (FirstDefinitionFragment && SecondDefinitionFragment)
	{
		TestTrue(TEXT("两个 Entity 直接持有同一个共享 Definition"),
			FirstDefinitionFragment->Definition.Get() == Definition
			&& SecondDefinitionFragment->Definition.Get() == Definition);
		TestEqual(TEXT("共享 Definition 保存两个 Mesh Part"),
			FirstDefinitionFragment->Definition->MeshParts.Num(), 2);
		TestTrue(TEXT("Part 0 保留 Frame Mesh"),
			FirstDefinitionFragment->Definition->MeshParts[0].Mesh == FrameMesh);
		TestTrue(TEXT("Part 1 保留 Leaf Mesh"),
			FirstDefinitionFragment->Definition->MeshParts[1].Mesh == LeafMesh);
	}

	const TWeakObjectPtr<UBuildTestDefinition> WeakDefinition = Definition;
	const TWeakObjectPtr<UStaticMesh> WeakFrameMesh = FrameMesh;
	const TWeakObjectPtr<UStaticMesh> WeakLeafMesh = LeafMesh;
	const TWeakObjectPtr<UStaticMesh> WeakCollisionMesh = CollisionMesh;
	Definition = nullptr;
	FrameMesh = nullptr;
	LeafMesh = nullptr;
	CollisionMesh = nullptr;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	TestTrue(TEXT("Definition Fragment 强引用使共享 Definition 在 GC 后存活"),
		WeakDefinition.IsValid());
	TestTrue(TEXT("Definition 的 UPROPERTY 链使全部 Mesh Part 在 GC 后存活"),
		WeakFrameMesh.IsValid() && WeakLeafMesh.IsValid());
	TestTrue(TEXT("Definition 的 UPROPERTY 链使 Collision Proxy 在 GC 后存活"),
		WeakCollisionMesh.IsValid());

	TestTrue(TEXT("销毁第一个 Entity"), Registry.DestroyEntity(FirstEntity));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestTrue(TEXT("仍有 Entity 引用时 Definition 保持存活"), WeakDefinition.IsValid());

	TestTrue(TEXT("销毁最后一个 Entity"), Registry.DestroyEntity(SecondEntity));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestFalse(TEXT("最后一个 Definition Fragment 注销后 Definition 可被回收"),
		WeakDefinition.IsValid());
	TestFalse(TEXT("Definition 回收后无其他引用的 Mesh Part 可被回收"),
		WeakFrameMesh.IsValid() || WeakLeafMesh.IsValid());
	TestFalse(TEXT("Definition 回收后 Collision Proxy 可被回收"),
		WeakCollisionMesh.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildDefinitionFragmentPoolLifetimeTest,
	"ElementSandbox.Building.Definition.StrongReferencePoolLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildDefinitionFragmentPoolLifetimeTest::RunTest(const FString& Parameters)
{
	FBuildEntityRegistry Registry;
	TArray<FBuildEntityHandle> Entities;
	TArray<TWeakObjectPtr<UBuildTestDefinition>> WeakDefinitions;
	Entities.Reserve(9);
	WeakDefinitions.Reserve(9);

	for (int32 Index = 0; Index < 9; ++Index)
	{
		UBuildTestDefinition* Definition = NewObject<UBuildTestDefinition>();
		WeakDefinitions.Add(Definition);

		const FBuildEntityHandle Entity = Registry.CreateEntity();
		Entities.Add(Entity);

		FBuildDefinitionFragment Fragment;
		Fragment.Definition.Reset(Definition);
		TestTrue(FString::Printf(TEXT("添加第 %d 个强引用 Fragment"), Index),
			Registry.AddFragment(Entity, Fragment));
		Fragment.Definition.Reset();
	}

	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	for (int32 Index = 0; Index < WeakDefinitions.Num(); ++Index)
	{
		TestTrue(FString::Printf(TEXT("Pool 扩容后 Definition %d 仍存活"), Index),
			WeakDefinitions[Index].IsValid());
	}

	TestTrue(TEXT("销毁 Pool 中间 Entity"), Registry.DestroyEntity(Entities[3]));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestFalse(TEXT("中间 Fragment 的独占 Definition 已释放"),
		WeakDefinitions[3].IsValid());

	const FBuildDefinitionFragment* MovedFragment =
		Registry.FindFragment<FBuildDefinitionFragment>(Entities[8]);
	TestNotNull(TEXT("Swap-Remove 后仍能找到被移动的最后一行"), MovedFragment);
	if (MovedFragment)
	{
		TestTrue(TEXT("Swap-Remove 保留正确的强引用"),
			MovedFragment->Definition.Get() == WeakDefinitions[8].Get());
	}

	TestTrue(TEXT("可单独移除 Definition Fragment"),
		Registry.RemoveFragment<FBuildDefinitionFragment>(Entities[5]));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestTrue(TEXT("移除 Fragment 不销毁 Entity"), Registry.IsAlive(Entities[5]));
	TestFalse(TEXT("单独移除 Fragment 会释放其独占 Definition"),
		WeakDefinitions[5].IsValid());

	Registry.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	for (int32 Index = 0; Index < WeakDefinitions.Num(); ++Index)
	{
		TestFalse(FString::Printf(TEXT("Reset 后 Definition %d 不再被 Pool 保活"), Index),
			WeakDefinitions[Index].IsValid());
	}
	return true;
}

#endif
