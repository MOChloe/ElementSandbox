#include "MeteorWorldSubsystem.h"

#include "Async/Async.h"
#include "BuildingWorldSubsystem.h"
#include "Chunk/WorldChunkCoordinates.h"
#include "Definition/BuildingDefinition.h"
#include "Definition/WorldDestructionDefinition.h"
#include "Definition/WorldObjectDefinition.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Entity/BuildDefinitionFragment.h"
#include "Entity/BuildEntityRegistry.h"
#include "Entity/BuildWorldIdentityFragment.h"
#include "Entity/WorldObjectCoreFragments.h"
#include "Entity/WorldObjectEntityRegistry.h"
#include "MeteorBallisticKernel.h"
#include "MeteorDebrisVisualPlan.h"
#include "MeteorPageScheduler.h"
#include "MeteorSettlementQueue.h"
#include "MeteorStrikeRuleSet.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Snapshot/BuildQuerySnapshotStream.h"
#include "Snapshot/WorldObjectQuerySnapshotStream.h"
#include "Spatial/BuildSpatialIndex.h"
#include "Spatial/WorldObjectSpatialIndex.h"
#include "Subsystems/SubsystemCollection.h"
#include "WorldDestructionAuthorityService.h"
#include "WorldObjectCreateDesc.h"
#include "WorldObjectWorldSubsystem.h"
#include "WorldStorageSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogElementSandboxMeteor, Log, All);

DECLARE_STATS_GROUP(TEXT("ElementSandboxMeteor"), STATGROUP_ElementSandboxMeteor, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("Meteor Server Pump"), STAT_MeteorServerPump, STATGROUP_ElementSandboxMeteor);
DECLARE_DWORD_COUNTER_STAT(TEXT("Meteor Query Tiles"), STAT_MeteorQueryTiles, STATGROUP_ElementSandboxMeteor);
DECLARE_DWORD_COUNTER_STAT(TEXT("Meteor Pending Targets"), STAT_MeteorPendingTargets, STATGROUP_ElementSandboxMeteor);
DECLARE_DWORD_COUNTER_STAT(TEXT("Meteor Activated Debris"), STAT_MeteorActivatedDebris, STATGROUP_ElementSandboxMeteor);
DECLARE_DWORD_COUNTER_STAT(TEXT("Meteor Settlement Backlog"), STAT_MeteorSettlementBacklog, STATGROUP_ElementSandboxMeteor);
CSV_DEFINE_CATEGORY(Meteor, true);

namespace UE::ElementSandbox::Meteor
{
	using namespace UE::ElementSandbox::Destruction;

	namespace
	{
		constexpr double QueryTileSize = 20000.0;

		double DistanceToBox2D(const FVector& Center, const FBox& Bounds)
		{
			const double X = FMath::Max3(Bounds.Min.X - Center.X, 0.0, Center.X - Bounds.Max.X);
			const double Y = FMath::Max3(Bounds.Min.Y - Center.Y, 0.0, Center.Y - Bounds.Max.Y);
			return FMath::Sqrt(X * X + Y * Y);
		}

		uint32 FoldStableSeed(const FWorldEntityId SourceId, const uint64 Revision, const uint64 EventSeed)
		{
			const uint64 Id = SourceId.GetValue();
			return HashCombineFast(
				HashCombineFast(static_cast<uint32>(Id), static_cast<uint32>(Id >> 32)),
				HashCombineFast(static_cast<uint32>(Revision),
					HashCombineFast(static_cast<uint32>(EventSeed), static_cast<uint32>(EventSeed >> 32))));
		}

		uint32 NextStateRevision(const uint32 Revision)
		{
			return Revision == MAX_uint32 ? 1u : Revision + 1u;
		}
	}

	struct FMeteorWorldRuntime final
	{
		struct FTile final
		{
			FBox Bounds = FBox(ForceInit);
			double MinimumDistance = 0.0;
		};

		struct FCandidate final
		{
			FWorldDestructionTarget Target;
			double Distance = 0.0;
		};

		struct FCompileResult final
		{
			FMeteorPageHandle Handle;
			FMeteorTrajectoryPage Page;
			bool bSucceeded = false;
		};

		struct FPreparedTarget final
		{
			FWorldDestructionTarget Target;
			FBox SourceBounds = FBox(ForceInit);
			const FWorldDestructionDefinition* Definition = nullptr;
			FWorldProductLaunchContext Launch;
			FName SettlementProductDefinitionId = NAME_None;
			double Distance = 0.0;
			double ActivationTimeSeconds = 0.0;
			uint32 BaseOrdinal = 0;
			uint32 ProductCount = 0;
			uint32 CompiledLaneCount = 0;
			bool bActivated = false;
			bool bCanceled = false;
			bool bCancellationPublished = false;

			bool HasCompiledProducts() const
			{
				return ProductCount > 0 && CompiledLaneCount == ProductCount;
			}
			bool IsReady() const { return !bCanceled && HasCompiledProducts(); }
		};

		struct FPageLaneRef final
		{
			uint64 PageId = 0;
			uint32 Revision = 0;
			uint16 Lane = MAX_uint16;

			bool IsSet() const { return PageId != 0 && Revision != 0 && Lane != MAX_uint16; }
		};

		~FMeteorWorldRuntime()
		{
			for (TFuture<void>& Future : CompileFutures)
			{
				Future.Wait();
			}
		}

		bool IsCandidateLess(const FCandidate& A, const FCandidate& B) const
		{
			if (!FMath::IsNearlyEqual(A.Distance, B.Distance)) return A.Distance < B.Distance;
			if (A.Target.Domain != B.Target.Domain) return A.Target.Domain < B.Target.Domain;
			return A.Target.WorldEntityId < B.Target.WorldEntityId;
		}

		void HeapPush(const FCandidate& Candidate)
		{
			int32 Index = CandidateHeap.Add(Candidate);
			while (Index > 0)
			{
				const int32 Parent = (Index - 1) / 2;
				if (!IsCandidateLess(CandidateHeap[Index], CandidateHeap[Parent])) break;
				Swap(CandidateHeap[Index], CandidateHeap[Parent]);
				Index = Parent;
			}
		}

		void PreparationHeapPush(const FCandidate& Candidate)
		{
			int32 Index = PreparationHeap.Add(Candidate);
			while (Index > 0)
			{
				const int32 Parent = (Index - 1) / 2;
				if (!IsCandidateLess(PreparationHeap[Index], PreparationHeap[Parent])) break;
				Swap(PreparationHeap[Index], PreparationHeap[Parent]);
				Index = Parent;
			}
		}

		bool PreparationHeapPop(FCandidate& OutCandidate)
		{
			if (PreparationHeap.IsEmpty()) return false;
			OutCandidate = PreparationHeap[0];
			PreparationHeap[0] = PreparationHeap.Last();
			PreparationHeap.Pop(EAllowShrinking::No);
			int32 Index = 0;
			while (true)
			{
				const int32 Left = Index * 2 + 1;
				const int32 Right = Left + 1;
				if (Left >= PreparationHeap.Num()) break;
				int32 Smallest = Left;
				if (Right < PreparationHeap.Num()
					&& IsCandidateLess(PreparationHeap[Right], PreparationHeap[Left])) Smallest = Right;
				if (!IsCandidateLess(PreparationHeap[Smallest], PreparationHeap[Index])) break;
				Swap(PreparationHeap[Index], PreparationHeap[Smallest]);
				Index = Smallest;
			}
			return true;
		}

			bool HeapPop(FCandidate& OutCandidate)
		{
			if (CandidateHeap.IsEmpty()) return false;
			OutCandidate = CandidateHeap[0];
			CandidateHeap[0] = CandidateHeap.Last();
			CandidateHeap.Pop(EAllowShrinking::No);
			int32 Index = 0;
			while (true)
			{
				const int32 Left = Index * 2 + 1;
				const int32 Right = Left + 1;
				if (Left >= CandidateHeap.Num()) break;
				int32 Smallest = Left;
				if (Right < CandidateHeap.Num() && IsCandidateLess(CandidateHeap[Right], CandidateHeap[Left]))
				{
					Smallest = Right;
				}
				if (!IsCandidateLess(CandidateHeap[Smallest], CandidateHeap[Index])) break;
				Swap(CandidateHeap[Index], CandidateHeap[Smallest]);
				Index = Smallest;
			}
				return true;
			}

		FMeteorRuntimeConfig Config;
		FMeteorBurstId BurstId;
		FVector ImpactCenter = FVector::ZeroVector;
		double ImpactTimeSeconds = 0.0;
		double PublishedWaveRadius = 0.0;
		uint32 NextOrdinal = 0;
		uint32 ActivatedLaneCount = 0;
		int32 CoreCandidateCount = 0;
		int32 ImpactFrameDestroyedTargets = 0;
		int32 FirstActivationLaneCount = 0;
		double ImpactToFirstActivationMilliseconds = -1.0;
		bool bImpactPumpRecorded = false;
		uint64 NextBurstValue = 1;
		bool bActive = false;

		TArray<FTile> Tiles;
		/** 只扫描当前已 Resident 的空间索引；Meteor 不拥有或改变任何 Chunk Residency。 */
		int32 NextResidentTileIndex = 0;
		TBitArray<> VisitedBuildings;
		TBitArray<> VisitedWorldObjects;
		TArray<FCandidate> CandidateHeap;
		TArray<FCandidate> PreparationHeap;
		/** 永久无法生成有效产品的目标；波前经过时跳过，不能让一个坏配置卡住整场。 */
		TSet<FWorldEntityId> RejectedPreparationTargets;
		TArray<FPreparedTarget> PreparedTargets;
		TMap<FWorldEntityId, int32> PreparedTargetById;
		TArray<int32> OrdinalOwners;
		TArray<FPageLaneRef> OrdinalPageLanes;
		FBuildSpatialQueryScratch BuildingScratch;
		FWorldObjectSpatialQueryScratch WorldObjectScratch;
		TArray<FBuildEntityHandle> BuildingHits;
		TArray<FWorldObjectEntityHandle> WorldObjectHits;

		FMeteorPageScheduler Scheduler;
		TAtomic<int32> WorkerInFlight{0};
		TQueue<TUniquePtr<FCompileResult>, EQueueMode::Mpsc> CompileResults;
		TArray<TFuture<void>> CompileFutures;
		/** 角色近场 Cell 优先 + 全局最老兜底的双索引到期队列。 */
		FMeteorSettlementQueue SettlementQueue;
		uint64 ProximitySettledLaneCount = 0;
		uint64 GlobalOldestSettledLaneCount = 0;
		TMap<uint64, FMeteorTrajectoryPage> ActivePages;
		TArray<FMeteorSettlementMapping> PublishedSettlementHistory;
		FMeteorTrajectoryPagePreparedEvent PagePreparedEvent;
		FMeteorTrajectoryActivatedEvent TrajectoryActivatedEvent;
		FMeteorTrajectoryCanceledEvent TrajectoryCanceledEvent;
		TArray<FMeteorTrajectoryActivation> PublishedActivationHistory;
		FMeteorSettlementPublishedEvent SettlementPublishedEvent;
		FDelegateHandle BuildingSnapshotHandle;
		FDelegateHandle WorldObjectSnapshotHandle;
		FWorldStorageMutationBatchHandle MutationBatch;
		double LastPumpMilliseconds = 0.0;
		double LastProgressLogSeconds = -TNumericLimits<double>::Max();
	};

