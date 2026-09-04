#pragma once

#include "CoreMinimal.h"
#include "Containers/ContainerAllocationPolicies.h"
#include "Entity/WorldEntityId.h"

namespace UE::ElementSandbox::Meteor
{
	constexpr int32 WorkPageCapacity = 1024;
	constexpr int32 ClientDirectoryPageCapacity = 4096;
	constexpr uint32 TrajectoryPayloadFormatVersion = 4;

	struct ELEMENTSANDBOXMETEOR_API FMeteorBurstId final
	{
		uint64 Value = 0;
		bool IsSet() const { return Value != 0; }
		friend bool operator==(const FMeteorBurstId&, const FMeteorBurstId&) = default;
		friend bool operator<(const FMeteorBurstId& A, const FMeteorBurstId& B) { return A.Value < B.Value; }
		friend uint32 GetTypeHash(const FMeteorBurstId Id) { return GetTypeHash(Id.Value); }
	};

	struct ELEMENTSANDBOXMETEOR_API FMeteorDebrisKey final
	{
		FMeteorBurstId BurstId;
		uint32 DebrisOrdinal = MAX_uint32;
		bool IsSet() const { return BurstId.IsSet() && DebrisOrdinal != MAX_uint32; }
		friend bool operator==(const FMeteorDebrisKey&, const FMeteorDebrisKey&) = default;
	};

	enum class EMeteorTrajectoryKernel : uint8
	{
		BallisticGroundPlane = 1
	};

	struct ELEMENTSANDBOXMETEOR_API FMeteorPageHandle final
	{
		uint32 Slot = MAX_uint32;
		uint32 Generation = 0;
		bool IsSet() const { return Slot != MAX_uint32 && Generation != 0; }
		friend bool operator==(const FMeteorPageHandle&, const FMeteorPageHandle&) = default;
	};

	/** 从破坏 Sink 进入服务器页面编译器的一条产品。 */
	struct ELEMENTSANDBOXMETEOR_API FMeteorDebrisSeed final
	{
		FMeteorDebrisKey Key;
		/** Authority 预留的最终身份；预留本身不创建世界实体，取消后也不复用。 */
		FWorldEntityId WorldEntityId;
		/** 破坏 Definition 配置的产品 DefinitionId；与斧头实际生成的产品完全同源。 */
		FName RenderArchetypeId = NAME_None;
		FVector StartPosition = FVector::ZeroVector;
		FQuat StartRotation = FQuat::Identity;
		FVector InitialVelocity = FVector::ZeroVector;
		FVector AngularVelocityDegrees = FVector::ZeroVector;
		FVector3f Scale = FVector3f::OneVector;
		/** 同一 RenderArchetype 页共享的规范产品 Bounds；用于求最终旋转姿态的精确触地中心。 */
		FBox3f ProductLocalBounds = FBox3f(EForceInit::ForceInit);
		float VisualRadius = 100.0f;
		double StartTimeSeconds = 0.0;
		double ValidFromSeconds = 0.0;
		double LatestComputeStartSeconds = 0.0;

		bool IsValid() const;
	};

	struct ELEMENTSANDBOXMETEOR_API FMeteorWorkPage final
	{
		FMeteorPageHandle Handle;
		EMeteorTrajectoryKernel Kernel = EMeteorTrajectoryKernel::BallisticGroundPlane;
		FName RenderArchetypeId = NAME_None;
		FVector3d PageOrigin = FVector3d::ZeroVector;
		FBox3f ProductLocalBounds = FBox3f(EForceInit::ForceInit);
		double EarliestDeadlineSeconds = TNumericLimits<double>::Max();
		uint32 Revision = 1;

		TArray<uint32, TAlignedHeapAllocator<64>> Ordinals;
		TArray<FWorldEntityId, TAlignedHeapAllocator<64>> WorldEntityIds;
		TArray<float, TAlignedHeapAllocator<64>> StartX;
		TArray<float, TAlignedHeapAllocator<64>> StartY;
		TArray<float, TAlignedHeapAllocator<64>> StartZ;
		TArray<float, TAlignedHeapAllocator<64>> VelocityX;
		TArray<float, TAlignedHeapAllocator<64>> VelocityY;
		TArray<float, TAlignedHeapAllocator<64>> VelocityZ;
		TArray<float, TAlignedHeapAllocator<64>> AngularX;
		TArray<float, TAlignedHeapAllocator<64>> AngularY;
		TArray<float, TAlignedHeapAllocator<64>> AngularZ;
		TArray<FQuat4f, TAlignedHeapAllocator<64>> StartRotations;
		TArray<FVector3f, TAlignedHeapAllocator<64>> Scales;
		TArray<float, TAlignedHeapAllocator<64>> VisualRadii;
		TArray<double, TAlignedHeapAllocator<64>> StartTimes;
		TArray<double, TAlignedHeapAllocator<64>> ValidFromTimes;

