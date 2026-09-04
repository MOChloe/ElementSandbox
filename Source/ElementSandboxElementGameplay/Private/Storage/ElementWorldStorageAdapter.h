#pragma once

#include "Storage/WorldStorageDomainAdapter.h"

class FElementFireDomain;

/** 创建正式 Element Dependent Adapter；Payload 仅接受当前单一格式。 */
TSharedRef<IWorldStorageDomainAdapter> MakeElementWorldStorageAdapter(FElementFireDomain& Runtime);
