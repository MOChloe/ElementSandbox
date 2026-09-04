#include "Storage/WorldObjectWorldStorageAdapter.h"

#include "WorldObjectWorldSubsystem.h"

#include "Components/PrimitiveComponent.h"
#include "Definition/WorldObjectDefinition.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Projection/WorldObjectPhysicsProxyActor.h"
#include "Projection/WorldObjectProxyComponent.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Storage/WorldObjectPersistenceExtension.h"
#include "WorldObjectWorldInternal.h"

using namespace UE::ElementSandbox::WorldObjects::Private;

namespace
{
constexpr uint32 WorldObjectPayloadMagic = 0x31424f57; // WOB1
constexpr uint16 WorldObjectPayloadVersion = 3;
constexpr int32 MaximumWorldObjectSectionBytes = 16 * 1024 * 1024;

struct FDecodedWorldObjectSection final
{
	FName Id = NAME_None;
	uint16 Version = 0;
	TArray<uint8> Payload;
};

struct FDecodedWorldObjectPayload final
{
	EWorldObjectMotionState MotionState = EWorldObjectMotionState::Dormant;
	TOptional<FBox> InstanceInteractionBounds;
	TOptional<FWorldObjectShapeDefinition> InstanceShapeGeometry;
	uint64 InstanceShapeRevision = 1;
	TOptional<FWorldObjectPhysicsBodyInit> PhysicsBody;
	TArray<FDecodedWorldObjectSection> Sections;
};

void SerializeWorldObjectShape(FArchive& Archive, FWorldObjectShapeDefinition& Shape, uint64& ShapeRevision)
{
	uint8 Kind = static_cast<uint8>(Shape.Kind);
	Archive << Kind << Shape.Center << Shape.Rotation << Shape.HalfExtents << Shape.Radius << Shape.CapsuleAxis
			<< Shape.CapsuleSegmentHalfLength << Shape.TemplateRevision << ShapeRevision;
	if (Archive.IsLoading())
	{
		Shape.Kind = static_cast<EWorldObjectShapeKind>(Kind);
	}
}

bool DecodeWorldObjectPayload(const TArray<uint8>& Payload, FDecodedWorldObjectPayload& OutPayload, FString& OutError)
{
	OutPayload = {};
	if (Payload.IsEmpty())
	{
		return true;
	}
	FMemoryReader Reader(const_cast<TArray<uint8>&>(Payload), true);
	uint32 Magic = 0;
	uint16 Version = 0;
	uint8 MotionState = 0;
	uint8 Flags = 0;
	Reader << Magic << Version << MotionState << Flags;
	if (Magic != WorldObjectPayloadMagic || Version != WorldObjectPayloadVersion ||
		MotionState > static_cast<uint8>(EWorldObjectMotionState::Physics) || (Flags & ~uint8(0x07)) != 0)
	{
		OutError = TEXT("WorldObject Payload 格式或版本不匹配；请重新生成种子存档。");
		return false;
	}
	OutPayload.MotionState = static_cast<EWorldObjectMotionState>(MotionState);
	if (OutPayload.MotionState == EWorldObjectMotionState::Attached)
	{
		OutError = TEXT("Attached WorldObject 由角色/背包存档拥有，不能出现在世界 Chunk。");
		return false;
	}
	if ((Flags & 0x01) != 0)
	{
		FBox Bounds(ForceInit);
		Reader << Bounds.Min << Bounds.Max;
		Bounds.IsValid = 1;
		if (Bounds.ContainsNaN() || Bounds.GetExtent().GetMin() <= UE_SMALL_NUMBER)
		{
			OutError = TEXT("WorldObject Payload 的实例 Bounds 非法。");
			return false;
		}
		OutPayload.InstanceInteractionBounds = Bounds;
	}
	if ((Flags & 0x04) != 0)
	{
		FWorldObjectShapeDefinition Shape;
		SerializeWorldObjectShape(Reader, Shape, OutPayload.InstanceShapeRevision);
		if (Reader.IsError() || !Shape.IsValid() || OutPayload.InstanceShapeRevision == 0)
		{
			OutError = TEXT("WorldObject Payload 的实例 Shape 非法。");
			return false;
		}
		OutPayload.InstanceShapeGeometry = Shape;
	}
	if ((Flags & 0x02) != 0)
	{
		FWorldObjectPhysicsBodyInit Physics;
		uint8 CollisionPolicy = 0;
		Reader << Physics.MassKg << CollisionPolicy << Physics.LinearVelocity << Physics.AngularVelocityDegrees;
		Physics.CollisionPolicy = static_cast<EWorldObjectPhysicsCollisionPolicy>(CollisionPolicy);
		if (!Physics.IsValid())
		{
			OutError = TEXT("WorldObject Payload 的 Physics 状态非法。");
			return false;
		}
		OutPayload.PhysicsBody = Physics;
	}
	uint16 SectionCount = 0;
	Reader << SectionCount;
	TSet<FName> SeenSections;
	OutPayload.Sections.Reserve(SectionCount);
	for (uint16 SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
	{
		FString SectionName;
		FDecodedWorldObjectSection& Section = OutPayload.Sections.AddDefaulted_GetRef();
		int32 PayloadSize = 0;
		Reader << SectionName << Section.Version << PayloadSize;
		Section.Id = FName(*SectionName);
		if (Section.Id.IsNone() || Section.Version == 0 || PayloadSize <= 0 ||
			PayloadSize > MaximumWorldObjectSectionBytes || SeenSections.Contains(Section.Id) ||
			Reader.Tell() + PayloadSize > Reader.TotalSize())
		{
			OutError = TEXT("WorldObject Payload 包含非法或重复 Section。");
			return false;
		}
		SeenSections.Add(Section.Id);
		Section.Payload.SetNumUninitialized(PayloadSize);
		Reader.Serialize(Section.Payload.GetData(), PayloadSize);
	}
	if (Reader.IsError() || Reader.Tell() != Reader.TotalSize())
	{
		OutError = TEXT("WorldObject Payload 截断或存在尾随字节。");
		return false;
	}
	return true;
}

} // namespace

/** WorldObject Chunk Section 的原子 Capture/Restore/Remove 边界。 */
class FWorldObjectWorldStorageAdapter final : public IWorldStorageDomainAdapter
{
public:
	explicit FWorldObjectWorldStorageAdapter(UWorldObjectWorldSubsystem& InOwner) : Owner(&InOwner) {}