		/** 从两套当前 Resident 空间索引发现一个查询 Tile 内的可破坏目标。 */
		void DiscoverTileCandidates(
			FMeteorWorldRuntime& Runtime,
			const FBox& TileBounds,
			UBuildingWorldSubsystem& Buildings,
			UWorldObjectWorldSubsystem& WorldObjects)
		{
			Buildings.GetSpatialIndex().QueryOverlaps(TileBounds, Runtime.BuildingScratch, Runtime.BuildingHits);
			for (const FBuildEntityHandle Entity : Runtime.BuildingHits)
			{
				const int32 Slot = Entity.GetIndex();
				if (Slot < 0) continue;
				if (Runtime.VisitedBuildings.Num() <= Slot) Runtime.VisitedBuildings.SetNum(Slot + 1, false);
				if (Runtime.VisitedBuildings[Slot]) continue;
				const FBuildEntityRegistry& Registry = Buildings.GetRegistry();
				const FBuildDefinitionFragment* DefFragment = Registry.FindFragment<FBuildDefinitionFragment>(Entity);
				const FBuildWorldIdentityFragment* Identity = Registry.FindFragment<FBuildWorldIdentityFragment>(Entity);
				const UBuildingDefinition* Definition = DefFragment ? DefFragment->Definition.Get() : nullptr;
				FBox Bounds(ForceInit);
				if (!Definition || !Definition->Destruction.IsEnabled() || !Identity
					|| !Buildings.GetSpatialIndex().TryGetBounds(Entity, Bounds)) continue;
				const double Distance = DistanceToBox2D(Runtime.ImpactCenter, Bounds);
				if (Distance > Runtime.Config.ShockwaveRadius) continue;
				Runtime.VisitedBuildings[Slot] = true;
				FWorldDestructionTarget Target;
				Target.Domain = EWorldDestructionTargetDomain::Building;
				Target.Building = Entity;
				Target.WorldEntityId = Identity->WorldEntityId;
				Target.SourceRevision = Identity->StateRevision;
				if (Distance <= Runtime.Config.ImpactCoreRadius) ++Runtime.CoreCandidateCount;
				Runtime.HeapPush({Target, Distance});
				Runtime.PreparationHeapPush({Target, Distance});
			}

			WorldObjects.QueryOverlap(TileBounds, Runtime.WorldObjectScratch, Runtime.WorldObjectHits);
			for (const FWorldObjectEntityHandle Entity : Runtime.WorldObjectHits)
			{
				const int32 Slot = Entity.GetSlot();
				if (Slot < 0) continue;
				if (Runtime.VisitedWorldObjects.Num() <= Slot) Runtime.VisitedWorldObjects.SetNum(Slot + 1, false);
				if (Runtime.VisitedWorldObjects[Slot]) continue;
				const FWorldObjectEntityRegistry& Registry = WorldObjects.GetRegistry();
				const FWorldObjectDefinitionFragment* DefFragment = Registry.FindFragment<FWorldObjectDefinitionFragment>(Entity);
				const FWorldObjectWorldIdentityFragment* Identity =
					Registry.FindFragment<FWorldObjectWorldIdentityFragment>(Entity);
				const UWorldObjectDefinition* Definition = DefFragment ? DefFragment->Definition.Get() : nullptr;
				FBox Bounds(ForceInit);
				if (!Definition || !Definition->Destruction.IsEnabled() || !Identity
					|| !WorldObjects.GetSpatialIndex().TryGetBounds(Entity, Bounds)) continue;
				const double Distance = DistanceToBox2D(Runtime.ImpactCenter, Bounds);
				if (Distance > Runtime.Config.ShockwaveRadius) continue;
				Runtime.VisitedWorldObjects[Slot] = true;
				FWorldDestructionTarget Target;
				Target.Domain = EWorldDestructionTargetDomain::WorldObject;
				Target.WorldObject = Entity;
				Target.WorldEntityId = Identity->WorldEntityId;
				Target.SourceRevision = Identity->StateRevision;
				if (Distance <= Runtime.Config.ImpactCoreRadius) ++Runtime.CoreCandidateCount;
				Runtime.HeapPush({Target, Distance});
				Runtime.PreparationHeapPush({Target, Distance});
			}
		}

	class FMeteorProductSink final : public IWorldDestructionProductSink
	{
	public:
		enum class EPrestageResult : uint8
		{
			Prepared,
			RetryCapacity,
			Rejected
		};

		FMeteorProductSink(UMeteorWorldSubsystem& InOwner, const int32 InPreparedTargetIndex)
			: Owner(InOwner), PreparedTargetIndex(InPreparedTargetIndex) {}

		/**
		 * 在冲击波真正提交源销毁之前生成稳定产品并送入页面编译器。这里不改 Damage
		 * Fragment、不销毁宿主，也不发布可见 Lane；因此算力不足时只会延后未发布波前。
		 */
		static EPrestageResult Prestage(
			UMeteorWorldSubsystem& Owner,
			FMeteorWorldRuntime::FCandidate& Candidate,
			const double NowSeconds)
		{
			FMeteorWorldRuntime* Runtime = Owner.Runtime.Get();
			if (!Runtime || !Runtime->bActive || !FMath::IsFinite(NowSeconds)
				|| Runtime->PreparedTargetById.Contains(Candidate.Target.WorldEntityId))
			{
				return EPrestageResult::Rejected;
			}

			FBox SourceBounds(ForceInit);
			const FWorldDestructionDefinition* Products = nullptr;
				if (Candidate.Target.Domain == EWorldDestructionTargetDomain::Building)
				{
				UBuildingWorldSubsystem* Buildings = Owner.GetWorld()->GetSubsystem<UBuildingWorldSubsystem>();
				if (!Buildings) return EPrestageResult::Rejected;
				const FBuildEntityRegistry& Registry = Buildings->GetRegistry();
				const FBuildWorldIdentityFragment* Identity =
					Registry.FindFragment<FBuildWorldIdentityFragment>(Candidate.Target.Building);
				const FBuildDefinitionFragment* DefinitionFragment =
					Registry.FindFragment<FBuildDefinitionFragment>(Candidate.Target.Building);
					const UBuildingDefinition* Definition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
					if (!Identity || !Definition || !Definition->Destruction.IsEnabled()
						|| Identity->WorldEntityId != Candidate.Target.WorldEntityId
						|| !Buildings->GetSpatialIndex().TryGetBounds(Candidate.Target.Building, SourceBounds))
				{
					return EPrestageResult::Rejected;
					}
					Candidate.Target.SourceRevision = Identity->StateRevision;
					Products = &Definition->Destruction;
				}
			else
			{
				UWorldObjectWorldSubsystem* WorldObjects = Owner.GetWorld()->GetSubsystem<UWorldObjectWorldSubsystem>();
				if (!WorldObjects) return EPrestageResult::Rejected;
				const FWorldObjectEntityRegistry& Registry = WorldObjects->GetRegistry();
				const FWorldObjectWorldIdentityFragment* Identity =
					Registry.FindFragment<FWorldObjectWorldIdentityFragment>(Candidate.Target.WorldObject);
				const FWorldObjectDefinitionFragment* DefinitionFragment =
					Registry.FindFragment<FWorldObjectDefinitionFragment>(Candidate.Target.WorldObject);
					const UWorldObjectDefinition* Definition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
					if (!Identity || !Definition || !Definition->Destruction.IsEnabled()
						|| Identity->WorldEntityId != Candidate.Target.WorldEntityId
						|| !WorldObjects->GetSpatialIndex().TryGetBounds(Candidate.Target.WorldObject, SourceBounds))
				{
					return EPrestageResult::Rejected;
					}
					Candidate.Target.SourceRevision = Identity->StateRevision;
					Products = &Definition->Destruction;
				}
				if (!Products || !Products->IsValid())
				{
					return EPrestageResult::Rejected;
			}
			const UWorldObjectDefinition* Product = Products->ProductClass
				? Products->ProductClass->GetDefaultObject<UWorldObjectDefinition>() : nullptr;
			if (!Product || !Product->IsDefinitionValid()) return EPrestageResult::Rejected;

			Candidate.Distance = DistanceToBox2D(Runtime->ImpactCenter, SourceBounds);
			// Candidate 在进入 Heap 时已经通过“波前尚未经过”的捕获门槛。预演或网络
			// 稍晚完成不能反过来令合法目标消失，否则最繁忙的近景永远不会产生碎片。
			if (Candidate.Distance > Runtime->Config.ShockwaveRadius) return EPrestageResult::Rejected;
			FWorldProductLaunchContext Launch;
			Launch.EventId = Runtime->BurstId.Value;
			Launch.ImpactCenter = Runtime->ImpactCenter;
			const float Falloff = 1.0f - FMath::Clamp(
				static_cast<float>(Candidate.Distance / Runtime->Config.ShockwaveRadius), 0.0f, 1.0f);
			Launch.RadialStrength = Runtime->Config.RadialStrength * FMath::Lerp(0.35f, 1.0f, Falloff);
			Launch.UpwardStrength = Runtime->Config.UpwardStrength * FMath::Lerp(0.45f, 1.0f, Falloff);
			Launch.DistanceFalloff = Falloff;
			Launch.RandomSeed = HashCombineFast(
				GetTypeHash(Runtime->BurstId.Value), GetTypeHash(Candidate.Target.WorldEntityId));
			if (Launch.RandomSeed == 0) Launch.RandomSeed = 1;
			const uint32 FoldedVisualPlanSeed = FoldStableSeed(
				Candidate.Target.WorldEntityId,
				Candidate.Target.SourceRevision,
				Launch.RandomSeed);
			const uint32 VisualPlanSeed = FoldedVisualPlanSeed == 0 ? 1u : FoldedVisualPlanSeed;
			FRandomStream ProductCountRandom(VisualPlanSeed);
			FMeteorDebrisVisualPlanInput VisualPlanInput;
			VisualPlanInput.SourceBounds = SourceBounds;
			VisualPlanInput.ProductLocalBounds = Product->InteractionLocalBounds;
			VisualPlanInput.UniformScaleRange = Products->UniformScaleRange;
			VisualPlanInput.AngularSpeedRange = Products->AngularSpeedRange;
			VisualPlanInput.StableSeed = VisualPlanSeed;
			VisualPlanInput.ProductCount = ProductCountRandom.RandRange(
				Products->MinimumProductCount, Products->MaximumProductCount);
			TArray<FMeteorDebrisVisualLane> ProductPlan;
			if (!BuildMeteorDebrisVisualPlan(VisualPlanInput, ProductPlan))
			{
				return EPrestageResult::Rejected;
			}

			FRandomStream Random(FoldStableSeed(
				Candidate.Target.WorldEntityId,
				Candidate.Target.SourceRevision,
				static_cast<uint64>(Launch.RandomSeed) ^ 0x4D455445u));
			// ProductClass 只提供原道具身份和资源；Lane 展开、速度与生命周期均由 Meteor 独立拥有。
			const int32 ProductCount = ProductPlan.Num();
			if (!Runtime->Scheduler.ReserveIncoming(ProductCount))
			{
				return EPrestageResult::RetryCapacity;
			}
			const uint32 BaseOrdinal = Runtime->NextOrdinal;
			const double NominalActivation = Runtime->Config.ComputeShockwaveArrivalTime(
				Runtime->ImpactTimeSeconds, Candidate.Distance);
			const double PreparationHorizon = Runtime->Config.NetworkLeadSeconds
				+ Runtime->Config.EncodingEstimateSeconds + Runtime->Config.QueueSafetySeconds;
			// NetworkLead 只定义页面最晚何时必须算完，不能改写 Gameplay 的绝对起飞时刻。
			// 页面晚到时客户端按该时刻直接快进，避免陨石消失后再空等一轮提前量。
			const double ActivationTime = NominalActivation;
			TArray<FMeteorDebrisSeed, TInlineAllocator<16>> Seeds;
			Seeds.Reserve(ProductCount);
			const float LayerPhase = Random.FRand();
			const float FanPhase = Random.FRand();
			for (int32 Index = 0; Index < ProductCount; ++Index)
			{
				const FMeteorDebrisVisualLane& ProductLane = ProductPlan[Index];
				FMeteorDebrisSeed& Seed = Seeds.AddDefaulted_GetRef();
				Seed.Key = {Runtime->BurstId, BaseOrdinal + static_cast<uint32>(Index)};
				Seed.WorldEntityId = Owner.GetWorld()->GetSubsystem<UWorldStorageSubsystem>()->AllocateEntityId();
				Seed.RenderArchetypeId = Product->DefinitionId;
				Seed.StartPosition = ProductLane.FlightWorldTransform.GetLocation();
				Seed.StartRotation = ProductLane.FlightWorldTransform.GetRotation();
				const FVector ProductScale = ProductLane.FlightWorldTransform.GetScale3D().GetAbs();
				Seed.Scale = FVector3f(ProductScale);
				Seed.ProductLocalBounds = FBox3f(
					FVector3f(Product->InteractionLocalBounds.Min),
					FVector3f(Product->InteractionLocalBounds.Max));
				Seed.VisualRadius = FMath::Max(1.0f, static_cast<float>(
					(Product->InteractionLocalBounds.GetExtent() * ProductScale).Size()));
				// 低矮源上的木块也必须从地面上方开始；VisualRadius 是任意旋转下的
				// 保守触地高度，不会让第一次精确触地求解因中心低于目标平面而失败。
				Seed.StartPosition.Z = FMath::Max<double>(
					Seed.StartPosition.Z,
					Runtime->Config.GroundPlaneZ + Seed.VisualRadius);
				const float EnvelopeStrength = FMath::Lerp(0.72f, 1.0f, Falloff);
				// 每个宿主内部使用均匀分层序列，而不是让 3-6 个产物各自独立抽签。
				// 这样任何一栋房子的碎片都会展开成宽扇面，不会整组趴地或整组冲天。
				const float LayerRoll = FMath::Fmod(
					(static_cast<float>(Index) + LayerPhase) / FMath::Max(1, ProductCount), 1.0f);
				const bool bGroundScatter = LayerRoll < Runtime->Config.DebrisGroundScatterFraction;
				const float AirRoll = bGroundScatter ? 0.0f
					: (LayerRoll - Runtime->Config.DebrisGroundScatterFraction)
					/ FMath::Max(UE_SMALL_NUMBER, 1.0f - Runtime->Config.DebrisGroundScatterFraction);
				const FVector2f* ElevationRange = &Runtime->Config.DebrisLowElevationDegrees;
				float ArcSpeedMultiplier = 1.0f;
				if (bGroundScatter)
				{
					ElevationRange = &Runtime->Config.DebrisGroundElevationDegrees;
					ArcSpeedMultiplier = Random.FRandRange(
						Runtime->Config.DebrisGroundSpeedMultiplier.X,
						Runtime->Config.DebrisGroundSpeedMultiplier.Y);
				}
				else if (AirRoll < Runtime->Config.DebrisHighArcFraction)
				{
					ElevationRange = &Runtime->Config.DebrisHighElevationDegrees;
					ArcSpeedMultiplier = 0.72f;
				}
				else if (AirRoll < Runtime->Config.DebrisHighArcFraction
					+ Runtime->Config.DebrisMediumArcFraction)
				{
					ElevationRange = &Runtime->Config.DebrisMediumElevationDegrees;
					ArcSpeedMultiplier = 0.86f;
				}
				const float ElevationDegrees = Random.FRandRange(ElevationRange->X, ElevationRange->Y);
				FVector Radial = Seed.StartPosition - Launch.ImpactCenter;
				Radial.Z = 0.0;
				if (Radial.SizeSquared2D() <= UE_KINDA_SMALL_NUMBER)
				{
					// 爆心正中的源没有可用径向；为该源稳定选一个基准方向，
					// 之后仍在外向半平面内展开各 Lane，而不是默认挤向世界 +X。
					const float FallbackRadians = Random.FRandRange(-PI, PI);
					Radial = FVector(FMath::Cos(FallbackRadians), FMath::Sin(FallbackRadians), 0.0);
				}
				const float MaximumAzimuthDeviation = bGroundScatter
					? Runtime->Config.DebrisGroundMaximumAzimuthDeviationDegrees
					: Runtime->Config.DebrisAirMaximumAzimuthDeviationDegrees;
				// 同一宿主的 Lane 均匀铺开整个外向扇面，再叠加稳定相位；
				// 避免随机碰巧都朝同一角度，形成可见的“散弹天幕”。
				const float FanAlpha = FMath::Fmod(
					(static_cast<float>(Index) + FanPhase) / FMath::Max(1, ProductCount), 1.0f);
				const float AzimuthDeviationDegrees = FMath::Lerp(
					-MaximumAzimuthDeviation, MaximumAzimuthDeviation, FanAlpha);
				// 三角分布把主体聚集在中速区，两端仍保留少量慢镜头与高速射流。
				const float SpeedAlpha = 0.5f * (Random.FRand() + Random.FRand());
				const float Speed = FMath::Lerp(
					Runtime->Config.DebrisSpeedRange.X,
					Runtime->Config.DebrisSpeedRange.Y, SpeedAlpha)
					* EnvelopeStrength * ArcSpeedMultiplier;
				FVector3f SolvedVelocity;
				if (!FMeteorBallisticKernel::BuildOutwardExplosionLaunchVelocity(
					FVector3f(Radial), AzimuthDeviationDegrees,
					ElevationDegrees, Speed, SolvedVelocity))
				{
					Runtime->Scheduler.CancelIncomingReservation(ProductCount);
					return EPrestageResult::Rejected;
				}
				Seed.InitialVelocity = FVector(SolvedVelocity);
				Seed.AngularVelocityDegrees = ProductLane.AngularVelocityDegrees;
				Seed.StartTimeSeconds = ActivationTime;
				Seed.ValidFromSeconds = ActivationTime;
				Seed.LatestComputeStartSeconds = ActivationTime - PreparationHorizon;
				if (!Seed.IsValid())
				{
					Runtime->Scheduler.CancelIncomingReservation(ProductCount);
					return EPrestageResult::Rejected;
				}
			}

			const int32 PreparedIndex = Runtime->PreparedTargets.AddDefaulted();
			FMeteorWorldRuntime::FPreparedTarget& Prepared = Runtime->PreparedTargets[PreparedIndex];
			Prepared.Target = Candidate.Target;
			Prepared.SourceBounds = SourceBounds;
			Prepared.Definition = Products;
			Prepared.Launch = Launch;
			Prepared.SettlementProductDefinitionId = Product->DefinitionId;
			Prepared.Distance = Candidate.Distance;
			Prepared.ActivationTimeSeconds = ActivationTime;
			Prepared.BaseOrdinal = BaseOrdinal;
			Prepared.ProductCount = ProductCount;
			Runtime->PreparedTargetById.Add(Candidate.Target.WorldEntityId, PreparedIndex);
			Runtime->OrdinalOwners.SetNum(BaseOrdinal + ProductCount, EAllowShrinking::No);
			Runtime->OrdinalPageLanes.SetNum(BaseOrdinal + ProductCount, EAllowShrinking::No);
			for (const FMeteorDebrisSeed& Seed : Seeds)
			{
				Runtime->OrdinalOwners[Seed.Key.DebrisOrdinal] = PreparedIndex;
				checkf(Runtime->Scheduler.EnqueueSeed(Seed),
					TEXT("Meteor Prestage 已预留完整 Inbox；入队不得失败。"));
			}
			Runtime->NextOrdinal += ProductCount;
			return EPrestageResult::Prepared;
		}

