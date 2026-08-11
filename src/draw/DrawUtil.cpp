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

#include "VWFC/VWObjects/VWGroupObj.h"
#include "VWFC/VWObjects/VWPolygon2DObj.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// デザインレイヤの種別コード（CreateLayer の layerType 引数）。1 = デザインレイヤ、
		// 2 = シート（プレゼンテーション）レイヤ。本プラグインが描くのはデザインレイヤ。
		constexpr short kDesignLayerType = 1;

		// 既存のコンポーネント（層）数。取得できなければ 0（＝層を持たない）とみなす。
		short CountComponents(MCObjectHandle object)
		{
			short count = 0;
			if (!gSDK->GetNumberOfComponents(object, count))
				return 0;
			return count;
		}

		// baseName から**まだ使われていない**名前付きリソース名を作って out に入れる。
		// 埋まっていれば " (2)" … と連番を付ける。既存のリソースには触れないので、そこで
		// 設定済みのクラス・マテリアル・用途が失われることが構造的に起きない。連番の上限は
		// 「同名が延々と埋まっている」異常時に無限ループしないための歯止めで、実運用で届く
		// 数ではない。baseName が空・空きが見つからないときは false。
		bool FindFreeResourceName(const std::string& baseName, std::string& out)
		{
			if (baseName.empty())
				return false;

			constexpr int kMaxAttempts = 1000;
			std::string name = baseName;
			for (int attempt = 2; gSDK->GetNamedObject(TXString(name.c_str())) != nil; ++attempt)
			{
				if (attempt > kMaxAttempts)
					return false;
				name = baseName + " (" + std::to_string(attempt) + ")";
			}
			out = name;
			return true;
		}
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

	MCObjectHandle CreateClosedPolygon(const std::vector<core::Vec2>& boundary)
	{
		if (boundary.empty())
			return nil;

		std::vector<VWPoint2D> vertices;
		vertices.reserve(boundary.size());
		for (const core::Vec2& point : boundary)
			vertices.emplace_back(point.x, point.y);

		VWPolygon2DObj polygon(vertices);
		polygon.SetClosed(true); // スラブのプロファイルは閉じた外形
		return polygon.GetThisObject();
	}

	void SetComponents(MCObjectHandle object, const std::vector<core::ComponentCommand>& components)
	{
		const short original = CountComponents(object);
		const auto wanted = static_cast<short>(components.size());

		// 1. 命令の層を先頭から順に挿入する（索引 i の層の「手前」に入るので、0,1,… の順に
		//    入れると命令どおりの並びが先頭にできる）。fill / ペン太さ / 線種は文書の既定に
		//    任せ（0）、描画属性はクラスに従わせる。
		for (short index = 0; index < wanted; ++index)
		{
			const core::ComponentCommand& component = components[static_cast<std::size_t>(index)];
			gSDK->InsertNewComponentN(object, index, component.thickness, 0, 0, 0, 0, 0);
			gSDK->SetComponentWidth(object, index, component.thickness);
			gSDK->SetComponentName(object, index, TXString(component.name.c_str()));
		}

		// 2. 挿入した層の直後に並んでいる元の層を、前から順に削除する（索引 wanted は常に
		//    「元の層の先頭」を指すので、同じ索引を元の層数だけ削除すればよい）。
		for (short removed = 0; removed < original; ++removed)
		{
			if (!gSDK->DeleteComponent(object, wanted))
				break;
		}
	}

	void SetSlabDatum(MCObjectHandle object, core::SlabDatum datum, short componentCount)
	{
		if (componentCount <= 0)
			return;
		// ローカル名は SDK 側の引数名（componentIndex / datumIsTopOfComponent）に寄せてある。
		// 似た並びの short + bool を渡すため、名前が違うと clang-tidy の
		// readability-suspicious-call-argument が「引数が入れ替わっているのでは」と誤検知する。
		const bool datumIsTop = (datum == core::SlabDatum::Top);
		// 三項演算子の共通型は int になるので、short への縮小は 1 か所でまとめて行う
		// （型はキャスト側に書いてあるので auto。modernize-use-auto）。
		const auto componentIndex = static_cast<short>(datumIsTop ? 0 : componentCount - 1);
		gSDK->SetDatumSlabComponent(object, componentIndex);
		// 構成要素を指すだけでは既定の面（中心／下端）のままなので、面も明示する。
		gSDK->SetComponentDatumIsTopOfComponent(object, componentIndex, datumIsTop);
	}

	InternalIndex ResolveSlabStyle(const std::string& styleName,
								   const std::vector<core::ComponentCommand>& components,
								   core::SlabDatum datum)
	{
		if (styleName.empty())
			return 0;

		const TXString name(styleName.c_str());
		MCObjectHandle style = gSDK->GetNamedObject(name);
		if (style == nil)
			style = gSDK->CreateSlabStyle(name);
		if (style == nil)
			return 0;

		SetComponents(style, components);
		// 基準面（構成要素とその上端／下端）はスタイルが持つので、スタイル側へ設定する。
		SetSlabDatum(style, datum, static_cast<short>(components.size()));
		return gSDK->GetObjectInternalIndex(style);
	}

	InternalIndex CreateUniqueSlabStyle(const std::string& baseName,
										const std::vector<core::ComponentCommand>& components,
										core::SlabDatum datum, std::string* outName)
	{
		std::string name;
		if (!FindFreeResourceName(baseName, name))
			return 0;

		MCObjectHandle style = gSDK->CreateSlabStyle(TXString(name.c_str()));
		if (style == nil)
			return 0;

		SetComponents(style, components);
		// 基準面（構成要素とその上端／下端）はスタイルが持つので、スタイル側へ設定する。
		SetSlabDatum(style, datum, static_cast<short>(components.size()));
		if (outName != nullptr)
			*outName = name;
		return gSDK->GetObjectInternalIndex(style);
	}

	InternalIndex CreateUniqueWallStyle(const std::string& baseName,
										const std::vector<core::ComponentCommand>& components,
										std::string* outName)
	{
		std::string name;
		if (!FindFreeResourceName(baseName, name))
			return 0;

		MCObjectHandle style = gSDK->CreateWallStyle(TXString(name.c_str()));
		if (style == nil)
			return 0;

		// 壁は構成層の合計がそのまま壁厚になるので、スラブと違って基準面の設定は要らない。
		SetComponents(style, components);

		// **コア構成要素**を指定する（VW が結合部で構成要素を融合する基準になる。指定が無いと
		// 壁結合しても平面で層が繋がらず、取り合いに面線が残る＝ローカル確認で判明した T 字の
		// 線。ROADMAP.md M10）。基礎の立上りは構成が 1 層（コンクリート）なので、その 1 枚が
		// コアになる。索引は SetComponents と同じ **0 始まり**（draw/DrawUtil.h 参照）。
		if (!components.empty())
			gSDK->SetCoreWallComponent(style, 0);

		if (outName != nullptr)
			*outName = name;
		return gSDK->GetObjectInternalIndex(style);
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

	MCObjectHandle CreateRectangleProfileGroup(double minX, double minY, double maxX, double maxY)
	{
		if (maxX - minX <= 0.0 || maxY - minY <= 0.0)
			return nil;

		VWPolygon2DObj profile({VWPoint2D(minX, minY), VWPoint2D(minX, maxY), VWPoint2D(maxX, maxY),
								VWPoint2D(maxX, minY)});
		profile.SetClosed(true);
		const MCObjectHandle profileHandle = profile.GetThisObject();
		if (profileHandle == nil)
			return nil;

		VWGroupObj group;
		group.AddObject(profileHandle);
		const MCObjectHandle groupHandle = group.GetThisObject();
		if (groupHandle == nil)
			return nil;
		// 断面が本当に入ったか（空のグループを渡さない。ヘッダ参照）。
		if (VWGroupObj(groupHandle).GetFirstMemberObject() == nil)
			return nil;
		return groupHandle;
	}

	RefNumber ResolvePluginStyle(const TXString& styleName)
	{
		MCObjectHandle style = gSDK->GetNamedObject(styleName);
		if (style == nil || !gSDK->IsPluginStyle(style))
			return 0;
		return static_cast<RefNumber>(gSDK->GetObjectInternalIndex(style));
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