	virtual EWorldEntityDomain GetDomain() const override { return EWorldEntityDomain::WorldObject; }

	virtual EWorldStorageRestorePhase GetRestorePhase() const override { return EWorldStorageRestorePhase::Primary; }

	virtual bool CaptureBatch(const TConstArrayView<FWorldEntityId> EntityIds,
							  TArray<FWorldPersistentEntityRecord>& OutRecords, FString& OutError) const override
	{
		UWorldObjectWorldSubsystem* WorldObjects = Owner.Get();
		if (!WorldObjects || !WorldObjects->Runtime)
		{
			OutError = TEXT("WorldObject Runtime 不可用。");
			return false;
		}
		OutRecords.Reserve(OutRecords.Num() + EntityIds.Num());
		for (const FWorldEntityId EntityId : EntityIds)
		{
			const FWorldObjectEntityHandle Entity = WorldObjects->FindEntity(EntityId);
			const FWorldObjectWorldIdentityFragment* Identity =
				WorldObjects->Runtime->Core.Registry.FindFragment<FWorldObjectWorldIdentityFragment>(Entity);
			const FWorldObjectDefinitionFragment* DefinitionFragment =
				WorldObjects->Runtime->Core.Registry.FindFragment<FWorldObjectDefinitionFragment>(Entity);
			const FWorldObjectTransformFragment* Transform =
				WorldObjects->Runtime->Core.Registry.FindFragment<FWorldObjectTransformFragment>(Entity);
			const FWorldObjectMotionFragment* Motion =
				WorldObjects->Runtime->Core.Registry.FindFragment<FWorldObjectMotionFragment>(Entity);
			const UWorldObjectDefinition* Definition =
				DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
			if (!Entity.IsSet() || !Identity || Identity->WorldEntityId != EntityId || Identity->StateRevision == 0 ||
				!Definition || !Transform || !Motion || Motion->State == EWorldObjectMotionState::Attached ||
				!WorldObjects->Runtime->Persistence.OwnedEntities.Contains(EntityId))
			{
				OutError = FString::Printf(TEXT("WorldObject %llu 缺少可持久化核心状态或不属于世界存档。"),
										   EntityId.GetValue());
				return false;
			}

			TArray<FDecodedWorldObjectSection> Sections;
			for (const FName SectionId : WorldObjects->Runtime->Persistence.ExtensionOrder)
			{
				const TSharedRef<IWorldObjectPersistenceExtension>& Extension =
					WorldObjects->Runtime->Persistence.Extensions.FindChecked(SectionId);
				FDecodedWorldObjectSection Section;
				Section.Id = SectionId;
				Section.Version = Extension->GetSectionVersion();
				if (Section.Version == 0 ||
					!Extension->Capture(WorldObjects->Runtime->Core.Registry, Entity, Section.Payload, OutError))
				{
					return false;
				}
				if (!Section.Payload.IsEmpty())
				{
					Sections.Add(MoveTemp(Section));
				}
			}

			const FWorldObjectInstanceInteractionBoundsFragment* Bounds =
				WorldObjects->Runtime->Core.Registry.FindFragment<FWorldObjectInstanceInteractionBoundsFragment>(
					Entity);
			const FWorldObjectInstanceShapeFragment* InstanceShape =
				WorldObjects->Runtime->Core.Registry.FindFragment<FWorldObjectInstanceShapeFragment>(Entity);
			const FWorldObjectPhysicsBodyFragment* Physics =
				WorldObjects->Runtime->Core.Registry.FindFragment<FWorldObjectPhysicsBodyFragment>(Entity);
			FWorldObjectPhysicsBodyInit PhysicsSnapshot;
			if (Physics)
			{
				PhysicsSnapshot.MassKg = Physics->MassKg;
				PhysicsSnapshot.CollisionPolicy = Physics->CollisionPolicy;
				PhysicsSnapshot.LinearVelocity = Physics->InitialLinearVelocity;
				PhysicsSnapshot.AngularVelocityDegrees = Physics->InitialAngularVelocityDegrees;
				if (const UWorldObjectProxyComponent* Proxy = WorldObjects->GetProxy(Entity))
				{
					if (UPrimitiveComponent* Primitive = Proxy->GetPhysicsPrimitive();
						IsValid(Primitive) && Primitive->IsSimulatingPhysics())
					{
						PhysicsSnapshot.LinearVelocity = Primitive->GetPhysicsLinearVelocity();
						PhysicsSnapshot.AngularVelocityDegrees = Primitive->GetPhysicsAngularVelocityInDegrees();
					}
				}
				if (!PhysicsSnapshot.IsValid())
				{
					OutError = TEXT("WorldObject Physics 快照非法。");
					return false;
				}
			}

			FWorldPersistentEntityRecord& Record = OutRecords.AddDefaulted_GetRef();
			Record.EntityId = EntityId;
			Record.Domain = EWorldEntityDomain::WorldObject;
			Record.DefinitionId = Definition->DefinitionId;
			Record.WorldTransform = Transform->WorldTransform;
			Record.StateRevision = Identity->StateRevision;
			const bool bNeedsPayload = Motion->State != EWorldObjectMotionState::Dormant || Bounds || InstanceShape ||
									   Physics || !Sections.IsEmpty();
			if (!bNeedsPayload)
			{
				continue;
			}

			FMemoryWriter Writer(Record.Payload, true);
			uint32 Magic = WorldObjectPayloadMagic;
			uint16 Version = WorldObjectPayloadVersion;
			uint8 MotionByte = static_cast<uint8>(Motion->State);
			uint8 Flags = (Bounds ? 0x01 : 0x00) | (Physics ? 0x02 : 0x00) | (InstanceShape ? 0x04 : 0x00);
			Writer << Magic << Version << MotionByte << Flags;
			if (Bounds)
			{
				FVector Min = Bounds->InteractionLocalBounds.Min;
				FVector Max = Bounds->InteractionLocalBounds.Max;
				Writer << Min << Max;
			}
			if (InstanceShape)
			{
				FWorldObjectShapeDefinition Shape = InstanceShape->ShapeGeometry;
				uint64 ShapeRevision = InstanceShape->Revision;
				SerializeWorldObjectShape(Writer, Shape, ShapeRevision);
			}
			if (Physics)
			{
				uint8 CollisionPolicy = static_cast<uint8>(PhysicsSnapshot.CollisionPolicy);
				Writer << PhysicsSnapshot.MassKg << CollisionPolicy << PhysicsSnapshot.LinearVelocity
					   << PhysicsSnapshot.AngularVelocityDegrees;
			}
			uint16 SectionCount = static_cast<uint16>(Sections.Num());
			Writer << SectionCount;
			for (FDecodedWorldObjectSection& Section : Sections)
			{
				FString SectionName = Section.Id.ToString();
				int32 PayloadSize = Section.Payload.Num();
				Writer << SectionName << Section.Version << PayloadSize;
				Writer.Serialize(Section.Payload.GetData(), PayloadSize);
			}
			if (Writer.IsError())
			{
				OutError = TEXT("WorldObject Payload 编码失败。");
				return false;
			}
		}
		return true;
	}