		static void PublishCancellation(
			UMeteorWorldSubsystem& Owner,
			const int32 PreparedTargetIndex)
		{
			FMeteorWorldRuntime* Runtime = Owner.Runtime.Get();
			if (!Runtime || !Runtime->PreparedTargets.IsValidIndex(PreparedTargetIndex)) return;
			FMeteorWorldRuntime::FPreparedTarget& Prepared = Runtime->PreparedTargets[PreparedTargetIndex];
			if (!Prepared.bCanceled || Prepared.bCancellationPublished || !Prepared.HasCompiledProducts()) return;
			TMap<uint64, FMeteorTrajectoryActivation> Cancellations;
			for (uint32 Ordinal = Prepared.BaseOrdinal;
				Ordinal < Prepared.BaseOrdinal + Prepared.ProductCount; ++Ordinal)
			{
				if (!Runtime->OrdinalPageLanes.IsValidIndex(Ordinal)) return;
				const FMeteorWorldRuntime::FPageLaneRef& Ref = Runtime->OrdinalPageLanes[Ordinal];
				if (!Ref.IsSet()) return;
				FMeteorTrajectoryActivation& Cancellation = Cancellations.FindOrAdd(Ref.PageId);
				Cancellation.BurstId = Runtime->BurstId;
				Cancellation.PageId = Ref.PageId;
				Cancellation.Revision = Ref.Revision;
				Cancellation.Ordinals.Add(Ordinal);
			}
			for (TPair<uint64, FMeteorTrajectoryActivation>& Pair : Cancellations)
			{
				Pair.Value.Ordinals.Sort();
				Runtime->TrajectoryCanceledEvent.Broadcast(Pair.Value);
			}
			Prepared.bCancellationPublished = true;
		}

		virtual bool Prepare(const FWorldDestructionProductBatch& Batch) override
		{
			FMeteorWorldRuntime* Runtime = Owner.Runtime.Get();
			if (!Runtime || !Runtime->bActive || !Batch.IsValid() || !Batch.LaunchContext
				|| !Runtime->PreparedTargets.IsValidIndex(PreparedTargetIndex))
			{
				UE_LOG(LogElementSandboxMeteor, Error,
					TEXT("Meteor Sink Prepare 基础契约失败：Runtime=%d Active=%d Batch=%d Launch=%d PreparedIndex=%d。"),
					Runtime != nullptr,
					Runtime && Runtime->bActive,
					Batch.IsValid(),
					Batch.LaunchContext != nullptr,
					PreparedTargetIndex);
				return false;
			}
			FMeteorWorldRuntime::FPreparedTarget& Prepared = Runtime->PreparedTargets[PreparedTargetIndex];
			bPrepared = Prepared.IsReady() && !Prepared.bActivated
				&& Batch.SourceId == Prepared.Target.WorldEntityId
				&& Batch.Target.WorldEntityId == Prepared.Target.WorldEntityId
				&& Batch.Definition == Prepared.Definition
				&& Batch.LaunchContext->EventId == Prepared.Launch.EventId;
			if (!bPrepared)
			{
				UE_LOG(LogElementSandboxMeteor, Error,
					TEXT("Meteor Sink Prepare 身份契约失败：Ready=%d Activated=%d Source=%d Target=%d Definition=%d Event=%d。"),
					Prepared.IsReady(),
					Prepared.bActivated,
					Batch.SourceId == Prepared.Target.WorldEntityId,
					Batch.Target.WorldEntityId == Prepared.Target.WorldEntityId,
					Batch.Definition == Prepared.Definition,
					Batch.LaunchContext->EventId == Prepared.Launch.EventId);
			}
			return bPrepared;
		}

