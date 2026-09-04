#pragma once

#include "CoreMinimal.h"

class UWorld;

/** 启动期判定 WorldStorage 进程职责所需的纯值上下文。 */
struct ELEMENTSANDBOXWORLDSTORAGE_API FWorldStorageProcessRoleContext final
{
	bool bGameProcess = false;
	bool bPlayInEditorStandaloneWorld = false;
	bool bCommandlet = false;
	bool bAutomationTesting = false;
	bool bDedicatedServer = false;
	bool bLocalServerChild = false;
	bool bLocalServerLaunchSuppressed = false;
};

/**
 * 统一决定当前 Standalone World 是否只是等待外部本地 Authority 的 Client 壳。
 * LocalServer 启动器与 WorldStorage 必须共用这一判定，避免一边未启动 Server、另一边却等待网络 ACK。
 */
class ELEMENTSANDBOXWORLDSTORAGE_API FWorldStorageProcessRole final
{
public:
	static bool ShouldUseExternalLocalServer(const FWorldStorageProcessRoleContext& Context);

	/**
	 * 同时读取进程职责与当前 Game World。普通 PIE 的进程不是 Game Process，
	 * 但其 Standalone World 仍必须作为外部 Authority 的 Client 壳启动。
	 */
	static bool ShouldUseExternalLocalServerForCurrentWorld(const UWorld* World);
};
