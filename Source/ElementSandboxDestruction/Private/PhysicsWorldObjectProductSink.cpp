#include "PhysicsWorldObjectProductSink.h"

#include "Definition/WorldDestructionDefinition.h"
#include "Definition/WorldObjectDefinition.h"
#include "WorldObjectWorldSubsystem.h"

namespace UE::ElementSandbox::Destruction
{
namespace
{
	int32 MakeProductSeed(const FWorldDestructionProductBatch& Batch)
	{
		const uint64 SourceValue = Batch.SourceId.GetValue();
		const uint32 FoldedSource = static_cast<uint32>(SourceValue)
			^ static_cast<uint32>(SourceValue >> 32);
		return static_cast<int32>(HashCombineFast(FoldedSource, Batch.DestructionRevision));
	}

	FVector RandomSignedVector(FRandomStream& Random, const FVector2D& AbsoluteRange)
	{
		auto MakeAxis = [&Random, &AbsoluteRange]()
		{
			const float Magnitude = Random.FRandRange(AbsoluteRange.X, AbsoluteRange.Y);
			return Random.RandRange(0, 1) == 0 ? -Magnitude : Magnitude;
		};
		return FVector(MakeAxis(), MakeAxis(), MakeAxis());
	}
}

bool FPhysicsWorldObjectProductSink::Prepare(const FWorldDestructionProductBatch& Batch)
{
	if (bPrepared || !Batch.IsValid())
	{
		return false;
	}
	const FWorldDestructionDefinition& Products = *Batch.Definition;
	UWorldObjectDefinition* ProductDefinition = Products.ProductClass
		? Products.ProductClass->GetDefaultObject<UWorldObjectDefinition>()
		: nullptr;
	if (!ProductDefinition || !ProductDefinition->IsDefinitionValid())
	{
		return false;
	}

	FRandomStream Random(MakeProductSeed(Batch));
	const int32 ProductCount = Random.RandRange(
		Products.MinimumProductCount,
		Products.MaximumProductCount);
	TArray<FWorldObjectCreateDesc> ProductDescs;
	ProductDescs.Reserve(ProductCount);
	for (int32 ProductIndex = 0; ProductIndex < ProductCount; ++ProductIndex)
	{
		const float Scale = Random.FRandRange(
			Products.UniformScaleRange.X,
			Products.UniformScaleRange.Y);
		const FVector Offset(
			Random.FRandRange(-Products.SpawnOffsetExtent.X, Products.SpawnOffsetExtent.X),
			Random.FRandRange(-Products.SpawnOffsetExtent.Y, Products.SpawnOffsetExtent.Y),
			Random.FRandRange(0.0f, Products.SpawnOffsetExtent.Z));
		const float Azimuth = Random.FRandRange(0.0f, 2.0f * UE_PI);
		const float HorizontalSpeed = Random.FRandRange(
			Products.HorizontalSpeedRange.X,
			Products.HorizontalSpeedRange.Y);

		FWorldObjectCreateDesc Desc;
		Desc.Definition = ProductDefinition;
		Desc.WorldTransform = FTransform(
			FRotator(
				Random.FRandRange(-180.0f, 180.0f),
				Random.FRandRange(-180.0f, 180.0f),
				Random.FRandRange(-180.0f, 180.0f)),
			Batch.SourceBounds.GetCenter() + Offset,
			FVector(Scale));
		Desc.MotionState = EWorldObjectMotionState::Physics;
		Desc.InstanceInteractionBounds = ProductDefinition->InteractionLocalBounds;
		FWorldObjectPhysicsBodyInit PhysicsBody;
		PhysicsBody.MassKg = Products.ProductMassKg;
		PhysicsBody.CollisionPolicy = EWorldObjectPhysicsCollisionPolicy::LooseDebris;
		PhysicsBody.LinearVelocity = FVector(
			FMath::Cos(Azimuth) * HorizontalSpeed,
			FMath::Sin(Azimuth) * HorizontalSpeed,
			Random.FRandRange(Products.UpwardSpeedRange.X, Products.UpwardSpeedRange.Y));
		PhysicsBody.AngularVelocityDegrees =
			RandomSignedVector(Random, Products.AngularSpeedRange);
		Desc.PhysicsBody = PhysicsBody;
		ProductDescs.Add(MoveTemp(Desc));
	}

	bPrepared = WorldObjects.StageCreateEntities(ProductDescs, StagedProducts);
	return bPrepared;
}

void FPhysicsWorldObjectProductSink::Commit()
{
	checkf(bPrepared, TEXT("未 Prepare 的 WorldObject 产品批次不能提交。"));
	TArray<FWorldObjectEntityHandle> CreatedProducts;
	checkf(WorldObjects.CommitStagedCreateEntities(StagedProducts, CreatedProducts),
		TEXT("源 GameplayDestroy 后，Prepare 完成的 WorldObject 产品必须无失败发布。"));
	checkf(!CreatedProducts.IsEmpty(), TEXT("破坏产品提交不能产生空批次。"));
	bPrepared = false;
}

void FPhysicsWorldObjectProductSink::Rollback()
{
	if (bPrepared)
	{
		WorldObjects.RollbackStagedCreateEntities(StagedProducts);
		bPrepared = false;
	}
}
}