		virtual void Commit() override
		{
			FMeteorWorldRuntime* Runtime = Owner.Runtime.Get();
			check(Runtime && bPrepared && Runtime->PreparedTargets.IsValidIndex(PreparedTargetIndex));
			FMeteorWorldRuntime::FPreparedTarget& Prepared = Runtime->PreparedTargets[PreparedTargetIndex];
			const double AuthorityStartTimeSeconds = FMath::Max(
				Prepared.ActivationTimeSeconds,
				Owner.GetWorld()->GetTimeSeconds());
			TMap<uint64, FMeteorTrajectoryActivation> Activations;
			for (uint32 Ordinal = Prepared.BaseOrdinal;
				Ordinal < Prepared.BaseOrdinal + Prepared.ProductCount; ++Ordinal)
			{
				check(Runtime->OrdinalPageLanes.IsValidIndex(Ordinal));
				const FMeteorWorldRuntime::FPageLaneRef& Ref = Runtime->OrdinalPageLanes[Ordinal];
					const FMeteorTrajectoryPage* Page = Runtime->ActivePages.Find(Ref.PageId);
					check(Page && Ref.IsSet() && Page->Revision == Ref.Revision && Page->Ordinals.IsValidIndex(Ref.Lane));
					FMeteorTrajectoryActivation& Activation = Activations.FindOrAdd(Ref.PageId);
					Activation.BurstId = Runtime->BurstId;
					Activation.PageId = Ref.PageId;
					Activation.Revision = Ref.Revision;
					Activation.SourceWorldEntityId = Prepared.Target.WorldEntityId;
					Activation.SourceTombstoneRevision = NextStateRevision(Prepared.Target.SourceRevision);
					Activation.AuthorityStartTimeSeconds = AuthorityStartTimeSeconds;
					Activation.Ordinals.Add(Ordinal);

					FMeteorSettlementLane Settlement;
					Settlement.Key = {Page->BurstId, Ordinal};
					Settlement.WorldEntityId = Page->WorldEntityIds[Ref.Lane];
					Settlement.ProductDefinitionId = Prepared.SettlementProductDefinitionId;
				Settlement.DueTimeSeconds = AuthorityStartTimeSeconds
					+ Page->ImpactDurations[Ref.Lane]
					+ Page->SettlingDurations[Ref.Lane];
					Settlement.WorldTransform = Page->GetRestTransform(Ref.Lane);
					checkf(Runtime->SettlementQueue.Enqueue(MoveTemp(Settlement)),
						TEXT("Activate 后每条解析木块必须且只能进入一次落地队列。"));
			}
			for (TPair<uint64, FMeteorTrajectoryActivation>& Pair : Activations)
			{
				Pair.Value.Ordinals.Sort();
				Runtime->PublishedActivationHistory.Add(Pair.Value);
				Runtime->TrajectoryActivatedEvent.Broadcast(Pair.Value);
			}
			Prepared.bActivated = true;
			if (Runtime->ActivatedLaneCount == 0)
			{
				Runtime->ImpactToFirstActivationMilliseconds = FMath::Max(
					0.0,
					(Owner.GetWorld()->GetTimeSeconds() - Runtime->ImpactTimeSeconds) * 1000.0);
			}
			Runtime->ActivatedLaneCount += Prepared.ProductCount;
			bPrepared = false;
		}

		virtual void Rollback() override
		{
			bPrepared = false;
		}

	private:
		UMeteorWorldSubsystem& Owner;
		int32 PreparedTargetIndex = INDEX_NONE;
		bool bPrepared = false;
	};
}

using namespace UE::ElementSandbox::Meteor;
using namespace UE::ElementSandbox::Destruction;

UMeteorWorldSubsystem::UMeteorWorldSubsystem() = default;
UMeteorWorldSubsystem::~UMeteorWorldSubsystem() = default;

void UMeteorWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UWorldStorageSubsystem>();
	Collection.InitializeDependency<UBuildingWorldSubsystem>();
	Collection.InitializeDependency<UWorldObjectWorldSubsystem>();
	Runtime = MakePimpl<FMeteorWorldRuntime>();
	Runtime->Config = GetDefault<UMeteorStrikeRuleSet>()->Freeze();
	checkf(Runtime->Config.IsValid(), TEXT("默认 Meteor RuleSet 必须可冻结为有效 POD。"));
	if (UBuildingWorldSubsystem* Buildings = GetWorld()->GetSubsystem<UBuildingWorldSubsystem>())
	{
		Runtime->BuildingSnapshotHandle = Buildings->OnQuerySnapshotBatchCommitted().AddLambda(
			[this](const FBuildQuerySnapshotBatchRef Batch)
			{
				if (!Runtime || !Runtime->bActive) return;
				UBuildingWorldSubsystem* Buildings = GetWorld()->GetSubsystem<UBuildingWorldSubsystem>();
				if (!Buildings) return;
				for (const FBuildQuerySnapshotChange& Change : Batch->Changes)
				{
					const FBuildEntityHandle Entity = Change.Entity;
					const int32 Slot = Entity.GetIndex();
					if (Slot < 0) continue;
					if (!Change.Current.IsSet())
					{
						// Visited 只属于当前 Generation。删除后释放 Slot，否则同一 Slot
						// 在波前到达前被新实体复用时会被旧代错误屏蔽。
						if (Runtime->VisitedBuildings.IsValidIndex(Slot)) Runtime->VisitedBuildings[Slot] = false;
						continue;
					}
					if (Runtime->VisitedBuildings.Num() <= Slot) Runtime->VisitedBuildings.SetNum(Slot + 1, false);
					if (Runtime->VisitedBuildings[Slot]) continue;
					const FBuildEntityRegistry& Registry = Buildings->GetRegistry();
					const FBuildDefinitionFragment* DefFragment = Registry.FindFragment<FBuildDefinitionFragment>(Entity);
					const FBuildWorldIdentityFragment* Identity = Registry.FindFragment<FBuildWorldIdentityFragment>(Entity);
					const UBuildingDefinition* Definition = DefFragment ? DefFragment->Definition.Get() : nullptr;
					if (!Definition || !Definition->Destruction.IsEnabled() || !Identity) continue;
					const double Distance = DistanceToBox2D(Runtime->ImpactCenter, Change.Current->WorldBounds);
					if (Distance < Runtime->PublishedWaveRadius || Distance > Runtime->Config.ShockwaveRadius) continue;
					Runtime->VisitedBuildings[Slot] = true;
					FWorldDestructionTarget Target;
					Target.Domain = EWorldDestructionTargetDomain::Building;
					Target.Building = Entity;
					Target.WorldEntityId = Identity->WorldEntityId;
					Target.SourceRevision = Identity->StateRevision;
					if (Distance <= Runtime->Config.ImpactCoreRadius) ++Runtime->CoreCandidateCount;
					Runtime->HeapPush({Target, Distance});
					Runtime->PreparationHeapPush({Target, Distance});
				}
			});
	}
	if (UWorldObjectWorldSubsystem* WorldObjects = GetWorld()->GetSubsystem<UWorldObjectWorldSubsystem>())
	{
		Runtime->WorldObjectSnapshotHandle = WorldObjects->OnQuerySnapshotBatchCommitted().AddLambda(
			[this](const FWorldObjectQuerySnapshotBatch& Batch)
			{
				if (!Runtime || !Runtime->bActive) return;
				UWorldObjectWorldSubsystem* WorldObjects = GetWorld()->GetSubsystem<UWorldObjectWorldSubsystem>();
				if (!WorldObjects) return;
				for (const FWorldObjectQuerySnapshotChange& Change : Batch.Changes)
				{
					const FWorldObjectEntityHandle Entity = Change.Entity;
					const int32 Slot = Entity.GetSlot();
					if (Slot < 0) continue;
					if (!Change.Current.IsSet())
					{
						if (Runtime->VisitedWorldObjects.IsValidIndex(Slot)) Runtime->VisitedWorldObjects[Slot] = false;
						continue;
					}
					if (Runtime->VisitedWorldObjects.Num() <= Slot) Runtime->VisitedWorldObjects.SetNum(Slot + 1, false);
					if (Runtime->VisitedWorldObjects[Slot]) continue;
					const FWorldObjectEntityRegistry& Registry = WorldObjects->GetRegistry();
					const FWorldObjectDefinitionFragment* DefFragment = Registry.FindFragment<FWorldObjectDefinitionFragment>(Entity);
					const FWorldObjectWorldIdentityFragment* Identity = Registry.FindFragment<FWorldObjectWorldIdentityFragment>(Entity);
					const UWorldObjectDefinition* Definition = DefFragment ? DefFragment->Definition.Get() : nullptr;
					if (!Definition || !Definition->Destruction.IsEnabled() || !Identity) continue;
					const double Distance = DistanceToBox2D(Runtime->ImpactCenter, Change.Current->WorldBounds);
					if (Distance < Runtime->PublishedWaveRadius || Distance > Runtime->Config.ShockwaveRadius) continue;
					Runtime->VisitedWorldObjects[Slot] = true;
					FWorldDestructionTarget Target;
					Target.Domain = EWorldDestructionTargetDomain::WorldObject;
					Target.WorldObject = Entity;
					Target.WorldEntityId = Identity->WorldEntityId;
					Target.SourceRevision = Identity->StateRevision;
					if (Distance <= Runtime->Config.ImpactCoreRadius) ++Runtime->CoreCandidateCount;
					Runtime->HeapPush({Target, Distance});
					Runtime->PreparationHeapPush({Target, Distance});
				}
			});
	}
}

void UMeteorWorldSubsystem::Deinitialize()
{
	if (Runtime)
	{
		if (UBuildingWorldSubsystem* Buildings = GetWorld()->GetSubsystem<UBuildingWorldSubsystem>();
			Buildings && Buildings->HasRuntimeState())
		{
			Buildings->OnQuerySnapshotBatchCommitted().Remove(Runtime->BuildingSnapshotHandle);
		}
		if (UWorldObjectWorldSubsystem* WorldObjects = GetWorld()->GetSubsystem<UWorldObjectWorldSubsystem>();
			WorldObjects && WorldObjects->HasRuntimeState())
		{
			WorldObjects->OnQuerySnapshotBatchCommitted().Remove(Runtime->WorldObjectSnapshotHandle);
		}
			if (UWorldStorageSubsystem* Storage = GetWorld()->GetSubsystem<UWorldStorageSubsystem>())
			{
				if (Runtime->MutationBatch.IsSet() && Runtime->NextOrdinal == 0)
			{
				Storage->CancelEmptyDelayedMutationBatch(Runtime->MutationBatch);
			}
		}
	}
	Runtime.Reset();
	Super::Deinitialize();
}

