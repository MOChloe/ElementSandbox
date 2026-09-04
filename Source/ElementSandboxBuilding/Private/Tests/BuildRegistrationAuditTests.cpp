#if WITH_DEV_AUTOMATION_TESTS

#include "Audit/BuildRegistrationAudit.h"
#include "BuildingWorldSubsystem.h"
#include "Collision/BuildCollisionHost.h"
#include "Definition/BuildCollisionPartDefinition.h"
#include "Definition/BuildMeshPartDefinition.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildTransformFragment.h"
#include "Misc/AutomationTest.h"
#include "Tests/BuildEntityTestTypes.h"

namespace ElementSandbox::Building::Audit::Tests
{
	struct FAuditTestWorld final
	{
		explicit FAuditTestWorld(const FName Name)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, Name, nullptr, true);
			check(World);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			Subsystem = World->GetSubsystem<UBuildingWorldSubsystem>();
		}

		~FAuditTestWorld()
		{
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}

		UWorld* World = nullptr;
		UBuildingWorldSubsystem* Subsystem = nullptr;
	};

	UStaticMesh* LoadAuditCube()
	{
		return LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	}

	UBuildTestDefinition* MakeAuditDefinition(
		UObject* Outer,
		UStaticMesh* Cube,
		const bool bWithCollision,
		const int32 PartCount = 1)
	{
		UBuildTestDefinition* Definition = NewObject<UBuildTestDefinition>(Outer);
		Definition->DefinitionId = bWithCollision
			? TEXT("Test.Audit.Solid")
			: TEXT("Test.Audit.Decorative");
		for (int32 PartId = 0; PartId < PartCount; ++PartId)
		{
			FBuildMeshPartDefinition& MeshPart = Definition->MeshParts.AddDefaulted_GetRef();
			MeshPart.Mesh = Cube;
			MeshPart.SurfaceProfileId = TEXT("Surface.Test.Neutral");
			MeshPart.LocalTransform = FTransform(FVector(PartId * 10.0, 0.0, 0.0));
			if (bWithCollision)
			{
				FBuildCollisionPartDefinition& CollisionPart =
					Definition->CollisionParts.AddDefaulted_GetRef();
				CollisionPart.CollisionMesh = Cube;
				CollisionPart.DrivenMeshPartId = PartId;
			}
		}
		return Definition;
	}

	FBuildCollisionSource MakeAuditSource(
		const FVector& Location,
		const uint64 Revision,
		const bool bUrgent)
	{
		FBuildCollisionSource Source;
		Source.SubjectLocation = Location;
		Source.ImmediateBounds = bUrgent
			? FBox::BuildAABB(Location, FVector(500.0))
			: FBox::BuildAABB(Location + FVector(650.0, 0.0, 0.0), FVector(1.0));
		Source.CameraBounds = Source.ImmediateBounds;
		Source.PrefetchBounds = FBox::BuildAABB(Location, FVector(500.0));
		Source.RetentionBounds = FBox::BuildAABB(Location, FVector(700.0));
		Source.Revision = Revision;
		return Source;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildRenderPartRegistrationAuditTest,
	"ElementSandbox.Building.Audit.RenderPartRegistration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildRenderPartRegistrationAuditTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Audit::Tests;
	FAuditTestWorld Harness(TEXT("BuildRenderPartRegistrationAudit"));
	UStaticMesh* Cube = LoadAuditCube();
	if (!TestTrue(TEXT("审计 World 与 Cube 有效"), Harness.Subsystem && Cube))
	{
		return false;
	}

	UBuildTestDefinition* Solid = MakeAuditDefinition(Harness.Subsystem, Cube, true);
	const FBuildEntityHandle SolidEntity = Harness.Subsystem->CreateEntity(
		*Solid,
		FTransform(FVector(1000.0, 1000.0, 1000.0)));
	if (!TestTrue(TEXT("创建可审计 Solid Entity"), SolidEntity.IsSet()))
	{
		return false;
	}

	const FBuildRenderPartShapeAudit Shape =
		Harness.Subsystem->AuditRenderPartShape(SolidEntity, 0);
	TestTrue(TEXT("渲染 Part 有独立稳定 Shape"),
		Shape.Status == EBuildRenderPartShapeAuditStatus::Registered);
	TestTrue(TEXT("Shape 审计保留完整身份"),
		Shape.Domain == TEXT("Building")
			&& Shape.WorldEntityId.IsSet()
			&& Shape.Entity == SolidEntity
			&& Shape.DefinitionId == Solid->DefinitionId
			&& Shape.MeshPartId == 0
			&& Shape.ShapeRef.IsSet()
			&& Shape.SurfaceProfileId == TEXT("Surface.Test.Neutral"));
	TestTrue(TEXT("成功 Shape 审计没有失败原因"), Shape.FailureReason.IsEmpty());

	const FBuildRenderPartCollisionAudit NoSource =
		Harness.Subsystem->AuditRenderPartCollision(SolidEntity, 0);
	TestEqual(TEXT("Solid Render Part 映射一个 Collision Part"),
		NoSource.CollisionParts.Num(), 1);
	if (NoSource.CollisionParts.Num() == 1)
	{
		TestTrue(TEXT("无 Source 时明确报告 NotRequired"),
			NoSource.CollisionParts[0].Status
				== EBuildCollisionPartAuditStatus::NotRequired);
	}

	const FBuildCollisionSourceHandle Source = Harness.Subsystem->RegisterCollisionSource(
		MakeAuditSource(FVector(1000.0), 1, true));
	TestTrue(TEXT("注册 Immediate Collision Source"), Source.IsSet());
	TestTrue(TEXT("Required Collision 投影成功"), Harness.Subsystem->FlushCollisionChanges());
	const FBuildRenderPartCollisionAudit Active =
		Harness.Subsystem->AuditRenderPartCollision(SolidEntity, 0);
		if (Active.CollisionParts.Num() == 1)
		{
			const FBuildCollisionPartAudit& Part = Active.CollisionParts[0];
			TestTrue(TEXT("Required Part 报告有效 Body"),
				Part.Status == EBuildCollisionPartAuditStatus::ActiveBody
					&& Part.bRequired
					&& Part.Instance.IsSet()
					&& Part.ClusterKey.IsSet());
			const FString Report = FormatBuildRenderPartRegistrationAudit(Shape, Active);
			TestTrue(TEXT("运行时报告包含稳定身份"),
				Report.Contains(TEXT("WorldEntityId="))
					&& Report.Contains(TEXT("DefinitionId=Test.Audit.Solid"))
					&& Report.Contains(TEXT("MeshPartId=0")));
			TestTrue(TEXT("运行时报告包含 Collision/Cluster/Instance 链路"),
				Report.Contains(TEXT("CollisionPartId=0"))
					&& Report.Contains(TEXT("Required=true"))
					&& Report.Contains(TEXT("Cluster="))
					&& Report.Contains(TEXT("Instance="))
					&& Report.Contains(TEXT("Status=ActiveBody")));
		}

	ABuildCollisionHost* Host = Harness.Subsystem->GetCollisionHost();
	TestNotNull(TEXT("审计可解析 Collision Host"), Host);
	if (Host)
	{
		Host->ClearInstances();
		const FBuildRenderPartCollisionAudit InvalidBody =
			Harness.Subsystem->AuditRenderPartCollision(SolidEntity, 0);
		if (InvalidBody.CollisionParts.Num() == 1)
		{
			TestTrue(TEXT("Host 中丢失的 Stable Instance 被明确报告"),
				InvalidBody.CollisionParts[0].Status
					== EBuildCollisionPartAuditStatus::InvalidInstance);
		}

		FBuildTransformFragment* Transform =
			Harness.Subsystem->GetRegistry().FindMutableFragment<FBuildTransformFragment>(
				SolidEntity);
		if (Transform)
		{
			Transform->WorldTransform.AddToTranslation(FVector(10.0, 0.0, 0.0));
		}
		TestTrue(TEXT("提交 Body 已失效后的宿主 Transform 变化"),
			Transform && Harness.Subsystem->CommitEntityTransformChange(SolidEntity));
		TestFalse(TEXT("Host Remove 失败触发完整重投影"),
			Harness.Subsystem->FlushCollisionChanges());
		const FBuildRenderPartCollisionAudit HostFailure =
			Harness.Subsystem->AuditRenderPartCollision(SolidEntity, 0);
		if (HostFailure.CollisionParts.Num() == 1)
		{
			TestTrue(TEXT("Host Apply 失败不会退化成笼统的 Body 缺失"),
				HostFailure.CollisionParts[0].Status
					== EBuildCollisionPartAuditStatus::HostApplyFailure);
		}
		TestTrue(TEXT("下一轮重投影可恢复"), Harness.Subsystem->FlushCollisionChanges());
	}

	UBuildTestDefinition* Decorative = MakeAuditDefinition(Harness.Subsystem, Cube, false);
	const FBuildEntityHandle DecorativeEntity = Harness.Subsystem->CreateEntity(
		*Decorative,
		FTransform(FVector(3000.0, 1000.0, 1000.0)));
	const FBuildRenderPartCollisionAudit DecorativeAudit =
		Harness.Subsystem->AuditRenderPartCollision(DecorativeEntity, 0);
	TestEqual(TEXT("Decorative 零碰撞仍返回一条明确审计结论"),
		DecorativeAudit.CollisionParts.Num(), 1);
	if (DecorativeAudit.CollisionParts.Num() == 1)
	{
		TestTrue(TEXT("Decorative 不会被擅自解释为 Solid"),
			DecorativeAudit.CollisionParts[0].Status
				== EBuildCollisionPartAuditStatus::NoCollisionDefinition);
	}

	const FBuildRenderPartShapeAudit MissingPart =
		Harness.Subsystem->AuditRenderPartShape(SolidEntity, 99);
	TestTrue(TEXT("非法 Render Part 有具体失败原因"),
		MissingPart.Status == EBuildRenderPartShapeAuditStatus::MissingRenderPart
			&& !MissingPart.FailureReason.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCollisionAuditPendingBudgetTest,
	"ElementSandbox.Building.Audit.PendingCollisionBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildCollisionAuditPendingBudgetTest::RunTest(const FString& Parameters)
{
	using namespace ElementSandbox::Building::Audit::Tests;
	FAuditTestWorld Harness(TEXT("BuildCollisionAuditPendingBudget"));
	UStaticMesh* Cube = LoadAuditCube();
	if (!Harness.Subsystem || !Cube)
	{
		return false;
	}

	constexpr int32 PartCount = 17;
	UBuildTestDefinition* Definition = MakeAuditDefinition(
		Harness.Subsystem,
		Cube,
		true,
		PartCount);
	const FBuildEntityHandle Entity = Harness.Subsystem->CreateEntity(
		*Definition,
		FTransform::Identity);
	const FBuildCollisionSourceHandle Source = Harness.Subsystem->RegisterCollisionSource(
		MakeAuditSource(FVector::ZeroVector, 1, false));
	if (!TestTrue(TEXT("创建 17 Part 预算审计场景"), Entity.IsSet() && Source.IsSet()))
	{
		return false;
	}
	TestTrue(TEXT("首轮只消费一个 Prefetch 预算"),
		Harness.Subsystem->FlushCollisionChanges());
	TestEqual(TEXT("首轮只投影默认 16 个 Part"),
		Harness.Subsystem->GetActiveCollisionBodyCount(), 16);

	int32 PendingCount = 0;
	for (int32 PartId = 0; PartId < PartCount; ++PartId)
	{
		const FBuildRenderPartCollisionAudit Audit =
			Harness.Subsystem->AuditRenderPartCollision(Entity, PartId);
		if (Audit.CollisionParts.Num() == 1
			&& Audit.CollisionParts[0].Status
				== EBuildCollisionPartAuditStatus::PendingBudget)
		{
			++PendingCount;
		}
	}
	TestEqual(TEXT("剩余一个 Required Part 明确处于预算队列"), PendingCount, 1);
	return true;
}

#endif