	virtual bool RestoreBatch(const FWorldChunkCoord& HomeChunk,
							  const TConstArrayView<FWorldPersistentEntityRecord> Records, FString& OutError) override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(WorldObjects_WorldStorage_RestoreBatch);
		UWorldObjectWorldSubsystem* WorldObjects = Owner.Get();
		if (!WorldObjects || !WorldObjects->Runtime)
		{
			OutError = TEXT("WorldObject Runtime 不可用。");
			return false;
		}
		if (Records.IsEmpty())
		{
			return true;
		}

		// 任何 Runtime 写入前完成整批格式、Definition、Bounds 与扩展版本检查。
		int32 NewEntityCount = 0;
		int32 InstanceBoundsCount = 0;
		int32 InstanceShapeCount = 0;
		int32 PhysicsBodyCount = 0;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(WorldObjects_WorldStorage_Prevalidate);
			TSet<FWorldEntityId> SeenEntityIds;
			SeenEntityIds.Reserve(Records.Num());
			for (const FWorldPersistentEntityRecord& Record : Records)
			{
				if (!Record.IsValid() || Record.Domain != EWorldEntityDomain::WorldObject ||
					FWorldChunkCoord::FromWorldLocation(Record.WorldTransform.GetLocation()) != HomeChunk ||
					SeenEntityIds.Contains(Record.EntityId))
				{
					OutError = TEXT("WorldObject Restore 全批预检发现非法、重复记录或错误 HomeChunk。");
					return false;
				}
				SeenEntityIds.Add(Record.EntityId);
				FDecodedWorldObjectPayload Payload;
				if (!DecodeWorldObjectPayload(Record.Payload, Payload, OutError))
				{
					return false;
				}
				UWorldObjectDefinition* Definition = WorldObjects->FindDefinition(Record.DefinitionId);
				FBox IgnoredBounds(ForceInit);
				if (!Definition || !TryCalculateWorldBounds(*Definition,
														Payload.InstanceInteractionBounds.IsSet()
															? &Payload.InstanceInteractionBounds.GetValue()
															: nullptr,
														Record.WorldTransform, IgnoredBounds))
				{
					OutError = FString::Printf(TEXT("WorldObject Restore Definition 未注册或 Bounds 非法：%s。"),
												   *Record.DefinitionId.ToString());
					return false;
				}
				for (const FDecodedWorldObjectSection& Section : Payload.Sections)
				{
					const TSharedRef<IWorldObjectPersistenceExtension>* Extension =
						WorldObjects->Runtime->Persistence.Extensions.Find(Section.Id);
					if (!Extension || Extension->Get().GetSectionVersion() != Section.Version)
					{
						OutError = FString::Printf(TEXT("WorldObject Section %s 缺失或版本不匹配；请重新生成种子存档。"),
													   *Section.Id.ToString());
						return false;
					}
				}
				NewEntityCount += WorldObjects->FindEntity(Record.EntityId).IsSet() ? 0 : 1;
				InstanceBoundsCount += Payload.InstanceInteractionBounds.IsSet() ? 1 : 0;
				InstanceShapeCount += Payload.InstanceShapeGeometry.IsSet() ? 1 : 0;
				PhysicsBodyCount += Payload.PhysicsBody.IsSet() ? 1 : 0;
			}
		}

