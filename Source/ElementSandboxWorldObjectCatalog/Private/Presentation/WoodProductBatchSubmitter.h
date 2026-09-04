#pragma once
#include "Presentation/WoodProductInstanceStore.h"
class AActor;
class UWoodProductFlightMaterialSet;
class UWoodProductPresentationSettings;

/** 仅生命周期批次触碰原生实例缓冲；飞行帧只检查组件级结束时刻和异步建树状态。 */
class FWoodProductBatchSubmitter final
{
public:
	void Tick(FWoodProductInstanceStore& Store, AActor& Host, UWoodProductFlightMaterialSet& Materials,
		const UWoodProductPresentationSettings& Settings, double Now);
private:
	void Apply(FWoodProductInstanceStore& Store, AActor& Host, UWoodProductFlightMaterialSet& Materials,
		const UWoodProductPresentationSettings& Settings, TConstArrayView<FWorldEntityId> Work, double Now);
};
