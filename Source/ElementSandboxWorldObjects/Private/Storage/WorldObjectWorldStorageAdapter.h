#pragma once

#include "WorldStorageSubsystem.h"

class UWorldObjectWorldSubsystem;

/** 创建 WorldObject 领域的 WorldStorage Adapter；具体编解码不暴露给运行时 Subsystem。 */
TSharedRef<IWorldStorageDomainAdapter> MakeWorldObjectWorldStorageAdapter(UWorldObjectWorldSubsystem& Owner);