void UMeteorWorldSubsystem::PumpSettlements(const double NowSeconds, const double DeadlineSeconds)
{
	if (Runtime->SettlementQueue.IsEmpty()) return;
	UWorldObjectWorldSubsystem* WorldObjects = GetWorld()->GetSubsystem<UWorldObjectWorldSubsystem>();
	UWorldStorageSubsystem* WorldStorage = GetWorld()->GetSubsystem<UWorldStorageSubsystem>();
	if (!WorldObjects || !WorldStorage) return;

	TArray<FVector, TInlineAllocator<4>> AuthorityPlayerLocations;
	for (FConstPlayerControllerIterator Iterator = GetWorld()->GetPlayerControllerIterator(); Iterator; ++Iterator)
	{
		const APlayerController* Controller = Iterator->Get();
		const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
		if (Pawn && !Pawn->GetActorLocation().ContainsNaN())
		{
			AuthorityPlayerLocations.Add(Pawn->GetActorLocation());
		}
	}

	TArray<FMeteorSettlementReservation> ReservedSettlements;
	TArray<FMeteorSettlementReservation> DueSettlements;
	TArray<FWorldObjectCreateDesc, TInlineAllocator<128>> SettlementDescs;
	TArray<FMeteorSettlementMapping, TInlineAllocator<128>> PublishedSettlements;
	while (FPlatformTime::Seconds() < DeadlineSeconds && !Runtime->SettlementQueue.IsEmpty())
	{
		Runtime->SettlementQueue.ReserveDue(AuthorityPlayerLocations, NowSeconds,
			Runtime->Config.MaximumSettlementBatchSize,
			Runtime->Config.MinimumGlobalSettlementCountPerBatch, ReservedSettlements);
		if (ReservedSettlements.IsEmpty()) break;

		DueSettlements.Reset();
		SettlementDescs.Reset();
		PublishedSettlements.Reset();
		for (const FMeteorSettlementReservation& Reservation : ReservedSettlements)
		{
			const FMeteorSettlementLane* Lane = Runtime->SettlementQueue.FindReserved(Reservation);
			check(Lane);
			UWorldObjectDefinition* Definition = WorldObjects->FindDefinition(Lane->ProductDefinitionId);
			if (!Definition)
			{
				verify(Runtime->SettlementQueue.RollbackReserved(Reservation));
				continue;
			}
			FWorldObjectCreateDesc& Desc = SettlementDescs.AddDefaulted_GetRef();
			Desc.Definition = Definition;
			Desc.ReservedWorldEntityId = Lane->WorldEntityId;
			// 沿用解析终态与最终身份，近场碰撞直接接管同一姿态。
			Desc.WorldTransform = Lane->WorldTransform;
			Desc.MotionState = EWorldObjectMotionState::Dormant;
			Desc.InstanceInteractionBounds = Definition->InteractionLocalBounds;
			DueSettlements.Add(Reservation);
		}
		if (DueSettlements.IsEmpty()) break;

		FWorldObjectStagedCreateBatch StagedBatch;
		TArray<FWorldObjectEntityHandle> CreatedEntities;
		const bool bCreated = WorldStorage->ExecuteInDelayedMutationBatch(Runtime->MutationBatch,
			[WorldObjects, &SettlementDescs, &StagedBatch, &CreatedEntities]()
			{
				if (!WorldObjects->StageCreateEntities(SettlementDescs, StagedBatch)) return false;
				if (!WorldObjects->CommitStagedCreateEntities(StagedBatch, CreatedEntities))
				{
					WorldObjects->RollbackStagedCreateEntities(StagedBatch);
					return false;
				}
				return true;
			});
		if (!bCreated || CreatedEntities.Num() != DueSettlements.Num())
		{
			for (const FMeteorSettlementReservation& Reservation : DueSettlements)
			{
				verify(Runtime->SettlementQueue.RollbackReserved(Reservation));
			}
			// 本帧无法提交时留待下一帧，不在时间片里反复重试同一批事务。
			break;
		}
		for (int32 Index = 0; Index < CreatedEntities.Num(); ++Index)
		{
			const FMeteorSettlementReservation& Reservation = DueSettlements[Index];
			const FMeteorSettlementLane* Lane = Runtime->SettlementQueue.FindReserved(Reservation);
			check(Lane);
			PublishedSettlements.Add({Lane->Key, WorldObjects->GetWorldEntityId(CreatedEntities[Index])});
			checkf(Runtime->SettlementQueue.CommitReserved(Reservation),
				TEXT("成功创建 WorldObject 后必须提交对应 Meteor Settlement Reservation。"));
			if (Reservation.Source == EMeteorSettlementReservationSource::ProximityCell)
			{
				++Runtime->ProximitySettledLaneCount;
			}
			else
			{
				++Runtime->GlobalOldestSettledLaneCount;
			}
		}
		Runtime->PublishedSettlementHistory.Append(PublishedSettlements);
		Runtime->SettlementPublishedEvent.Broadcast(PublishedSettlements);
	}
}

