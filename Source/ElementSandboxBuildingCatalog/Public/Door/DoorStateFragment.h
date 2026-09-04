#pragma once

#include "CoreMinimal.h"
#include "Entity/BuildFragment.h"

#include "DoorStateFragment.generated.h"

/** Door 的互斥稳定态与过渡态；避免两个 bool 形成无意义组合。 */
UENUM()
enum class EBuildDoorState : uint8
{
	Closed,
	Opening,
	Open,
	Closing
};

/** 每个 Door Entity 独占的运行时状态；后续可作为服务器权威同步数据。 */
USTRUCT()
struct ELEMENTSANDBOXBUILDINGCATALOG_API FBuildDoorStateFragment : public FBuildFragment
{
	GENERATED_BODY()

	UPROPERTY()
	EBuildDoorState State = EBuildDoorState::Closed;

	/** 进入 Opening/Closing 时的服务器时间；客户端据此本地派生动画，不逐帧同步开度。 */
	UPROPERTY()
	double TransitionStartServerTimeSeconds = 0.0;
};