		void Reset(FMeteorPageHandle InHandle, FName ProductId, const FVector3d& Origin);
		bool Append(const FMeteorDebrisSeed& Seed);
		int32 Num() const { return Ordinals.Num(); }
		bool IsFull() const { return Num() >= WorkPageCapacity; }
		bool IsStructurallyValid() const;
	};

	/** Worker 输出与网络传输共用的不可变轨迹页。 */
	struct ELEMENTSANDBOXMETEOR_API FMeteorTrajectoryPage final
	{
		FMeteorBurstId BurstId;
		uint64 PageId = 0;
		uint32 Revision = 1;
		uint32 FormatVersion = TrajectoryPayloadFormatVersion;
		EMeteorTrajectoryKernel Kernel = EMeteorTrajectoryKernel::BallisticGroundPlane;
		FName RenderArchetypeId = NAME_None;
		FVector3d PageOrigin = FVector3d::ZeroVector;
		double ValidFromSeconds = 0.0;
		double ValidUntilSeconds = 0.0;
		FBox3f SweptBounds = FBox3f(EForceInit::ForceInit);

		TArray<uint32> Ordinals;
		TArray<FWorldEntityId> WorldEntityIds;
		TArray<FVector3f> LocalStarts;
		TArray<FVector3f> InitialVelocities;
		TArray<FVector3f> Accelerations;
		TArray<FVector3f> AngularVelocitiesDegrees;
		TArray<FQuat4f> StartRotations;
		TArray<FVector3f> Scales;
		TArray<float> VisualRadii;
		TArray<float> StartTimeOffsets;
		/** 起飞到旋转包围盒首次接触地面的时间。 */
		TArray<float> ImpactDurations;
		/** 首次触地后解析翻滚并收敛到稳定支撑面的时间。 */
		TArray<float> SettlingDurations;
		TArray<FVector3f> LocalImpactEndpoints;
		TArray<FVector3f> LocalRestEndpoints;
		TArray<FQuat4f> RestRotations;
		/** 结算插值为避免盒体穿地而使用的中段抬升包络高度。 */
		TArray<float> SettlingLiftHeights;

		int32 Num() const { return Ordinals.Num(); }
		bool IsValid() const;
		bool SerializeToBytes(TArray<uint8>& OutBytes) const;
		/** 客户端准备与 Authority 落地共同使用 Chunk 精度终态，认领不能被 Snapshot 量化触发位置更新。 */
		FTransform GetRestTransform(int32 Lane) const;
		static bool DeserializeFromBytes(TConstArrayView<uint8> Bytes, FMeteorTrajectoryPage& OutPage);
		/** 从已预流送页面抽取本次已提交源对应的 Lane；页面身份保持不变。 */
		bool BuildOrdinalSubset(TConstArrayView<uint32> ActivatedOrdinals, FMeteorTrajectoryPage& OutPage) const;
	};

	/** Payload 与源提交解耦后的轻量激活消息；只让已成功 GameplayDestroy 的 Lane 开始表现。 */
	struct ELEMENTSANDBOXMETEOR_API FMeteorTrajectoryActivation final
	{
		FMeteorBurstId BurstId;
		uint64 PageId = 0;
		uint32 Revision = 0;
		/** 与本次 Lane 同因果提交的源实体删除；客户端必须先应用它，随后才可显示碎片。 */
		FWorldEntityId SourceWorldEntityId;
		uint32 SourceTombstoneRevision = 0;
		/**
		 * 对应源实体真正完成 GameplayDestroy/Commit 的服务器绝对时刻。
		 * Page 内时间只服务预演与 Deadline；客户端必须从这个时刻开始播放，
		 * 只有网络或客户端预算真正迟到时才允许按该时刻快进。
		 */
		double AuthorityStartTimeSeconds = 0.0;
		TArray<uint32> Ordinals;

		bool IsValid() const
		{
			return BurstId.IsSet() && PageId != 0 && Revision != 0
				&& SourceWorldEntityId.IsSet() && SourceTombstoneRevision != 0
				&& FMath::IsFinite(AuthorityStartTimeSeconds)
				&& AuthorityStartTimeSeconds >= 0.0 && !Ordinals.IsEmpty();
		}

		/** Cancel 不提交源生命周期，因此只校验页面身份与 Ordinal。 */
		bool IsCancellationValid() const
		{
			return BurstId.IsSet() && PageId != 0 && Revision != 0 && !Ordinals.IsEmpty();
		}
	};