void UMeteorWorldSubsystem::Tick(const float DeltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_MeteorServerPump);
	if (!Runtime || !Runtime->bActive || GetWorld()->GetNetMode() == NM_Client) return;
	const double PumpStartSeconds = FPlatformTime::Seconds();
	const bool bDedicatedServer = GetWorld()->GetNetMode() == NM_DedicatedServer;
	const double GameplayBudgetSeconds = 0.001 * (bDedicatedServer
		? Runtime->Config.DedicatedServerGameplayBudgetMilliseconds
		: Runtime->Config.LocalServerGameplayBudgetMilliseconds);
	const int32 WorkerConcurrency = bDedicatedServer
		? Runtime->Config.DedicatedServerWorkerConcurrency : Runtime->Config.LocalServerWorkerConcurrency;
	const double GameplayDeadlineSeconds = PumpStartSeconds + GameplayBudgetSeconds;
	const auto HasGameplayBudget = [&]()
	{
		return FPlatformTime::Seconds() < GameplayDeadlineSeconds;
	};
	const double Now = GetWorld()->GetTimeSeconds();
	UBuildingWorldSubsystem* Buildings = GetWorld()->GetSubsystem<UBuildingWorldSubsystem>();
	UWorldObjectWorldSubsystem* WorldObjects = GetWorld()->GetSubsystem<UWorldObjectWorldSubsystem>();
	UWorldStorageSubsystem* WorldStorage = GetWorld()->GetSubsystem<UWorldStorageSubsystem>();
	if (!Buildings || !WorldObjects || !WorldStorage) return;
	// Delayed Mutation Batch 在打开时会先异步封口 Burst 之前已有的 Dirty 状态。
	// Meteor 必须主动推进并观察这道事务屏障；Ready 前只继续查询和预演，不能让
	// ExecuteInDelayedMutationBatch 永久被拒绝。该检查不申请或改变任何 Chunk Residency。
	const bool bMutationBatchReady = Runtime->MutationBatch.IsSet()
		&& WorldStorage->IsDelayedMutationBatchReady(Runtime->MutationBatch);
	// 保留到期落地的先行份额，防止持续破坏使已经落地的近场产物无法进入普通 ECS。
	if (bMutationBatchReady)
	{
		PumpSettlements(Now, PumpStartSeconds + FMath::Min(0.001, GameplayBudgetSeconds * 0.2));
	}
	// 波前一旦因预流送、Worker 或存储预算停下，后续只能按正常波速继续向外推进，
	// 不能用绝对 ImpactTime 瞬间追平理论半径。否则一次短暂积压会在解除后的
	// 单帧内同时提交多个远近目标，既破坏冲击波表现，也重新制造 CPU 峰值。
	const double DesiredRadius = Now < Runtime->ImpactTimeSeconds ? 0.0
		: FMath::Min<double>(Runtime->Config.ShockwaveRadius,
			FMath::Max<double>(
				Runtime->Config.ImpactCoreRadius,
				Runtime->PublishedWaveRadius
				+ Runtime->Config.ShockwaveSpeed * FMath::Max(0.0f, DeltaTime)));
	double SafeRadius = DesiredRadius;
	if (Runtime->NextResidentTileIndex < Runtime->Tiles.Num())
	{
		SafeRadius = FMath::Min(
			SafeRadius,
			Runtime->Tiles[Runtime->NextResidentTileIndex].MinimumDistance);
	}

	const bool bFirstImpactPump = Now >= Runtime->ImpactTimeSeconds && !Runtime->bImpactPumpRecorded;
	const uint32 ActivatedBeforePump = Runtime->ActivatedLaneCount;
	int32 DestroyedThisFrame = 0;
	int32 ExaminedThisFrame = 0;
	// 已到期源先于新的预演/查询；软预算分段留出后续进度，不能等预演堆清空才销毁。
	// 首次有界尝试不受计时开销阻止。没有新预演/查询待办时，破坏可使用全部剩余预算。
	const bool bPreparationPending = !Runtime->PreparationHeap.IsEmpty()
		|| Runtime->NextResidentTileIndex < Runtime->Tiles.Num();
	const double DestructionDeadlineSeconds = PumpStartSeconds
		+ GameplayBudgetSeconds * (bPreparationPending ? 0.75 : 1.0);
	const auto HasDestructionBudget = [&]()
	{
		return ExaminedThisFrame == 0 || FPlatformTime::Seconds() < DestructionDeadlineSeconds;
	};
		TArray<FMeteorWorldRuntime::FCandidate, TInlineAllocator<64>> DeferredDueCandidates;
		const int32 MaximumExaminedThisFrame = FMath::Max(
			64, Runtime->Config.MaximumDestructionTargetsPerPump * 4);
		// Query Snapshot 的 Commit 会同步广播给表现和 Element Host Bridge，必须把它也纳入
		// 预算片。旧实现虽然逐目标检查墙钟，却在循环结束后一次广播 1024 个销毁，
		// 因而把数百毫秒工作藏到了预算检查之外。
		const bool bCanProcessDestruction = Now >= Runtime->ImpactTimeSeconds
			&& bMutationBatchReady && !Runtime->CandidateHeap.IsEmpty();
		while (bCanProcessDestruction
			&& DestroyedThisFrame < Runtime->Config.MaximumDestructionTargetsPerPump
			&& ExaminedThisFrame < MaximumExaminedThisFrame
			&& HasDestructionBudget()
			&& !Runtime->CandidateHeap.IsEmpty())
		{
			if (Runtime->CandidateHeap[0].Distance > SafeRadius) break;
			const bool bBuildingSnapshotBatch = Buildings->BeginGameplayDestructionBatch();
			const bool bWorldObjectSnapshotBatch = bBuildingSnapshotBatch
				&& WorldObjects->BeginGameplayDestructionBatch();
			if (!bBuildingSnapshotBatch || !bWorldObjectSnapshotBatch)
			{
				if (bBuildingSnapshotBatch) Buildings->EndGameplayDestructionBatch(false);
				if (bWorldObjectSnapshotBatch) WorldObjects->EndGameplayDestructionBatch(false);
				UE_LOG(LogElementSandboxMeteor, Error,
					TEXT("Meteor 无法开启宿主批量 Query Snapshot；本帧停止销毁，避免退化为逐目标广播。"));
				break;
			}
			const int32 DestroyedAtBatchStart = DestroyedThisFrame;
			const int32 ExaminedAtBatchStart = ExaminedThisFrame;
			while (DestroyedThisFrame - DestroyedAtBatchStart
					< Runtime->Config.MaximumDestructionTargetsPerSnapshotBatch
				&& DestroyedThisFrame < Runtime->Config.MaximumDestructionTargetsPerPump
				&& ExaminedThisFrame < MaximumExaminedThisFrame
				&& HasDestructionBudget()
				&& !Runtime->CandidateHeap.IsEmpty())
			{
			const FMeteorWorldRuntime::FCandidate Next = Runtime->CandidateHeap[0];
			if (Next.Distance > SafeRadius) break;
			FMeteorWorldRuntime::FCandidate Candidate;
			Runtime->HeapPop(Candidate);
			++ExaminedThisFrame;

		// 候选进入 Heap 后宿主仍可能移动或被别的玩法修改。正式破坏前重新读取宿主真值；
		// moved-behind-wave 的对象不追溯破坏，仍在波前前方的对象按新距离重新排队。
		bool bCandidateStillValid = false;
		FBox CurrentBounds(ForceInit);
		if (Candidate.Target.Domain == EWorldDestructionTargetDomain::Building)
		{
			const FBuildEntityRegistry& Registry = Buildings->GetRegistry();
			const FBuildWorldIdentityFragment* Identity =
				Registry.FindFragment<FBuildWorldIdentityFragment>(Candidate.Target.Building);
			const FBuildDefinitionFragment* DefinitionFragment =
				Registry.FindFragment<FBuildDefinitionFragment>(Candidate.Target.Building);
			const UBuildingDefinition* Definition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
			bCandidateStillValid = Identity && Definition && Definition->Destruction.IsEnabled()
				&& Identity->WorldEntityId == Candidate.Target.WorldEntityId
				&& Buildings->GetSpatialIndex().TryGetBounds(Candidate.Target.Building, CurrentBounds);
			if (bCandidateStillValid)
			{
				Candidate.Target.SourceRevision = Identity->StateRevision;
			}
		}
		else
		{
			const FWorldObjectEntityRegistry& Registry = WorldObjects->GetRegistry();
			const FWorldObjectWorldIdentityFragment* Identity =
				Registry.FindFragment<FWorldObjectWorldIdentityFragment>(Candidate.Target.WorldObject);
			const FWorldObjectDefinitionFragment* DefinitionFragment =
				Registry.FindFragment<FWorldObjectDefinitionFragment>(Candidate.Target.WorldObject);
			const UWorldObjectDefinition* Definition = DefinitionFragment ? DefinitionFragment->Definition.Get() : nullptr;
			bCandidateStillValid = Identity && Definition && Definition->Destruction.IsEnabled()
				&& Identity->WorldEntityId == Candidate.Target.WorldEntityId
				&& WorldObjects->GetSpatialIndex().TryGetBounds(Candidate.Target.WorldObject, CurrentBounds);
			if (bCandidateStillValid)
			{
				Candidate.Target.SourceRevision = Identity->StateRevision;
			}
		}
		if (!bCandidateStillValid)
		{
			if (const int32* PreparedIndex = Runtime->PreparedTargetById.Find(Candidate.Target.WorldEntityId);
				PreparedIndex && Runtime->PreparedTargets.IsValidIndex(*PreparedIndex))
			{
				const int32 CanceledIndex = *PreparedIndex;
				Runtime->PreparedTargets[CanceledIndex].bCanceled = true;
				Runtime->PreparedTargetById.Remove(Candidate.Target.WorldEntityId);
				FMeteorProductSink::PublishCancellation(*this, CanceledIndex);
			}
			continue;
		}
		Candidate.Distance = DistanceToBox2D(Runtime->ImpactCenter, CurrentBounds);
		if (Candidate.Distance > Runtime->Config.ShockwaveRadius)
		{
			continue;
		}
		if (Candidate.Distance > SafeRadius)
		{
			Runtime->HeapPush(Candidate);
			continue;
		}

		const int32* PreparedIndexPtr = Runtime->PreparedTargetById.Find(Candidate.Target.WorldEntityId);
		if (!PreparedIndexPtr || !Runtime->PreparedTargets.IsValidIndex(*PreparedIndexPtr))
		{
			if (Runtime->RejectedPreparationTargets.Contains(Candidate.Target.WorldEntityId))
			{
				continue;
			}
				// 未预演候选已经由发现路径排队；不能在每次到期重试时重复插入。
				DeferredDueCandidates.Add(Candidate);
				continue;
		}
		const int32 PreparedIndex = *PreparedIndexPtr;
		FMeteorWorldRuntime::FPreparedTarget& Prepared = Runtime->PreparedTargets[PreparedIndex];
		const bool bSameBounds = Prepared.SourceBounds.Min.Equals(CurrentBounds.Min, 0.1)
			&& Prepared.SourceBounds.Max.Equals(CurrentBounds.Max, 0.1);
		if (Prepared.Target.SourceRevision != Candidate.Target.SourceRevision || !bSameBounds)
		{
			// Upsert 发生在预流送之后：旧 Lane 永不 Activate，并用新 Revision/位置重新预演。
			Prepared.bCanceled = true;
			Runtime->PreparedTargetById.Remove(Candidate.Target.WorldEntityId);
			Runtime->RejectedPreparationTargets.Remove(Candidate.Target.WorldEntityId);
				FMeteorProductSink::PublishCancellation(*this, PreparedIndex);
				Runtime->PreparationHeapPush(Candidate);
				DeferredDueCandidates.Add(Candidate);
				continue;
			}
			if (!Prepared.IsReady() || Now < Prepared.ActivationTimeSeconds)
			{
				DeferredDueCandidates.Add(Candidate);
				continue;
		}

		FMeteorProductSink Sink(*this, PreparedIndex);
		FWorldDestructionRequest Request;
		Request.Target = Candidate.Target;
		Request.DamageMode = EWorldDestructionDamageMode::ExhaustDurability;
		Request.ProductSink = &Sink;
		Request.LaunchContext = Prepared.Launch;
		const bool bDestroyed = WorldStorage->ExecuteInDelayedMutationBatch(
			Runtime->MutationBatch,
			[this, &Request]()
			{
				return FWorldDestructionAuthorityService::TryApplyRequest(*GetWorld(), Request);
			});
		if (!bDestroyed)
		{
				// 一个源暂时无法提交时只延期它自己。不得用单个目标把整个
				// 冲击波半径卡住，否则密集区会退化成建筑逐个消失。
				DeferredDueCandidates.Add(Candidate);
				continue;
			}
				++DestroyedThisFrame;
			}
			const bool bBuildingCommitted = Buildings->EndGameplayDestructionBatch();
			const bool bWorldObjectCommitted = WorldObjects->EndGameplayDestructionBatch();
			if (!bBuildingCommitted || !bWorldObjectCommitted)
			{
				UE_LOG(LogElementSandboxMeteor, Error,
					TEXT("Meteor 宿主批量 Query Snapshot 提交失败：Building=%d WorldObject=%d。"),
					bBuildingCommitted, bWorldObjectCommitted);
				break;
			}
			// 即使本批只有失效或待预演候选，也必须确认确实推进过 Heap；否则避免空转。
			if (DestroyedThisFrame == DestroyedAtBatchStart
				&& ExaminedThisFrame == ExaminedAtBatchStart)
			{
				break;
			}
		}
		for (const FMeteorWorldRuntime::FCandidate& Candidate : DeferredDueCandidates)
		{
			Runtime->HeapPush(Candidate);
	}
	if (bFirstImpactPump)
	{
		Runtime->bImpactPumpRecorded = true;
		Runtime->ImpactFrameDestroyedTargets = DestroyedThisFrame;
	}
	const uint32 ActivatedThisPump = Runtime->ActivatedLaneCount - ActivatedBeforePump;
	if (Runtime->FirstActivationLaneCount == 0 && ActivatedThisPump > 0)
	{
		Runtime->FirstActivationLaneCount = static_cast<int32>(ActivatedThisPump);
	}
	Runtime->PublishedWaveRadius = FMath::Max(Runtime->PublishedWaveRadius, SafeRadius);

	// 只预演客户端提前窗口内的候选；为尚未查询的 Resident Tile 留出尾段预算。
	const double PreparationDeadlineSeconds = PumpStartSeconds + GameplayBudgetSeconds * 0.9;
	const double PreparationHorizon = Runtime->Config.NetworkLeadSeconds
		+ Runtime->Config.EncodingEstimateSeconds + Runtime->Config.QueueSafetySeconds;
	const double PreparationLeadSeconds = Now + PreparationHorizon - Runtime->ImpactTimeSeconds;
	const double PreparationRadius = Now < Runtime->ImpactTimeSeconds
		? (PreparationLeadSeconds < 0.0 ? 0.0 : Runtime->Config.ComputeShockwaveRadius(PreparationLeadSeconds))
		: FMath::Min<double>(Runtime->Config.ShockwaveRadius,
			FMath::Max<double>(Runtime->PublishedWaveRadius, Runtime->Config.ImpactCoreRadius)
			+ PreparationHorizon * Runtime->Config.ShockwaveSpeed);
	for (int32 PreparedThisPump = 0;
		PreparedThisPump < Runtime->Config.MaximumDestructionTargetsPerPump
		&& (PreparedThisPump == 0 || FPlatformTime::Seconds() < PreparationDeadlineSeconds)
		&& !Runtime->PreparationHeap.IsEmpty(); ++PreparedThisPump)
	{
		if (Runtime->PreparationHeap[0].Distance > PreparationRadius) break;
		FMeteorWorldRuntime::FCandidate Candidate;
		Runtime->PreparationHeapPop(Candidate);
		if (Runtime->PreparedTargetById.Contains(Candidate.Target.WorldEntityId)) continue;
		const FMeteorProductSink::EPrestageResult Result = FMeteorProductSink::Prestage(*this, Candidate, Now);
		if (Result == FMeteorProductSink::EPrestageResult::RetryCapacity)
		{
			Runtime->PreparationHeapPush(Candidate);
			break;
		}
		if (Result == FMeteorProductSink::EPrestageResult::Rejected)
		{
			Runtime->RejectedPreparationTargets.Add(Candidate.Target.WorldEntityId);
		}
	}
	// 只读取已 Resident 的 ECS 索引；每帧至少尝试一个 Tile，防止持续预演使查询饥饿。
	for (int32 QueryBudget = 0;
		QueryBudget < Runtime->Config.MaximumQueryTilesPerPump
		&& (QueryBudget == 0 || HasGameplayBudget()) && Runtime->NextResidentTileIndex < Runtime->Tiles.Num();
		++QueryBudget)
	{
		DiscoverTileCandidates(*Runtime, Runtime->Tiles[Runtime->NextResidentTileIndex++].Bounds,
			*Buildings, *WorldObjects);
	}

	Runtime->Scheduler.Pump(Now);
	while (Runtime->WorkerInFlight.Load() < WorkerConcurrency)
	{
		FMeteorPageHandle Handle;
		FMeteorWorkPage WorkPage;
		if (!Runtime->Scheduler.TryAcquireWork(Handle, WorkPage)) break;
		const uint64 PageId = Runtime->Scheduler.GetNextTrajectoryPageId();
		const FMeteorRuntimeConfig Config = Runtime->Config;
		const FMeteorBurstId BurstId = Runtime->BurstId;
		++Runtime->WorkerInFlight;
		Runtime->CompileFutures.Add(Async(EAsyncExecution::ThreadPool,
			[Runtime = Runtime.Get(), Handle, WorkPage = MoveTemp(WorkPage), Config, BurstId, PageId]() mutable
			{
				TUniquePtr<FMeteorWorldRuntime::FCompileResult> Result = MakeUnique<FMeteorWorldRuntime::FCompileResult>();
				Result->Handle = Handle;
				Result->bSucceeded = FMeteorBallisticKernel::CompilePage(
					WorkPage, Config, BurstId, PageId, Result->Page);
				Runtime->CompileResults.Enqueue(MoveTemp(Result));
				--Runtime->WorkerInFlight;
			}));
	}
	Runtime->CompileFutures.RemoveAllSwap(
		[](const TFuture<void>& Future) { return Future.IsReady(); }, EAllowShrinking::No);
	TUniquePtr<FMeteorWorldRuntime::FCompileResult> Result;
	while (Runtime->CompileResults.Dequeue(Result))
	{
		if (Result->bSucceeded)
		{
			if (!Runtime->Scheduler.CompleteWork(Result->Handle, MoveTemp(Result->Page)))
			{
				UE_LOG(LogElementSandboxMeteor, Error,
					TEXT("Burst=%llu Worker 结果无法提交：PageSlot=%u Generation=%u；尝试把原页重新提急。"),
					Runtime->BurstId.Value,
					Result->Handle.Slot,
					Result->Handle.Generation);
				if (!Runtime->Scheduler.FailWork(Result->Handle))
				{
					UE_LOG(LogElementSandboxMeteor, Error,
						TEXT("Burst=%llu PageSlot=%u Generation=%u 已不属于 Computing，拒绝静默泄漏。"),
						Runtime->BurstId.Value,
						Result->Handle.Slot,
						Result->Handle.Generation);
				}
			}
		}
		else
		{
			UE_LOG(LogElementSandboxMeteor, Error,
				TEXT("Burst=%llu 解析轨迹页编译失败：PageSlot=%u Generation=%u。"),
				Runtime->BurstId.Value,
				Result->Handle.Slot,
				Result->Handle.Generation);
			Runtime->Scheduler.FailWork(Result->Handle);
		}
	}
	FMeteorPageHandle CompletedHandle;
	FMeteorTrajectoryPage CompletedPage;
	while (Runtime->Scheduler.ConsumeCompleted(CompletedHandle, CompletedPage))
	{
		Runtime->ActivePages.Add(CompletedPage.PageId, CompletedPage);
		// Payload 先发布；这里只建立 Ordinal→Page/Lane 目录，不创建 Settlement，
		// 也不让客户端开始显示。后续源 GameplayDestroy 成功才发轻量 Activate。
		TSet<int32> TouchedPreparedTargets;
		for (int32 Lane = 0; Lane < CompletedPage.Num(); ++Lane)
		{
			const uint32 Ordinal = CompletedPage.Ordinals[Lane];
			if (!Runtime->OrdinalOwners.IsValidIndex(Ordinal)
				|| !Runtime->OrdinalPageLanes.IsValidIndex(Ordinal)) continue;
			const int32 PreparedIndex = Runtime->OrdinalOwners[Ordinal];
			if (!Runtime->PreparedTargets.IsValidIndex(PreparedIndex)) continue;
			TouchedPreparedTargets.Add(PreparedIndex);
			Runtime->OrdinalPageLanes[Ordinal] = {
				CompletedPage.PageId, CompletedPage.Revision, static_cast<uint16>(Lane)};
			++Runtime->PreparedTargets[PreparedIndex].CompiledLaneCount;
		}
		Runtime->PagePreparedEvent.Broadcast(CompletedPage);
		for (const int32 PreparedIndex : TouchedPreparedTargets)
		{
			FMeteorProductSink::PublishCancellation(*this, PreparedIndex);
		}
		Runtime->Scheduler.ReleaseCompleted(CompletedHandle);
	}

	if (bMutationBatchReady)
	{
		PumpSettlements(Now, GameplayDeadlineSeconds);
	}

	const bool bWaveFinished = Runtime->PublishedWaveRadius >= Runtime->Config.ShockwaveRadius
		&& Runtime->NextResidentTileIndex >= Runtime->Tiles.Num() && Runtime->CandidateHeap.IsEmpty();
	const FMeteorSchedulerStats SchedulerStats = Runtime->Scheduler.GetStats(Now);
	if (bWaveFinished && Runtime->SettlementQueue.IsEmpty() && Runtime->WorkerInFlight.Load() == 0
		&& SchedulerStats.AllocatedPages == 0 && Runtime->Scheduler.GetPendingSeedCount() == 0)
	{
		if (WorldStorage->CommitDelayedMutationBatch(Runtime->MutationBatch))
		{
			UE_LOG(LogElementSandboxMeteor, Display,
				TEXT("Burst=%llu 已完成：激活碎片=%u，全部落地事务已提交。"),
				Runtime->BurstId.Value, Runtime->ActivatedLaneCount);
			Runtime->MutationBatch = {};
			Runtime->bActive = false;
			Runtime->SettlementQueue.Reset();
			Runtime->ActivePages.Reset();
			Runtime->PublishedActivationHistory.Reset();
			Runtime->PublishedSettlementHistory.Reset();
		}
	}
	Runtime->LastPumpMilliseconds = (FPlatformTime::Seconds() - PumpStartSeconds) * 1000.0;
	SET_DWORD_STAT(STAT_MeteorQueryTiles, Runtime->NextResidentTileIndex);
	SET_DWORD_STAT(STAT_MeteorPendingTargets, Runtime->CandidateHeap.Num());
	SET_DWORD_STAT(STAT_MeteorActivatedDebris, Runtime->ActivatedLaneCount);
	SET_DWORD_STAT(STAT_MeteorSettlementBacklog, Runtime->SettlementQueue.Num());
	CSV_CUSTOM_STAT(Meteor, WaveRadius, Runtime->PublishedWaveRadius, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Meteor, PendingTargets, Runtime->CandidateHeap.Num(), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Meteor, CoreCandidates, Runtime->CoreCandidateCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Meteor, ImpactFrameTargets, Runtime->ImpactFrameDestroyedTargets, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Meteor, FirstActivationLanes, Runtime->FirstActivationLaneCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Meteor, TotalActivatedLanes,
		static_cast<double>(Runtime->ActivatedLaneCount), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Meteor, ImpactToFirstActivationMs,
		Runtime->ImpactToFirstActivationMilliseconds, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Meteor, WorkerInFlight, Runtime->WorkerInFlight.Load(), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Meteor, GameplayBudgetMilliseconds, GameplayBudgetSeconds * 1000.0, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Meteor, WorkerConcurrencyLimit, WorkerConcurrency, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Meteor, SettlementBacklog, Runtime->SettlementQueue.Num(), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Meteor, ProximitySettledLanes,
		static_cast<double>(Runtime->ProximitySettledLaneCount), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Meteor, GlobalOldestSettledLanes,
		static_cast<double>(Runtime->GlobalOldestSettledLaneCount), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Meteor, PumpMilliseconds, Runtime->LastPumpMilliseconds, ECsvCustomStatOp::Set);
	if (Now >= Runtime->LastProgressLogSeconds + 1.0)
	{
		Runtime->LastProgressLogSeconds = Now;
		int32 ReadyTargetCount = 0;
		uint32 CompiledLaneCount = 0;
		for (const FMeteorWorldRuntime::FPreparedTarget& Prepared : Runtime->PreparedTargets)
		{
			ReadyTargetCount += Prepared.IsReady() ? 1 : 0;
			CompiledLaneCount += Prepared.CompiledLaneCount;
		}
		UE_LOG(LogElementSandboxMeteor, Display,
			TEXT("Burst=%llu 波前=%.0fm/%.0fm；核心候选=%d，撞击帧目标=%d，首批 Activate=%d Lane，累计 Activate=%u，首批延迟=%.2fms；Resident Tile=%d/%d；候选=%d；待预演=%d；已预演目标=%d Ready=%d CompiledLane=%u；Scheduler Seed=%d Open=%d BG=%d Urgent=%d Computing=%d Completed=%d；落地积压=%d，近场已结算=%llu，全局最老已结算=%llu；Worker=%d；Pump=%.3fms。"),
			Runtime->BurstId.Value,
			Runtime->PublishedWaveRadius / 100.0,
			Runtime->Config.ShockwaveRadius / 100.0,
			Runtime->CoreCandidateCount,
			Runtime->ImpactFrameDestroyedTargets,
			Runtime->FirstActivationLaneCount,
			Runtime->ActivatedLaneCount,
			Runtime->ImpactToFirstActivationMilliseconds,
			Runtime->NextResidentTileIndex,
			Runtime->Tiles.Num(),
			Runtime->CandidateHeap.Num(),
			Runtime->PreparationHeap.Num(),
			Runtime->PreparedTargets.Num(),
			ReadyTargetCount,
			CompiledLaneCount,
			Runtime->Scheduler.GetPendingSeedCount(),
			SchedulerStats.OpenPages,
			SchedulerStats.BackgroundPages,
			SchedulerStats.UrgentPages,
			SchedulerStats.ComputingPages,
			SchedulerStats.CompletedPages,
			Runtime->SettlementQueue.Num(),
			Runtime->ProximitySettledLaneCount,
			Runtime->GlobalOldestSettledLaneCount,
			Runtime->WorkerInFlight.Load(),
			Runtime->LastPumpMilliseconds);
	}
}

