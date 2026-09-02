//
//	Extensions/ExtFoundation.cpp
//
//	基礎の PIO の実装（意図は ExtFoundation.h 参照）。
//
//	リセット（Recalculate）のたびに、
//	  1. レコードの文字列パラメータから部品を復号し（core::decodeFoundation）、
//	  2. OIP の寸法パラメータを読んで部品へ差を配り（core::applyFoundationParams）、
//	  3. 平面の外形（2D ポリゴン）とソリッド（押し出し）を自分の中に描く
//	という流れで作図する。幾何はすべて core/Foundation が決めるので、ここに基礎の形の
//	知識は無い（テストできない場所に形の知識を積まないため）。
//
//	【座標は PIO のローカル】PIO のジオメトリは挿入点からの相対で持たれる（SDK リファレンス
//	Findings「Parametric Objects」）。取り込みは PIO を**原点**に置く（draw/Footing）ので、
//	部品のワールド座標をそのまま描けばよい。ユーザーが PIO を動かせば基礎全体が一緒に動く
//	——それが 1 つのオブジェクトにした意味なので、ResetOnMove / ResetOnRotate は立てない
//	（記号 PIO と違って他の図形を参照しないため、動かしても嘘にならない）。
//
//	【描いたものの属性】各ソリッド・外形はレコードに保存した素材クラス（底盤・地中梁＝基礎
//	スラブ、立上り＝立ち上がり、床付け＝捨てコン／砕石の構成要素クラス）を付け、描画属性は
//	すべてクラスに従わせる。ソリッドには「断面ビューポートで構造用図形として扱う」
//	（ovIsStructural）を立て、同素材どうしの接ぎ目を断面で一体に見せる（M10 で実機確認した
//	手当て）。
//
//	【生成時にダイアログを出さない】PIO の既定は「作るたびに設定ダイアログを出す」。取り込みが
//	自動生成するので、OnInitXProperties で kCustomObjectPrefNever を宣言する（ExtColumnMark と
//	同じ。定義が作られる 1 回目には間に合わないので draw/Footing が DefineCustomObject を先に
//	呼ぶ）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtFoundation.h"
#include "draw/DrawUtil.h"

#include "core/Foundation.h"

#include "VWFC/VWObjects/VWParametricObj.h"

#include <string>
#include <vector>

namespace HomeskzIfcImport
{
	namespace
	{
		// PIO の定義。**関数ローカル static** で持つ理由は ExtMenu の menuDef と同じ
		// （SDK の非ローカル static を名前空間スコープの初期化子から参照しない）。
		// 点 PIO（1 点で挿入する）。移動・回転でリセットしない（ヘッダ冒頭「座標は PIO の
		// ローカル」）。
		const SParametricDef& parametricDef()
		{
			static const SParametricDef def = {/*LocalizedName*/ {PLUGIN_VWR_ID, "foundationName"},
											   /*SubType*/ kParametricSubType_Point,
											   /*ResetOnMove*/ false,
											   /*ResetOnRotate*/ false,
											   /*WallInsertOnEdge*/ false,
											   /*WallInsertNoBreak*/ false,
											   /*WallInsertHalfBreak*/ false,
											   /*WallInsertHideCaps*/ false};
			return def;
		}

		// パラメータ。寸法 6 つは**寸法欄**（kFieldCoordDisp。図面の単位で表示・編集される）、
		// 部品の直列化は文字列。既定値は 0／空＝「取り込みが書くまで何も描かない」。
		const SParametricParamDef* paramDefs()
		{
			static const SParametricParamDef defs[] = {
				{kParamSlabThickness,
				 {PLUGIN_VWR_ID, "foundationSlabThickness"},
				 "0",
				 "0",
				 kFieldCoordDisp,
				 0},
				{kParamSlabTop, {PLUGIN_VWR_ID, "foundationSlabTop"}, "0", "0", kFieldCoordDisp, 0},
				{kParamRiserTop,
				 {PLUGIN_VWR_ID, "foundationRiserTop"},
				 "0",
				 "0",
				 kFieldCoordDisp,
				 0},
				{kParamBeamDepth,
				 {PLUGIN_VWR_ID, "foundationBeamDepth"},
				 "0",
				 "0",
				 kFieldCoordDisp,
				 0},
				{kParamHaunchWidth,
				 {PLUGIN_VWR_ID, "foundationHaunchWidth"},
				 "0",
				 "0",
				 kFieldCoordDisp,
				 0},
				{kParamHaunchHeight,
				 {PLUGIN_VWR_ID, "foundationHaunchHeight"},
				 "0",
				 "0",
				 kFieldCoordDisp,
				 0},
				{kParamData, {PLUGIN_VWR_ID, "foundationData"}, "", "", kFieldText, 0},
				{"", {}, "", "", kFieldText, 0}}; // 終端
			return defs;
		}

		// PIO の寸法パラメータを読む（読めなければ 0）。
		double ParamReal(VWParametricObj& pio, const char* name)
		{
			try
			{
				return pio.GetParamReal(TXString(name));
			}
			catch (...)
			{
				return 0.0;
			}
		}

