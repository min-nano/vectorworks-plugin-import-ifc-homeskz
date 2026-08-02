//
//	draw/DrawUtil.cpp
//
//	draw/ 共通ヘルパーの実装。呼ぶ SDK API はいずれも従来 各 draw/*.cpp が個別に
//	持っていたものと同一で、集約しただけ（振る舞いは変えない）。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには
//	入れない（CLAUDE.md「依存の向きは厳守する」）。
//

#include "PluginPrefix.h"
#include "draw/DrawUtil.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// デザインレイヤの種別コード（CreateLayer の layerType 引数）。1 = デザインレイヤ、
		// 2 = シート（プレゼンテーション）レイヤ。本プラグインが描くのはデザインレイヤ。
		constexpr short kDesignLayerType = 1;
	} // namespace

	void SetClassByName(MCObjectHandle object, const std::string& className)
	{
		if (className.empty())
			return;
		const InternalIndex classID = gSDK->AddClass(TXString(className.c_str()));
		gSDK->SetObjectClass(object, classID);
	}

	void SetAllAttributesByClass(MCObjectHandle object)
	{
		gSDK->SetPColorsByClass(object);
		gSDK->SetFColorsByClass(object);
		gSDK->SetLWByClass(object);
		gSDK->SetPPatByClass(object);
		gSDK->SetFPatByClass(object);
		gSDK->SetArrowByClass(object);
		gSDK->SetOpacityByClass(object);
	}

	TXString ResolveParamName(const VWParametricObj& pio, const char* universalName,
							  const char* localizedName)
	{
		// const にしない: 戻り値として返すので、const だと move されず余計なコピーになる
		// （clang-tidy performance-no-automatic-move）。
		TXString universal(universalName);
		if (pio.GetParamIndex(universal) != static_cast<size_t>(-1))
			return universal;

		const TXString localized(localizedName);
		const size_t count = pio.GetParamsCount();
		for (size_t i = 0; i < count; ++i)
		{
			if (pio.GetParamLocalizedName(i) == localized)
				return pio.GetParamName(i);
		}
		return universal;
	}

	bool SetParamRealChecked(VWParametricObj& pio, const TXString& param, double value,
							 double tolerance)
	{
		pio.SetParamReal(param, value);
		if (std::abs(pio.GetParamReal(param) - value) <= tolerance)
			return true;

		// 実数で入らなかった＝そのパラメータは文字列で保持されている。文字列で入れ直す
		// （"%g" で余分な 0 を落とす。寸法は mm の実数）。
		std::array<char, 32> buffer{};
		std::snprintf(buffer.data(), buffer.size(), "%g", value);
		pio.SetParamAsString(param, TXString(buffer.data()));
		return std::abs(pio.GetParamReal(param) - value) <= tolerance;
	}

	MCObjectHandle PrepareLayer(const std::string& layerName)
	{
		const TXString name(layerName.c_str());
		MCObjectHandle layer = gSDK->GetNamedLayer(name);
		if (layer == nil)
			layer = gSDK->CreateLayer(name, kDesignLayerType);
		if (layer != nil)
			gSDK->SetCurrentLayer(layer);
		return layer;
	}

	MCObjectHandle ActivateExistingLayer(const std::string& layerName)
	{
		MCObjectHandle layer = gSDK->GetNamedLayer(TXString(layerName.c_str()));
		if (layer == nil)
			return nil;
		gSDK->SetCurrentLayer(layer);
		return layer;
	}
} // namespace HomeskzIfcImport::draw