bool UMeteorWorldSubsystem::IsTickable() const
{
	return Runtime && Runtime->bActive && GetWorld() && GetWorld()->GetNetMode() != NM_Client;
}

TStatId UMeteorWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMeteorWorldSubsystem, STATGROUP_Tickables);
}

bool UMeteorWorldSubsystem::ScheduleStrike(
	const FVector& ImpactCenter,
	const double ImpactTimeSeconds,
	FMeteorBurstId& OutBurstId)
{
	OutBurstId = {};
	if (!Runtime || Runtime->bActive || !GetWorld() || GetWorld()->GetNetMode() == NM_Client
		|| ImpactCenter.ContainsNaN() || !FMath::IsFinite(ImpactTimeSeconds)
		|| ImpactTimeSeconds < GetWorld()->GetTimeSeconds())
	{
		return false;
	}
	Runtime->BurstId.Value = Runtime->NextBurstValue++;
	if (!Runtime->BurstId.IsSet()) Runtime->BurstId.Value = Runtime->NextBurstValue++;
	Runtime->ImpactCenter = ImpactCenter;
	Runtime->ImpactCenter.Z = Runtime->Config.GroundPlaneZ;
	Runtime->ImpactTimeSeconds = ImpactTimeSeconds;
	Runtime->PublishedWaveRadius = 0.0;
	Runtime->NextOrdinal = 0;
	Runtime->ActivatedLaneCount = 0;
	Runtime->CoreCandidateCount = 0;
	Runtime->ImpactFrameDestroyedTargets = 0;
	Runtime->FirstActivationLaneCount = 0;
	Runtime->ImpactToFirstActivationMilliseconds = -1.0;
	Runtime->bImpactPumpRecorded = false;
	Runtime->Tiles.Reset();
	Runtime->NextResidentTileIndex = 0;
	Runtime->VisitedBuildings.Reset();
	Runtime->VisitedWorldObjects.Reset();
	Runtime->CandidateHeap.Reset();
	Runtime->PreparationHeap.Reset();
	Runtime->RejectedPreparationTargets.Reset();
	Runtime->PreparedTargets.Reset();
	Runtime->PreparedTargetById.Reset();
	Runtime->OrdinalOwners.Reset();
	Runtime->OrdinalPageLanes.Reset();
	checkf(Runtime->SettlementQueue.Initialize(
		Runtime->BurstId,
		Runtime->Config.SettlementPriorityCellSize,
		Runtime->Config.SettlementPriorityRadius),
		TEXT("有效 Meteor RuleSet 必须可初始化近场优先落地队列。"));
	Runtime->ProximitySettledLaneCount = 0;
	Runtime->GlobalOldestSettledLaneCount = 0;
	Runtime->ActivePages.Reset();
	Runtime->PublishedActivationHistory.Reset();
	Runtime->PublishedSettlementHistory.Reset();
	Runtime->LastProgressLogSeconds = -TNumericLimits<double>::Max();
	Runtime->Scheduler.Initialize(Runtime->BurstId, Runtime->Config, GetWorld()->GetTimeSeconds());
	const int32 RadiusCells = FMath::CeilToInt(Runtime->Config.ShockwaveRadius / QueryTileSize);
	Runtime->Tiles.Reserve((RadiusCells * 2 + 1) * (RadiusCells * 2 + 1));
	for (int32 Y = -RadiusCells; Y <= RadiusCells; ++Y)
	{
		for (int32 X = -RadiusCells; X <= RadiusCells; ++X)
		{
			const FVector Min(
				Runtime->ImpactCenter.X + X * QueryTileSize,
				Runtime->ImpactCenter.Y + Y * QueryTileSize,
				Runtime->Config.GroundPlaneZ - 100000.0);
			const FBox Bounds(Min, Min + FVector(QueryTileSize, QueryTileSize, 200000.0));
			const double Distance = DistanceToBox2D(Runtime->ImpactCenter, Bounds);
			if (Distance <= Runtime->Config.ShockwaveRadius)
			{
				Runtime->Tiles.Add({Bounds, Distance});
			}
		}
	}
	Runtime->Tiles.Sort([](const FMeteorWorldRuntime::FTile& A, const FMeteorWorldRuntime::FTile& B)
	{
		if (!FMath::IsNearlyEqual(A.MinimumDistance, B.MinimumDistance))
			return A.MinimumDistance < B.MinimumDistance;
		const FVector AC = A.Bounds.GetCenter();
		const FVector BC = B.Bounds.GetCenter();
		return AC.X == BC.X ? AC.Y < BC.Y : AC.X < BC.X;
	});
	UWorldStorageSubsystem* WorldStorage = GetWorld()->GetSubsystem<UWorldStorageSubsystem>();
	if (!WorldStorage)
	{
		Runtime->Scheduler.Reset();
		Runtime->SettlementQueue.Reset();
		return false;
	}
	Runtime->MutationBatch = WorldStorage->BeginDelayedMutationBatch();
	if (!Runtime->MutationBatch.IsSet())
	{
		Runtime->Scheduler.Reset();
		Runtime->SettlementQueue.Reset();
		return false;
	}
	Runtime->bActive = true;
	OutBurstId = Runtime->BurstId;
	UE_LOG(LogElementSandboxMeteor, Display,
		TEXT("排程唯一主陨石 Burst=%llu：落点=(%.0f, %.0f, %.0f)，ImpactTime=%.3f，即时核心=%.0fm，总半径=%.0fm，Resident 查询 Tile=%d；不会申请或改变 Chunk Residency。"),
		Runtime->BurstId.Value,
		Runtime->ImpactCenter.X,
		Runtime->ImpactCenter.Y,
		Runtime->ImpactCenter.Z,
		Runtime->ImpactTimeSeconds,
		Runtime->Config.ImpactCoreRadius / 100.0,
		Runtime->Config.ShockwaveRadius / 100.0,
		Runtime->Tiles.Num());
	return true;
}

bool UMeteorWorldSubsystem::HasActiveBurst() const
{
	return Runtime && Runtime->bActive;
}