		// OIP の寸法 6 つを core::FoundationParams へ写す。
		core::FoundationParams ReadParams(VWParametricObj& pio)
		{
			core::FoundationParams params;
			params.slabThickness = ParamReal(pio, kParamSlabThickness);
			params.slabTop = ParamReal(pio, kParamSlabTop);
			params.riserTop = ParamReal(pio, kParamRiserTop);
			params.beamDepth = ParamReal(pio, kParamBeamDepth);
			params.haunchWidth = ParamReal(pio, kParamHaunchWidth);
			params.haunchHeight = ParamReal(pio, kParamHaunchHeight);
			return params;
		}

		// 描いた図形の属性を揃える（クラスと by-class 属性）。
		void Dress(MCObjectHandle object, const std::string& className)
		{
			draw::SetClassByName(object, className);
			draw::SetAllAttributesByClass(object);
		}
	} // namespace

	// --------------------------------------------------------------------------
	// - NOLINTBEGIN(misc-const-correctness)
#ifdef VW_DEV_BUILD
	// UUID: 70a64e00-315b-470f-ab51-224b58296846  (dev build)
	IMPLEMENT_VWParametricExtension(
		/*Extension class*/ CExtFoundation,
		/*Event sink*/ CFoundation_EventSink,
		/*Universal name*/ kFoundationUniversalName,
		/*Version*/ 1,
		/*UUID*/ 0x70a64e00, 0x315b, 0x470f, 0xab, 0x51, 0x22, 0x4b, 0x58, 0x29, 0x68, 0x46);
#else
	// UUID: bfb10a36-9663-47e4-8ee5-9fd63c41475e  (stable build)
	IMPLEMENT_VWParametricExtension(
		/*Extension class*/ CExtFoundation,
		/*Event sink*/ CFoundation_EventSink,
		/*Universal name*/ kFoundationUniversalName,
		/*Version*/ 1,
		/*UUID*/ 0xbfb10a36, 0x9663, 0x47e4, 0x8e, 0xe5, 0x9f, 0xd6, 0x3c, 0x41, 0x47, 0x5e);
#endif
	// NOLINTEND(misc-const-correctness)

	// ---------------------------------------------------------------------------
	CExtFoundation::CExtFoundation(CallBackPtr cbp)
		: VWExtensionParametric(cbp, parametricDef(), paramDefs())
	{
	}

	CExtFoundation::~CExtFoundation() = default;

	// ---------------------------------------------------------------------------
	CFoundation_EventSink::CFoundation_EventSink(IVWUnknown* parent)
		: VWParametric_EventSink(parent)
	{
	}

	CFoundation_EventSink::~CFoundation_EventSink() = default;

	// ---------------------------------------------------------------------------
	EObjectEvent CFoundation_EventSink::OnInitXProperties(CodeRefID objectID)
	{
		const EObjectEvent result = VWParametric_EventSink::OnInitXProperties(objectID);

		// 生成のたびに「オブジェクトの設定」ダイアログを出さない（意図はヘッダ参照）。
		// 値は Sint8 なので SetObjectPropertyChar を使う（Extensions/ExtColumnMark と同じ）。
		gSDK->SetObjectPropertyChar(objectID, kObjXPropShowPrefDialogWhen,
									static_cast<unsigned char>(kCustomObjectPrefNever));
		return result;
	}

	EObjectEvent CFoundation_EventSink::Recalculate()
	{
		// 自分自身のハンドルは基底の protected メンバ（VWFC が Execute で詰める）。
		if (this->fhObject == nil)
			return kObjectEventNoErr;

		try
		{
			VWParametricObj self(this->fhObject);

			// 1. レコードの部品。無い／読めない（別の版・壊れている）なら何も描かない
			//    （空の PIO のまま。取り込みは書けたかを読み戻して診断に出している）。
			core::FoundationCommand imported;
			const std::string data = self.GetParamString(TXString(kParamData)).GetStdString();
			if (!core::decodeFoundation(data, imported))
				return kObjectEventNoErr;

			// 2. OIP の寸法を部品へ配る。
			const core::FoundationCommand current =
				core::applyFoundationParams(imported, ReadParams(self));

			// 3a. 平面の外形（2D/平面ビューで見せる図。ハイブリッドの 2D 側）。
			for (const core::FoundationPlanShape& shape : core::foundationPlanShapes(current))
			{
				const MCObjectHandle polygon = draw::CreateClosedPolygon(shape.outline);
				if (polygon == nil)
					continue;
				Dress(polygon, shape.drawClass);
			}

			// 3b. ソリッド（3D 側）。同素材の接ぎ目は構造用図形として断面で一体になる。
			for (const core::FoundationSolid& solid : core::foundationSolids(current))
			{
				const MCObjectHandle handle = draw::CreateExtrudedSolid(solid.base, solid.extent);
				if (handle == nil)
					continue;
				Dress(handle, solid.drawClass);
				draw::SetBooleanVariable(handle, ovIsStructural, true);
			}
		}
		catch (...)
		{
			// 1 つの異常で基礎全体を落とさない（CLAUDE.md「エラーハンドリング」）。
			// kObjectEventHadError を返すと VW がオブジェクトをエラー表示にするので、
			// ここまでに描けたものを残したまま正常終了として抜ける。
			return kObjectEventNoErr;
		}
		return kObjectEventNoErr;
	}
} // namespace HomeskzIfcImport
