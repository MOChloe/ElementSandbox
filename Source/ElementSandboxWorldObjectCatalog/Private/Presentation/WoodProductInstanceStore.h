#pragma once
#include "CoreMinimal.h"
#include "WorldObjects/WoodProductFlight.h"
#include "Chunk/WorldChunkCoordinates.h"

class UDeferredHISMComponent;
/** 统一木块身份目录。普通 Upsert 与飞行共用槽位；移除交换必须回填被移动身份。 */
class FWoodProductInstanceStore final
{
public:
	struct FGroupKey
	{
		/** 只包含不会随飞行状态变化的 Mesh 类型与空间剔除边界。 */
		FName Product;
		FIntPoint Cell = FIntPoint::ZeroValue;
		friend bool operator==(const FGroupKey&, const FGroupKey&) = default;
		friend uint32 GetTypeHash(const FGroupKey& K)
		{
			return HashCombineFast(GetTypeHash(K.Product), GetTypeHash(K.Cell));
		}
	};
	struct FEntry
	{
		FName Product;
		FTransform Transform = FTransform::Identity;
		FGroupKey Group;
		int32 Index = INDEX_NONE, FlightIndex = INDEX_NONE;
		bool bPersistent = false, bRemoved = false, bQueued = false, bServerSettled = false;
		bool bPreparedOnGPU = false, bVisualFinished = false;
	};
	struct FGroup
	{
		TWeakObjectPtr<UDeferredHISMComponent> Component;
		TArray<FWorldEntityId> Owners;
		bool bFlightMaterial = false, bScheduled = false;
		int32 PreparedCount = 0, FlightMaterialTier = INDEX_NONE;
		double LastEndTime = 0.0;
	};
	bool AcceptFlight(const FWoodProductFlight& Flight);
	void Upsert(FWorldEntityId Id, FName Product, const FTransform& Transform);
	void Remove(FWorldEntityId Id);
	void RetireBurst(uint64 Burst);
	void Queue(FWorldEntityId Id);
	void ScheduleGroup(const FGroupKey& Key);
	/** 飞行使用整条轨迹包络；落地后只保留最终位置仍在兴趣并集内的临时引用。 */
	bool IsRetained(const FEntry& Entry) const;
	void SetRetentionBoxes(TArray<FWorldChunkBox>&& Boxes);
	FWoodProductFlight* FindFlight(const FEntry& Entry);
	const FWoodProductFlight* FindFlight(const FEntry& Entry) const;
	void SetFlight(FEntry& Entry, const FWoodProductFlight& Flight);
	void ReleaseFlight(FEntry& Entry);
	// 临时参数使用连续存储；最后一组结束时实际释放，静态目录不内嵌整份飞行参数。
	TArray<FWoodProductFlight> FlightData;
	TArray<int32> FreeFlightSlots;
	int32 ActiveFlightCount = 0;
	TMap<FWorldEntityId, FEntry> Entries;
	TMap<FGroupKey, TUniquePtr<FGroup>> Groups;
	TArray<FWorldEntityId> Pending;
	int32 PendingHead = 0;
	TArray<FGroupKey> Maintenance;
	int32 MaintenanceHead = 0;
	TArray<FWorldChunkBox> RetentionBoxes;
	bool bHasInterest = false;
	TSet<FWorldEntityId> TerminalFlights;
	uint64 RetiredBurst = 0;
	uint64 TotalAdds = 0, TotalRemoves = 0, TotalTransformUpdates = 0, TotalAdoptions = 0, NativeBatches = 0;
	double LastApplyMilliseconds = 0;
};