		FWorldObjectEntityRegistry& Registry = WorldObjects->Runtime->Core.Registry;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(WorldObjects_WorldStorage_Reserve);
			Registry.ReserveEntities(NewEntityCount);
			Registry.ReserveFragments<FWorldObjectDefinitionFragment>(NewEntityCount);
			Registry.ReserveFragments<FWorldObjectTransformFragment>(NewEntityCount);
			Registry.ReserveFragments<FWorldObjectMotionFragment>(NewEntityCount);
			Registry.ReserveFragments<FWorldObjectWorldIdentityFragment>(NewEntityCount);
			Registry.ReserveFragments<FWorldObjectInstanceInteractionBoundsFragment>(InstanceBoundsCount);
			Registry.ReserveFragments<FWorldObjectInstanceShapeFragment>(InstanceShapeCount);
			Registry.ReserveFragments<FWorldObjectPhysicsBodyFragment>(PhysicsBodyCount);
			WorldObjects->Runtime->Core.EntityByWorldEntityId.Reserve(
				WorldObjects->Runtime->Core.EntityByWorldEntityId.Num() + NewEntityCount);
			WorldObjects->Runtime->Persistence.OwnedEntities.Reserve(
				WorldObjects->Runtime->Persistence.OwnedEntities.Num() + NewEntityCount);
			WorldObjects->Runtime->Persistence.HomeChunks.Reserve(
				WorldObjects->Runtime->Persistence.HomeChunks.Num() + NewEntityCount);
		}
		struct FAppliedRecord final
		{
			FWorldEntityId EntityId;
			bool bCreated = false;
			TOptional<FWorldPersistentEntityRecord> Backup;
			TOptional<FWorldObjectShapeInstanceSnapshot> PreviousShape;
		};
		TArray<FAppliedRecord> Applied;
		Applied.Reserve(Records.Num());
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(WorldObjects_WorldStorage_ApplyRecords);
			for (const FWorldPersistentEntityRecord& Record : Records)
			{
				FAppliedRecord AppliedRecord;
				AppliedRecord.EntityId = Record.EntityId;
				AppliedRecord.bCreated = !WorldObjects->FindEntity(Record.EntityId).IsSet();
				if (!AppliedRecord.bCreated)
				{
					FWorldObjectShapeInstanceSnapshot PreviousShape;
					if (!WorldObjects->BuildShapeSnapshot(WorldObjects->FindEntity(Record.EntityId), PreviousShape))
					{
						OutError = TEXT("WorldObject Restore 无法捕获旧 Shape 快照。");
						Rollback(*WorldObjects, Applied);
						return false;
					}
					AppliedRecord.PreviousShape = MoveTemp(PreviousShape);
					TArray<FWorldPersistentEntityRecord> BackupRecords;
					if (!CaptureBatch(MakeArrayView(&Record.EntityId, 1), BackupRecords, OutError) ||
						BackupRecords.Num() != 1)
					{
						Rollback(*WorldObjects, Applied);
						return false;
					}
					AppliedRecord.Backup = MoveTemp(BackupRecords[0]);
				}
				Applied.Add(MoveTemp(AppliedRecord));
				if (!ApplyRecord(*WorldObjects, Record, HomeChunk, OutError))
				{
					Rollback(*WorldObjects, Applied);
					return false;
				}
			}
		}
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(WorldObjects_WorldStorage_RebuildStaticIndex);
			WorldObjects->Runtime->Core.SpatialIndex.RebuildStaticIfDirty();
		}
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(WorldObjects_WorldStorage_PublishQuerySnapshots);
			verify(WorldObjects->Runtime->Core.QuerySnapshots.BeginTransaction());
			TArray<FWorldObjectLifecycleRecord> Published;
			Published.Reserve(Records.Num());
			for (int32 RecordIndex = 0; RecordIndex < Records.Num(); ++RecordIndex)
			{
				const FWorldPersistentEntityRecord& Record = Records[RecordIndex];
				FWorldObjectLifecycleRecord& Lifecycle = Published.AddDefaulted_GetRef();
				if (!WorldObjects->BuildLifecycleRecord(WorldObjects->FindEntity(Record.EntityId), Lifecycle))
				{
					Published.Pop(EAllowShrinking::No);
				}
				const FWorldObjectEntityHandle CurrentEntity = WorldObjects->FindEntity(Record.EntityId);
				if (UWorldObjectProxyComponent* Proxy = WorldObjects->GetProxy(CurrentEntity))
				{
					if (AWorldObjectPhysicsProxyActor* PhysicsProxy = Cast<AWorldObjectPhysicsProxyActor>(Proxy->GetOwner()))
						PhysicsProxy->RefreshClientPhysicsProjection();
				}
				FWorldObjectShapeInstanceSnapshot CurrentShape;
				if (WorldObjects->BuildShapeSnapshot(CurrentEntity, CurrentShape))
				{
					verify(WorldObjects->PublishShapeTransition(
						Applied[RecordIndex].PreviousShape,
						TOptional<FWorldObjectShapeInstanceSnapshot>(CurrentShape),
						EWorldObjectQuerySnapshotChangeKind::Upsert,
						EWorldObjectQuerySnapshotChangeKind::ShapeRemove,
						GetEffectiveTimeMilliseconds(WorldObjects->GetWorldRef())));
				}
			}
			verify(WorldObjects->Runtime->Core.QuerySnapshots.CommitTransaction());
			if (!Published.IsEmpty())
			{
				WorldObjects->EntitiesUpsertedEvent.Broadcast(Published);
			}
		}
		for (const FWorldPersistentEntityRecord& Record : Records)
		{
			WorldObjects->ScheduleAutomaticPhysicsRelease(
				WorldObjects->FindEntity(Record.EntityId));
		}
		return true;
	}

	virtual bool RuntimeEvictBatch(const FWorldChunkCoord& HomeChunk, const TConstArrayView<FWorldEntityId> EntityIds,
								   FString& OutError) override
	{
		return RemoveBatch(HomeChunk, EntityIds, UWorldObjectWorldSubsystem::ERemovalSemantic::RuntimeEvict, OutError);
	}

	virtual bool GameplayDestroyBatch(const FWorldChunkCoord& HomeChunk,
									  const TConstArrayView<FWorldEntityId> EntityIds, FString& OutError) override
	{
		return RemoveBatch(HomeChunk, EntityIds, UWorldObjectWorldSubsystem::ERemovalSemantic::GameplayDestroy,
						   OutError);
	}

	virtual bool LeaveInterestBatch(const FWorldChunkCoord& HomeChunk, const TConstArrayView<FWorldEntityId> EntityIds,
									FString& OutError) override
	{
		return RemoveBatch(HomeChunk, EntityIds, UWorldObjectWorldSubsystem::ERemovalSemantic::LeaveInterest, OutError);
	}

	virtual bool RollbackRestoreBatch(const FWorldChunkCoord& HomeChunk,
									  const TConstArrayView<FWorldEntityId> EntityIds, FString& OutError) override
	{
		return RemoveBatch(HomeChunk, EntityIds,
						   UWorldObjectWorldSubsystem::ERemovalSemantic::FailedRegistrationRollback, OutError);
	}

	bool RemoveBatch(const FWorldChunkCoord& HomeChunk, const TConstArrayView<FWorldEntityId> EntityIds,
					 const UWorldObjectWorldSubsystem::ERemovalSemantic Semantic, FString& OutError)
	{
		UWorldObjectWorldSubsystem* WorldObjects = Owner.Get();
		if (!WorldObjects || !WorldObjects->Runtime)
		{
			OutError = TEXT("WorldObject Runtime 不可用。");
			return false;
		}
		TArray<FWorldPersistentEntityRecord> Backups;
		if (!CaptureBatch(EntityIds, Backups, OutError) || Backups.Num() != EntityIds.Num())
		{
			return false;
		}
		for (const FWorldPersistentEntityRecord& Backup : Backups)
		{
			// Client 活动物件的预测位姿可以先跨边界；卸载按已接收的权威归属核对。
			const FWorldChunkCoord* RegisteredHome = WorldObjects->Runtime->Persistence.HomeChunks.Find(Backup.EntityId);
			if (!RegisteredHome || *RegisteredHome != HomeChunk)
			{
				OutError = TEXT("WorldObject 批量移除的 HomeChunk 不匹配。");
				return false;
			}
		}
		TArray<FWorldObjectLifecycleRecord> RemovedRecords;
		RemovedRecords.Reserve(EntityIds.Num());
		TArray<FWorldObjectShapeInstanceSnapshot> RemovedShapes;
		for (const FWorldEntityId EntityId : EntityIds)
		{
			FWorldObjectLifecycleRecord& Lifecycle = RemovedRecords.AddDefaulted_GetRef();
			if (!WorldObjects->BuildLifecycleRecord(WorldObjects->FindEntity(EntityId), Lifecycle))
			{
				OutError = FString::Printf(TEXT("WorldObject %llu 批量移除缺少生命周期状态。"), EntityId.GetValue());
				return false;
			}
			const FWorldObjectEntityHandle Entity = WorldObjects->FindEntity(EntityId);
			FWorldObjectShapeInstanceSnapshot& Shape = RemovedShapes.AddDefaulted_GetRef();
			if (!WorldObjects->BuildShapeSnapshot(Entity, Shape))
			{
				OutError = FString::Printf(TEXT("WorldObject %llu 批量移除缺少 Shape 状态。"), EntityId.GetValue());
				return false;
			}
		}
		int32 DestroyedCount = 0;
		for (const FWorldEntityId EntityId : EntityIds)
		{
			if (!WorldObjects->DestroyEntityInternal(WorldObjects->FindEntity(EntityId), Semantic, true, false, false))
			{
				for (int32 Index = 0; Index < DestroyedCount; ++Index)
				{
					FString Ignored;
					if (ApplyRecord(*WorldObjects, Backups[Index], HomeChunk, Ignored))
					{
						WorldObjects->ScheduleAutomaticPhysicsRelease(
							WorldObjects->FindEntity(Backups[Index].EntityId));
					}
				}
				OutError = FString::Printf(TEXT("WorldObject %llu 批量移除失败。"), EntityId.GetValue());
				return false;
			}
			++DestroyedCount;
		}
		WorldObjects->Runtime->Core.SpatialIndex.RebuildStaticIfDirty();

		EWorldObjectQuerySnapshotChangeKind RemovalKind = EWorldObjectQuerySnapshotChangeKind::RuntimeEvict;
		switch (Semantic)
		{
		case UWorldObjectWorldSubsystem::ERemovalSemantic::GameplayDestroy:
			RemovalKind = EWorldObjectQuerySnapshotChangeKind::GameplayDestroy;
			break;
		case UWorldObjectWorldSubsystem::ERemovalSemantic::RuntimeEvict:
			RemovalKind = EWorldObjectQuerySnapshotChangeKind::RuntimeEvict;
			break;
		case UWorldObjectWorldSubsystem::ERemovalSemantic::LeaveInterest:
			RemovalKind = EWorldObjectQuerySnapshotChangeKind::LeaveInterest;
			break;
		case UWorldObjectWorldSubsystem::ERemovalSemantic::FailedRegistrationRollback:
			RemovalKind = EWorldObjectQuerySnapshotChangeKind::FailedRegistrationRollback;
			break;
		default:
			checkNoEntry();
			return false;
		}
		verify(WorldObjects->Runtime->Core.QuerySnapshots.BeginTransaction());
		for (const FWorldObjectShapeInstanceSnapshot& Shape : RemovedShapes)
		{
			verify(WorldObjects->PublishShapeTransition(Shape, {}, EWorldObjectQuerySnapshotChangeKind::Metadata, RemovalKind,
														GetEffectiveTimeMilliseconds(WorldObjects->GetWorldRef())));
		}
		verify(WorldObjects->Runtime->Core.QuerySnapshots.CommitTransaction());
		if (Semantic == UWorldObjectWorldSubsystem::ERemovalSemantic::RuntimeEvict)
		{
			WorldObjects->EntitiesRuntimeEvictedEvent.Broadcast(RemovedRecords);
		}
		else if (Semantic == UWorldObjectWorldSubsystem::ERemovalSemantic::GameplayDestroy)
		{
			WorldObjects->EntitiesGameplayDestroyedEvent.Broadcast(RemovedRecords);
		}
		return true;
	}

	virtual bool CanRuntimeEvict(const FWorldEntityId EntityId) const override
	{
		const UWorldObjectWorldSubsystem* WorldObjects = Owner.Get();
		if (!WorldObjects || !WorldObjects->Runtime)
		{
			return false;
		}
		const FWorldObjectEntityHandle Entity = WorldObjects->FindEntity(EntityId);
		const FWorldObjectMotionFragment* Motion =
			WorldObjects->Runtime->Core.Registry.FindFragment<FWorldObjectMotionFragment>(Entity);
		return Motion && Motion->State == EWorldObjectMotionState::Dormant;
	}

