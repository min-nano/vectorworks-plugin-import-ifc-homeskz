//
//	draw/LayerElevation.cpp
//
//	layerElevation の実装。【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include する。
//

#include "PluginPrefix.h"
#include "draw/LayerElevation.h"

namespace HomeskzIfcImport::draw
{
	double layerElevation(MCObjectHandle layer)
	{
		if (layer == nil)
			return 0.0;

		TVariableBlock value;
		if (!gSDK->GetObjectVariable(layer, ovLayerHeightInCurrUnits, value))
			return 0.0;
		double elevation = 0.0;
		if (!value.GetReal64(elevation))
			return 0.0;
		return elevation;
	}
} // namespace HomeskzIfcImport::draw