	struct ELEMENTSANDBOXMETEOR_API FMeteorRuntimeConfig final
	{
		/** 主陨石球心相对统一地面的初始高度。 */
		float MeteorHeight = 600000.0f;
		/** 单颗主陨石直径；Engine Basic Sphere 的原始直径为 100cm。 */
		float MeteorDiameter = 300000.0f;
		float MeteorFallSeconds = 6.0f;
		/** 只在角色前方近场选择有宿主的 Resident Chunk，优先靠近正前方首选点。 */
		float MeteorShowcaseMinimumDistance = 10000.0f;
		float MeteorShowcasePreferredDistance = 20000.0f;
		float MeteorShowcaseMaximumDistance = 30000.0f;
		/** 陨石从落点相对角色的外侧斜向切入；它不再从角色头顶垂落。 */
		float MeteorApproachHorizontalDistance = 400000.0f;
		float ShockwaveRadius = 600000.0f;
		/** 撞击帧立即释放的核心半径；外围波前从该半径继续扩散。 */
		float ImpactCoreRadius = 200000.0f;
		float ShockwaveSpeed = 300000.0f;
		/** 破坏命令携带的冲击强度包络；解析 Lane 由下方速度/角度参数独立采样。 */
		float RadialStrength = 18000.0f;
		float UpwardStrength = 14000.0f;
		/** 解析 Lane 使用可读的宽角度抛物线；中高弧线构成主体，避免高速贴地射流。 */
		FVector2f DebrisSpeedRange = FVector2f(9000.0f, 22000.0f);
		FVector2f DebrisLowElevationDegrees = FVector2f(8.0f, 28.0f);
		FVector2f DebrisMediumElevationDegrees = FVector2f(28.0f, 50.0f);
		FVector2f DebrisHighElevationDegrees = FVector2f(50.0f, 72.0f);
		float DebrisMediumArcFraction = 0.42f;
		float DebrisHighArcFraction = 0.18f;
		/** 空中 Lane 相对源径向的最大偏转；必须小于 90°，保证主速度永远远离爆心。 */
		float DebrisAirMaximumAzimuthDeviationDegrees = 86.0f;
		/** 近地 Lane 允许接近切向散开，但仍不得反向飞回爆心。 */
		float DebrisGroundMaximumAzimuthDeviationDegrees = 88.0f;
		/** 短程近地碎片负责在各源附近落下，避免只剩一圈空心壳。 */
		float DebrisGroundScatterFraction = 0.18f;
		FVector2f DebrisGroundElevationDegrees = FVector2f(-8.0f, 12.0f);
		FVector2f DebrisGroundSpeedMultiplier = FVector2f(0.35f, 0.65f);
		float GravityZ = -1600.0f;
		float GroundPlaneZ = 0.0f;
		/** 首次触地后仍在 GPU 解析 Lane 内翻滚、躺平的时长。 */
		float DebrisSettlingSeconds = 0.55f;
		float NetworkLeadSeconds = 5.0f;
		float EncodingEstimateSeconds = 0.10f;
		float QueueSafetySeconds = 0.35f;
		int32 MaximumWorkPages = 512;
		int32 MaximumQueryTilesPerPump = 64;
		int32 MaximumDestructionTargetsPerPump = 1024;
		/** 单次宿主 Query Snapshot 广播的最大销毁目标数；较小批次限制一次同步 Commit 的不可中断尾延迟。 */
		int32 MaximumDestructionTargetsPerSnapshotBatch = 32;
		/** 本机外置服务器与可见客户端竞争 CPU；撞击提交必须保留客户端一帧的调度余量。 */
		float LocalServerGameplayBudgetMilliseconds = 3.0f;
		float DedicatedServerGameplayBudgetMilliseconds = 8.0f;
		int32 LocalServerWorkerConcurrency = 1;
		int32 DedicatedServerWorkerConcurrency = 4;
		/** 单次不可抢占的 WorldObject Stage/Commit 上限；近场优先不得突破该硬上限。 */
		int32 MaximumSettlementBatchSize = 128;
		/** 落地待办的二维索引 Cell 边长；每帧只枚举角色周边 Cell。 */
		float SettlementPriorityCellSize = 5000.0f;
		/** 该半径内已到期木块优先实体化为可拾取 WorldObject。 */
		float SettlementPriorityRadius = 30000.0f;
		/** 每批为全局最老项保留的最小配额，防止远景永久饥饿。 */
		int32 MinimumGlobalSettlementCountPerBatch = 16;
		/** Prepare/Activate 生命周期工作共用的 GameThread 墙钟预算。 */
		float ClientGameThreadBudgetMilliseconds = 1.5f;

		bool IsValid() const;
		/** 用落点相对观察角色的外向方向构造可见的斜向入射起点。 */
		FVector ComputeMeteorStartLocation(
			const FVector& ImpactLocation,
			const FVector& ViewerLocation) const;
		double ComputeShockwaveArrivalTime(double ImpactTimeSeconds, double Distance) const;
		double ComputeShockwaveRadius(double SecondsAfterImpact) const;
	};
}
