#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/PimplPtr.h"

#include "LocalServerSessionSubsystem.generated.h"

class FLocalServerSessionRuntime;

/**
 * 单机也使用真实跨进程客户端/服务器协议。客户端隐藏启动本地 Dedicated/Editor Server，
 * 等待 Ready 标记后再走 Loopback ClientTravel；退出时要求最终 Checkpoint。
 */
UCLASS()
class ELEMENTSANDBOX_API ULocalServerSessionSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool IsManagingLocalServer() const;
	bool IsLocalServerReady() const;

private:
	bool TickSession(float DeltaSeconds);
	bool LaunchClientServerProcess();
	void TickServerChild();
	void TickClientParent();
	void RequestServerShutdownAndWait();

	TPimplPtr<FLocalServerSessionRuntime> Runtime;
};