bool UMeteorWorldSubsystem::TryGetMapImpactLocation(
	const FVector& ViewerLocation,
	const FVector& ViewerForward,
	FVector& OutImpactLocation) const
{
	OutImpactLocation = FVector::ZeroVector;
	if (!Runtime || !GetWorld() || GetWorld()->GetNetMode() == NM_Client)
	{
		return false;
	}
	const UWorldStorageSubsystem* WorldStorage = GetWorld()->GetSubsystem<UWorldStorageSubsystem>();
	if (!WorldStorage || ViewerLocation.ContainsNaN() || ViewerForward.ContainsNaN())
	{
		UE_LOG(LogElementSandboxMeteor, Warning, TEXT("无法选择主陨石落点：观察角色或 WorldStorage 无效。"));
		return false;
	}
	FVector FlatForward(ViewerForward.X, ViewerForward.Y, 0.0);
	FlatForward = FlatForward.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);

	struct FShowcaseCandidate final
	{
		FWorldChunkCoord Coord;
		FVector Center = FVector::ZeroVector;
		int32 DirectPopulation = 0;
		double PreferredPointDistanceSquared = TNumericLimits<double>::Max();
		double ForwardAlignment = -1.0;
		bool bSet = false;
	};

	const FMeteorRuntimeConfig& Config = Runtime->Config;
	const FVector GroundViewer(ViewerLocation.X, ViewerLocation.Y, Config.GroundPlaneZ);
	const FVector PreferredPoint = GroundViewer + FlatForward * Config.MeteorShowcasePreferredDistance;
	const FWorldChunkCoord OriginChunk = FWorldChunkCoord::FromWorldLocation(GroundViewer);
	const int32 MaximumOffset = FMath::CeilToInt(
		Config.MeteorShowcaseMaximumDistance / FWorldChunkCoord::EdgeCentimeters) + 1;
	constexpr double MinimumForwardAlignment = 0.5; // 角色前方正负 60 度，避免落到侧后方。
	FShowcaseCandidate Best;
	for (int32 OffsetX = -MaximumOffset; OffsetX <= MaximumOffset; ++OffsetX)
	{
		for (int32 OffsetY = -MaximumOffset; OffsetY <= MaximumOffset; ++OffsetY)
		{
			const FWorldChunkCoord Coord(
				OriginChunk.X + OffsetX, OriginChunk.Y + OffsetY, OriginChunk.Z);
			if (!WorldStorage->IsChunkResident(Coord))
			{
				continue;
			}
			const int32 DirectPopulation = WorldStorage->GetChunkResidentEntityCount(Coord);
			if (DirectPopulation <= 0)
			{
				continue;
			}
			const FVector Minimum = Coord.GetWorldMinimum();
			const FVector Center(
				Minimum.X + FWorldChunkCoord::EdgeCentimeters * 0.5,
				Minimum.Y + FWorldChunkCoord::EdgeCentimeters * 0.5,
				Config.GroundPlaneZ);
			const FVector FromViewer = Center - GroundViewer;
			const double Distance = FVector2D(FromViewer.X, FromViewer.Y).Size();
			if (Distance < Config.MeteorShowcaseMinimumDistance
				|| Distance > Config.MeteorShowcaseMaximumDistance)
			{
				continue;
			}
			const double ForwardAlignment = FVector2D::DotProduct(
				FVector2D(FromViewer.X, FromViewer.Y).GetSafeNormal(),
				FVector2D(FlatForward.X, FlatForward.Y));
			if (ForwardAlignment < MinimumForwardAlignment)
			{
				continue;
			}

			// 先保证近处正前方可见，再以宿主数量打破平局。
			// 密度不能压过距离，否则更多 Chunk 加载后会把爆裂展示拉向远处。
			const double PreferredDistanceSquared = FVector::DistSquared2D(Center, PreferredPoint);
			const bool bSamePreferredDistance = FMath::IsNearlyEqual(
				PreferredDistanceSquared, Best.PreferredPointDistanceSquared);
			const bool bBetter = !Best.bSet
				|| PreferredDistanceSquared < Best.PreferredPointDistanceSquared - UE_KINDA_SMALL_NUMBER
				|| (bSamePreferredDistance && DirectPopulation > Best.DirectPopulation)
				|| (bSamePreferredDistance
					&& DirectPopulation == Best.DirectPopulation
					&& Coord < Best.Coord);
			if (bBetter)
			{
				Best.Coord = Coord;
				Best.Center = Center;
				Best.DirectPopulation = DirectPopulation;
				Best.PreferredPointDistanceSquared = PreferredDistanceSquared;
				Best.ForwardAlignment = ForwardAlignment;
				Best.bSet = true;
			}
		}
	}

	if (!Best.bSet)
	{
		// 宿主目录可能只覆盖角色身边很小的一圈，不能因此让一次性陨石核心失效。
		// 保底落点仍严格位于角色正前方，并且只改变撞击坐标；它不请求、注入或固定任何 Chunk。
		OutImpactLocation = PreferredPoint;
		UE_LOG(LogElementSandboxMeteor, Warning,
			TEXT("角色前方 %.0f-%.0fm 内没有带宿主的 Resident Chunk；使用正前方 %.0fm 保底落点，冲击波仍只处理既有 Resident 对象且不触发 Chunk 加载。"),
			Config.MeteorShowcaseMinimumDistance / 100.0f,
			Config.MeteorShowcaseMaximumDistance / 100.0f,
			Config.MeteorShowcasePreferredDistance / 100.0f);
		return !OutImpactLocation.ContainsNaN();
	}

	OutImpactLocation = Best.Center;
	UE_LOG(LogElementSandboxMeteor, Display,
		TEXT("主陨石选择角色前方 Resident 展示 Chunk=(%d,%d,%d)：本格=%d，距角色=%.0fm，对齐=%.2f，落点=(%.0f, %.0f, %.0f)；不改变 Chunk Residency。"),
		Best.Coord.X,
		Best.Coord.Y,
		Best.Coord.Z,
		Best.DirectPopulation,
		FVector::Dist2D(GroundViewer, OutImpactLocation) / 100.0f,
		Best.ForwardAlignment,
		OutImpactLocation.X,
		OutImpactLocation.Y,
		OutImpactLocation.Z);
	return !OutImpactLocation.ContainsNaN();
}

bool UMeteorWorldSubsystem::CancelScheduledStrike(const FMeteorBurstId BurstId)
{
	if (!Runtime || !Runtime->bActive || Runtime->BurstId != BurstId || !GetWorld()
		|| GetWorld()->GetTimeSeconds() >= Runtime->ImpactTimeSeconds
		|| Runtime->NextOrdinal != 0 || Runtime->WorkerInFlight.Load() != 0)
	{
		return false;
	}
	Runtime->bActive = false;
	Runtime->Tiles.Reset();
	Runtime->NextResidentTileIndex = 0;
	Runtime->CandidateHeap.Reset();
	Runtime->PreparationHeap.Reset();
	Runtime->RejectedPreparationTargets.Reset();
	Runtime->PreparedTargets.Reset();
	Runtime->PreparedTargetById.Reset();
	Runtime->OrdinalOwners.Reset();
	Runtime->OrdinalPageLanes.Reset();
	Runtime->SettlementQueue.Reset();
	Runtime->ActivePages.Reset();
	Runtime->PublishedActivationHistory.Reset();
	Runtime->Scheduler.Reset();
	if (UWorldStorageSubsystem* WorldStorage = GetWorld()->GetSubsystem<UWorldStorageSubsystem>())
	{
		if (Runtime->MutationBatch.IsSet())
		{
			WorldStorage->CancelEmptyDelayedMutationBatch(Runtime->MutationBatch);
		}
	}
	Runtime->MutationBatch = {};
	return true;
}

FMeteorBurstId UMeteorWorldSubsystem::GetActiveBurstId() const
{
	return Runtime ? Runtime->BurstId : FMeteorBurstId{};
}

FMeteorRuntimeConfig UMeteorWorldSubsystem::GetRuntimeConfig() const
{
	return Runtime ? Runtime->Config : FMeteorRuntimeConfig{};
}

FMeteorAuthorityStats UMeteorWorldSubsystem::GetAuthorityStats() const
{
	FMeteorAuthorityStats Stats;
	if (!Runtime) return Stats;
	Stats.bBurstActive = Runtime->bActive;
	Stats.BurstId = Runtime->BurstId;
	Stats.PublishedWaveRadius = Runtime->PublishedWaveRadius;
	Stats.QueriedTiles = Runtime->NextResidentTileIndex;
	Stats.TotalTiles = Runtime->Tiles.Num();
	Stats.PendingTargets = Runtime->CandidateHeap.Num();
	Stats.CoreCandidateCount = Runtime->CoreCandidateCount;
	Stats.ImpactFrameDestroyedTargets = Runtime->ImpactFrameDestroyedTargets;
	Stats.FirstActivationLaneCount = Runtime->FirstActivationLaneCount;
	Stats.TotalActivatedLaneCount = Runtime->ActivatedLaneCount;
	Stats.ImpactToFirstActivationMilliseconds = Runtime->ImpactToFirstActivationMilliseconds;
	Stats.WorkerInFlight = Runtime->WorkerInFlight.Load();
	Stats.SettlementBacklog = Runtime->SettlementQueue.Num();
	Stats.LastPumpMilliseconds = Runtime->LastPumpMilliseconds;
	Stats.Scheduler = Runtime->Scheduler.GetStats(GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0);
	return Stats;
}

void UMeteorWorldSubsystem::GetPreparedTrajectoryPages(TArray<FMeteorTrajectoryPage>& OutPages) const
{
	OutPages.Reset();
	if (!Runtime || !Runtime->bActive) return;
	TSet<uint64> ActivatedPageIds;
	for (const FMeteorTrajectoryActivation& Activation : Runtime->PublishedActivationHistory)
	{
		ActivatedPageIds.Add(Activation.PageId);
	}
	for (const TPair<uint64, FMeteorTrajectoryPage>& Pair : Runtime->ActivePages)
	{
		if (ActivatedPageIds.Contains(Pair.Key)) OutPages.Add(Pair.Value);
	}
	OutPages.Sort([](const FMeteorTrajectoryPage& A, const FMeteorTrajectoryPage& B)
	{
		return A.PageId < B.PageId;
	});
}

void UMeteorWorldSubsystem::GetPublishedTrajectoryActivations(
	TArray<FMeteorTrajectoryActivation>& OutActivations) const
{
	OutActivations = Runtime && Runtime->bActive
		? Runtime->PublishedActivationHistory : TArray<FMeteorTrajectoryActivation>();
}

void UMeteorWorldSubsystem::GetPublishedSettlementMappings(
	TArray<FMeteorSettlementMapping>& OutMappings) const
{
	OutMappings = Runtime && Runtime->bActive
		? Runtime->PublishedSettlementHistory : TArray<FMeteorSettlementMapping>();
}

FMeteorTrajectoryPagePreparedEvent& UMeteorWorldSubsystem::OnTrajectoryPagePrepared()
{
	check(Runtime);
	return Runtime->PagePreparedEvent;
}

FMeteorTrajectoryActivatedEvent& UMeteorWorldSubsystem::OnTrajectoryActivated()
{
	check(Runtime);
	return Runtime->TrajectoryActivatedEvent;
}

FMeteorTrajectoryCanceledEvent& UMeteorWorldSubsystem::OnTrajectoryCanceled()
{
	check(Runtime);
	return Runtime->TrajectoryCanceledEvent;
}

FMeteorSettlementPublishedEvent& UMeteorWorldSubsystem::OnSettlementPublished()
{
	check(Runtime);
	return Runtime->SettlementPublishedEvent;
}

bool UMeteorWorldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

#if WITH_DEV_AUTOMATION_TESTS
void UMeteorWorldSubsystem::OverrideRuntimeConfigForTesting(const FMeteorRuntimeConfig& Config)
{
	check(Runtime && !Runtime->bActive && Config.IsValid());
	Runtime->Config = Config;
}
#endif