private:
	static UWorldObjectProxyComponent* SpawnPhysicsProxy(UWorldObjectWorldSubsystem& WorldObjects,
														 const FTransform& WorldTransform, const FBox& InstanceBounds,
													 const FWorldObjectPhysicsBodyInit& Physics,
													 const uint32 ActivationRevision)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags |= RF_Transient;
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWorldObjectPhysicsProxyActor* Actor =
			WorldObjects.GetWorld()->SpawnActor<AWorldObjectPhysicsProxyActor>(SpawnParameters);
		if (!IsValid(Actor))
		{
			return nullptr;
		}
		Actor->SetActorTransform(AWorldObjectPhysicsProxyActor::MakeActorTransform(WorldTransform, InstanceBounds),
								 false, nullptr, ETeleportType::TeleportPhysics);
		if (!Actor->ConfigurePhysics(InstanceBounds, Physics.MassKg, Physics.CollisionPolicy, Physics.LinearVelocity,
									 Physics.AngularVelocityDegrees, ActivationRevision))
		{
			Actor->Destroy();
			return nullptr;
		}
		return Actor->GetWorldObjectProxyComponent();
	}

	static bool ApplyRecord(UWorldObjectWorldSubsystem& WorldObjects, const FWorldPersistentEntityRecord& Record,
							const FWorldChunkCoord& HomeChunk, FString& OutError)
	{
		FDecodedWorldObjectPayload Payload;
		if (!DecodeWorldObjectPayload(Record.Payload, Payload, OutError))
		{
			return false;
		}
		TMap<FName, const FDecodedWorldObjectSection*> Sections;
		for (const FDecodedWorldObjectSection& Section : Payload.Sections)
		{
			const TSharedRef<IWorldObjectPersistenceExtension>* Extension =
				WorldObjects.Runtime->Persistence.Extensions.Find(Section.Id);
			if (!Extension || Extension->Get().GetSectionVersion() != Section.Version)
			{
				OutError = FString::Printf(TEXT("WorldObject Section %s 缺失或版本不匹配；请重新生成种子存档。"),
										   *Section.Id.ToString());
				return false;
			}
			Sections.Add(Section.Id, &Section);
		}

		UWorldObjectDefinition* Definition = WorldObjects.FindDefinition(Record.DefinitionId);
		if (!Definition)
		{
			OutError = FString::Printf(TEXT("WorldObject Definition 未注册：%s。"), *Record.DefinitionId.ToString());
			return false;
		}
		UWorldObjectProxyComponent* SpawnedProxy = nullptr;
		if (Payload.MotionState == EWorldObjectMotionState::Physics &&
			WorldObjects.GetWorld()->GetNetMode() != NM_Client)
		{
			if (!Payload.InstanceInteractionBounds.IsSet() || !Payload.PhysicsBody.IsSet())
			{
				OutError = TEXT("Physics WorldObject 缺少实例 Bounds 或 Physics 快照。");
				return false;
			}
			SpawnedProxy =
				SpawnPhysicsProxy(WorldObjects, Record.WorldTransform, Payload.InstanceInteractionBounds.GetValue(),
								  Payload.PhysicsBody.GetValue(), Record.StateRevision);
			if (!SpawnedProxy)
			{
				OutError = TEXT("Physics WorldObject 无法恢复 Chaos Proxy。");
				return false;
			}
		}

		FWorldObjectEntityHandle Entity = WorldObjects.FindEntity(Record.EntityId);
		const bool bCreated = !Entity.IsSet();
		if (bCreated)
		{
			Entity = WorldObjects.CreateEntityInternal(
				*Definition, Record.WorldTransform, Payload.MotionState, Record.EntityId, Record.StateRevision,
				SpawnedProxy, Payload.InstanceInteractionBounds, Payload.InstanceShapeGeometry,
				Payload.InstanceShapeRevision, Payload.PhysicsBody.IsSet() ? &Payload.PhysicsBody.GetValue() : nullptr,
				true, false, false);
			if (!Entity.IsSet())
			{
				if (SpawnedProxy && SpawnedProxy->GetOwner())
				{
					SpawnedProxy->GetOwner()->Destroy();
				}
				OutError = TEXT("WorldObject ECS Restore 创建失败。");
				return false;
			}
		}
		else
		{
			if (SpawnedProxy && SpawnedProxy->GetOwner())
			{
				SpawnedProxy->GetOwner()->Destroy();
			}
			const FWorldObjectDefinitionFragment* ExistingDefinition =
				WorldObjects.Runtime->Core.Registry.FindFragment<FWorldObjectDefinitionFragment>(Entity);
			FWorldObjectTransformFragment* Transform =
				WorldObjects.Runtime->Core.Registry.FindMutableFragment<FWorldObjectTransformFragment>(Entity);
			FWorldObjectWorldIdentityFragment* Identity =
				WorldObjects.Runtime->Core.Registry.FindMutableFragment<FWorldObjectWorldIdentityFragment>(Entity);
			FWorldObjectInstanceInteractionBoundsFragment* ExistingBounds =
				WorldObjects.Runtime->Core.Registry.FindMutableFragment<FWorldObjectInstanceInteractionBoundsFragment>(
					Entity);
			FWorldObjectInstanceShapeFragment* ExistingShape =
				WorldObjects.Runtime->Core.Registry.FindMutableFragment<FWorldObjectInstanceShapeFragment>(Entity);
			const FWorldObjectPhysicsBodyFragment* ExistingPhysics =
				WorldObjects.Runtime->Core.Registry.FindFragment<FWorldObjectPhysicsBodyFragment>(Entity);
			if (!ExistingDefinition || ExistingDefinition->Definition.Get() != Definition || !Transform || !Identity ||
				Payload.InstanceInteractionBounds.IsSet() != (ExistingBounds != nullptr) ||
				Payload.InstanceShapeGeometry.IsSet() != (ExistingShape != nullptr) ||
				Payload.PhysicsBody.IsSet() != (ExistingPhysics != nullptr) ||
				(ExistingPhysics && Payload.PhysicsBody.IsSet() &&
				 ExistingPhysics->CollisionPolicy != Payload.PhysicsBody->CollisionPolicy))
			{
				OutError = TEXT("同 ID WorldObject 的 Definition 或持久化形状冲突。");
				return false;
			}
			const bool bBoundsChanged = ExistingBounds && !ExistingBounds->InteractionLocalBounds.Equals(
															  Payload.InstanceInteractionBounds.GetValue(), 0.01);
			const bool bShapeChanged =
				ExistingShape &&
				(!AreWorldObjectShapesEqual(ExistingShape->ShapeGeometry, Payload.InstanceShapeGeometry.GetValue()) ||
				 ExistingShape->Revision != Payload.InstanceShapeRevision);
			bool bTransformUpdated = true;
			if (Definition->SpatialClass == EWorldObjectSpatialClass::PermanentStatic)
			{
				if (!Transform->WorldTransform.Equals(Record.WorldTransform) || bBoundsChanged)
				{
					FBox OldBounds(ForceInit);
					FBox NewBounds(ForceInit);
					const FTransform OldTransform = Transform->WorldTransform;
					if (!WorldObjects.Runtime->Core.SpatialIndex.TryGetBounds(Entity, OldBounds) ||
						!TryCalculateWorldBounds(*Definition,
												 Payload.InstanceInteractionBounds.IsSet()
													 ? &Payload.InstanceInteractionBounds.GetValue()
													 : nullptr,
												 Record.WorldTransform, NewBounds) ||
						!WorldObjects.Runtime->Core.SpatialIndex.Remove(Entity))
					{
						bTransformUpdated = false;
					}
					else
					{
						Transform->WorldTransform = Record.WorldTransform;
						Transform->Revision = NextRevision64(Transform->Revision);
						bTransformUpdated = WorldObjects.Runtime->Core.SpatialIndex.Insert(
							Entity, NewBounds, EWorldObjectSpatialClass::PermanentStatic,
							Record.WorldTransform.GetLocation());
						if (!bTransformUpdated)
						{
							Transform->WorldTransform = OldTransform;
							Transform->Revision = Transform->Revision > 1 ? Transform->Revision - 1 : MAX_uint64;
							WorldObjects.Runtime->Core.SpatialIndex.Insert(Entity, OldBounds,
																		   EWorldObjectSpatialClass::PermanentStatic,
																		   OldTransform.GetLocation());
						}
					}
				}
			}
			else
			{
				bTransformUpdated = WorldObjects.CommitTransformInternal(Entity, Record.WorldTransform, false, false);
				if (bTransformUpdated && bBoundsChanged)
				{
					FBox NewBounds(ForceInit);
					bTransformUpdated =
						TryCalculateWorldBounds(*Definition, &Payload.InstanceInteractionBounds.GetValue(),
												Record.WorldTransform, NewBounds) &&
						WorldObjects.Runtime->Core.SpatialIndex.UpdatePortable(Entity, NewBounds);
				}
			}
			if (!bTransformUpdated || !WorldObjects.SetMotionStateInternal(Entity, Payload.MotionState, false, false))
			{
				OutError = TEXT("WorldObject ECS Restore 更新 Transform/MotionState 失败。");
				return false;
			}
			if (bBoundsChanged)
			{
				ExistingBounds->InteractionLocalBounds = Payload.InstanceInteractionBounds.GetValue();
				ExistingBounds->Revision = NextRevision64(ExistingBounds->Revision);
			}
			if (bShapeChanged)
			{
				ExistingShape->ShapeGeometry = Payload.InstanceShapeGeometry.GetValue();
				ExistingShape->Revision = Payload.InstanceShapeRevision;
			}
			Identity->StateRevision = Record.StateRevision;
			WorldObjects.Runtime->Persistence.OwnedEntities.Add(Record.EntityId);
		}
		WorldObjects.Runtime->Persistence.HomeChunks.Add(Record.EntityId, HomeChunk);

		// Definition 已为新实体写入规范默认 Fragment。新实体缺失可选
		// Section 时不遍历扩展；已有实体仍以空 Section 清除旧状态。
		const TConstArrayView<FName> ExtensionIds =
			bCreated ? TConstArrayView<FName>()
					 : TConstArrayView<FName>(WorldObjects.Runtime->Persistence.ExtensionOrder);
		for (const FName ExtensionId : ExtensionIds)
		{
			const TSharedRef<IWorldObjectPersistenceExtension>& Extension =
				WorldObjects.Runtime->Persistence.Extensions.FindChecked(ExtensionId);
			const FDecodedWorldObjectSection* const* SectionPtr = Sections.Find(ExtensionId);
			const FDecodedWorldObjectSection* Section = SectionPtr ? *SectionPtr : nullptr;
			if (!Extension->Restore(WorldObjects.Runtime->Core.Registry, Entity,
									Section ? Section->Version : Extension->GetSectionVersion(),
									Section ? TConstArrayView<uint8>(Section->Payload) : TConstArrayView<uint8>(),
									OutError))
			{
				if (bCreated)
				{
					WorldObjects.DestroyEntityInternal(
						Entity, UWorldObjectWorldSubsystem::ERemovalSemantic::FailedRegistrationRollback, true, false,
						false);
				}
				return false;
			}
		}
		if (bCreated)
		{
			for (const FDecodedWorldObjectSection& Section : Payload.Sections)
			{
				const TSharedRef<IWorldObjectPersistenceExtension>& Extension =
					WorldObjects.Runtime->Persistence.Extensions.FindChecked(Section.Id);
				if (!Extension->Restore(WorldObjects.Runtime->Core.Registry, Entity, Section.Version, Section.Payload,
										OutError))
				{
					WorldObjects.DestroyEntityInternal(
						Entity, UWorldObjectWorldSubsystem::ERemovalSemantic::FailedRegistrationRollback, true, false,
						false);
					return false;
				}
			}
		}
		return true;
	}

	template <typename TAppliedRecord>
	static void Rollback(UWorldObjectWorldSubsystem& WorldObjects, const TArray<TAppliedRecord>& Applied)
	{
		for (int32 Index = Applied.Num() - 1; Index >= 0; --Index)
		{
			const TAppliedRecord& Entry = Applied[Index];
			if (Entry.bCreated)
			{
				WorldObjects.DestroyEntityInternal(
					WorldObjects.FindEntity(Entry.EntityId),
					UWorldObjectWorldSubsystem::ERemovalSemantic::FailedRegistrationRollback, true, false, false);
			}
			else if (Entry.Backup.IsSet())
			{
				FString Ignored;
				if (ApplyRecord(WorldObjects, Entry.Backup.GetValue(),
							FWorldChunkCoord::FromWorldLocation(Entry.Backup->WorldTransform.GetLocation()), Ignored))
				{
					WorldObjects.ScheduleAutomaticPhysicsRelease(
						WorldObjects.FindEntity(Entry.EntityId));
				}
			}
		}
	}

	TWeakObjectPtr<UWorldObjectWorldSubsystem> Owner;
};

TSharedRef<IWorldStorageDomainAdapter> MakeWorldObjectWorldStorageAdapter(UWorldObjectWorldSubsystem& Owner)
{
	return MakeShared<FWorldObjectWorldStorageAdapter>(Owner);
}
