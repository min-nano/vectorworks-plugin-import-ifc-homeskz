//
//	Extensions/ExtColumnMark.cpp
//
//	柱・小屋束の記号 PIO の実装（意図は ExtColumnMark.h 参照）。
//
//	リセット（Recalculate）のたびに、
//	  1. パラメータの対象レイヤを名前で引き、
//	  2. そのレイヤの構造材（StructuralMember）を走査して構造用途 4/5 だけを採り、
//	  3. 1 本ごとに記号を描く（断面＝実断面の対角線／平面＝シンボル 1 つ）
//	という流れで作図する。**記号の大きさ・位置・本数は毎回実物から導く**ので、柱を
//	編集しても（リセットが走れば）記号が嘘にならない。
//
//	使用する SDK API は ci-debug の sdk-grep で実在を確認したもの:
//	  gSDK->GetNamedLayer / GetFirstMemberObject / NextObject / GetObjectBounds /
//	  CreateLine、VWSymbolObj、VWParametricObj（パラメータの読み）。
//	実際の見え方（線の太さ・シンボルの向き・リセットの契機）はローカルの VectorWorks で
//	目視確認する（ROADMAP.md M12「ローカル確認」）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtColumnMark.h"
#include "draw/DrawUtil.h"

#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWSymbolObj.h"

#include <string>

namespace HomeskzIfcImport
{
	namespace
	{
		// 構造材ツールのフィールド名（draw/StructuralMember と同じ登録名）。記号は
		// 構造用途で柱／小屋束を見分け、断面は幅・せいから描く。
		constexpr const char* kFieldStructuralUse = "StructuralUse";
		constexpr const char* kFieldMajorBreadth = "MajorBreadth";
		constexpr const char* kFieldMajorDepth = "MajorDepth";
		constexpr const char* kUseColumn = "4";	  // 柱（管柱・通し柱）→ ×
		constexpr const char* kUseKoyazuka = "5"; // 小屋束 → ／

		// PIO の定義。**関数ローカル static** で持つ理由は ExtMenu の menuDef と同じ
		// （SDK の非ローカル static を名前空間スコープの初期化子から参照しない）。
		// 点 PIO（シンボルのように 1 点で挿入する）で、移動・回転でリセットはしない
		// ——記号の中身は対象レイヤの柱から決まり、PIO 自身の位置には依存しないため。
		const SParametricDef& parametricDef()
		{
			static const SParametricDef def = {/*LocalizedName*/ {PLUGIN_VWR_ID, "columnMarkName"},
											   /*SubType*/ kParametricSubType_Point,
											   /*ResetOnMove*/ false,
											   /*ResetOnRotate*/ false,
											   /*WallInsertOnEdge*/ false,
											   /*WallInsertNoBreak*/ false,
											   /*WallInsertHalfBreak*/ false,
											   /*WallInsertHideCaps*/ false};
			return def;
		}

		// パラメータ（すべて文字列）。既定値は空＝「対象レイヤ未指定なら何も描かない」。
		const SParametricParamDef* paramDefs()
		{
			static const SParametricParamDef defs[] = {
				{kParamTargetLayer,
				 {PLUGIN_VWR_ID, "columnMarkTargetLayer"},
				 "",
				 "",
				 kFieldText,
				 0},
				{kParamTargetClass,
				 {PLUGIN_VWR_ID, "columnMarkTargetClass"},
				 "",
				 "",
				 kFieldText,
				 0},
				{kParamMarkStyle,
				 {PLUGIN_VWR_ID, "columnMarkStyle"},
				 kMarkStyleSection,
				 kMarkStyleSection,
				 kFieldText,
				 0},
				{kParamMarkSymbol, {PLUGIN_VWR_ID, "columnMarkSymbol"}, "", "", kFieldText, 0},
				{"", {}, "", "", kFieldText, 0}}; // 終端
			return defs;
		}

		// PIO のパラメータを文字列で読む（無ければ空）。
		std::string ParamString(VWParametricObj& pio, const char* name)
		{
			try
			{
				return pio.GetParamString(TXString(name)).GetStdString();
			}
			catch (...)
			{
				return {};
			}
		}

		// 対象オブジェクトが構造材で、構造用途が柱／小屋束なら true。併せて断面寸法も返す。
		bool ColumnSection(MCObjectHandle object, bool& outKoyazuka, double& outWidth,
						   double& outDepth)
		{
			try
			{
				VWParametricObj pio(object);
				const std::string use = ParamString(pio, kFieldStructuralUse);
				if (use != kUseColumn && use != kUseKoyazuka)
					return false;
				outKoyazuka = use == kUseKoyazuka;
				outWidth = pio.GetParamReal(TXString(kFieldMajorBreadth));
				outDepth = pio.GetParamReal(TXString(kFieldMajorDepth));
				return outWidth > 0.0 && outDepth > 0.0;
			}
			catch (...)
			{
				return false;
			}
		}

		// 記号 1 つ分を描く。断面記号は実断面の対角線（柱＝×・小屋束＝／）、平面記号は
		// シンボル 1 つ。作った図形は PIO のジオメトリとして取り込まれる。
		void DrawMark(bool plan, const TXString& symbol, bool koyazuka, const WorldPt& centre,
					  double width, double depth)
		{
			if (plan)
			{
				if (!symbol.IsEmpty())
					const VWSymbolObj instance(symbol, VWPoint2D(centre.x, centre.y), 0.0);
				return;
			}

			const double halfW = width / 2.0;
			const double halfD = depth / 2.0;
			// ／（左下→右上）。小屋束はこの 1 本だけ、柱はもう 1 本足して ×。
			gSDK->CreateLine(WorldPt(centre.x - halfW, centre.y - halfD),
							 WorldPt(centre.x + halfW, centre.y + halfD));
			if (!koyazuka)
				gSDK->CreateLine(WorldPt(centre.x + halfW, centre.y - halfD),
								 WorldPt(centre.x - halfW, centre.y + halfD));
		}
	} // namespace

	// ---------------------------------------------------------------------------
	// NOLINTBEGIN(misc-const-correctness)
#ifdef VW_DEV_BUILD
	// UUID: 5c1f0a76-2d4e-4b93-9a11-7e6c8d240f31  (dev build)
	IMPLEMENT_VWParametricExtension(
		/*Extension class*/ CExtColumnMark,
		/*Event sink*/ CColumnMark_EventSink,
		/*Universal name*/ kColumnMarkUniversalName,
		/*Version*/ 1,
		/*UUID*/ 0x5c1f0a76, 0x2d4e, 0x4b93, 0x9a, 0x11, 0x7e, 0x6c, 0x8d, 0x24, 0x0f, 0x31);
#else
	// UUID: 8b3d29e4-61af-4c07-85d2-3f9b1c6a7e58  (stable build)
	IMPLEMENT_VWParametricExtension(
		/*Extension class*/ CExtColumnMark,
		/*Event sink*/ CColumnMark_EventSink,
		/*Universal name*/ kColumnMarkUniversalName,
		/*Version*/ 1,
		/*UUID*/ 0x8b3d29e4, 0x61af, 0x4c07, 0x85, 0xd2, 0x3f, 0x9b, 0x1c, 0x6a, 0x7e, 0x58);
#endif
	// NOLINTEND(misc-const-correctness)

	// ---------------------------------------------------------------------------
	CExtColumnMark::CExtColumnMark(CallBackPtr cbp)
		: VWExtensionParametric(cbp, parametricDef(), paramDefs())
	{
	}

	CExtColumnMark::~CExtColumnMark() = default;

	// ---------------------------------------------------------------------------
	CColumnMark_EventSink::CColumnMark_EventSink(IVWUnknown* parent)
		: VWParametric_EventSink(parent)
	{
	}

	CColumnMark_EventSink::~CColumnMark_EventSink() = default;

	EObjectEvent CColumnMark_EventSink::Recalculate()
	{
		try
		{
			VWParametricObj self(this->GetObject());
			const std::string targetLayer = ParamString(self, kParamTargetLayer);
			if (targetLayer.empty())
				return kObjectEventNoChange; // 対象未指定なら何も描かない

			const std::string style = ParamString(self, kParamMarkStyle);
			const bool plan = style == kMarkStylePlan;
			const TXString symbol(ParamString(self, kParamMarkSymbol).c_str());
			const std::string targetClass = ParamString(self, kParamTargetClass);

			const MCObjectHandle layer = gSDK->GetNamedLayer(TXString(targetLayer.c_str()));
			if (layer == nil)
				return kObjectEventNoChange; // レイヤが無い＝その階が生成されていない

			for (MCObjectHandle h = gSDK->GetFirstMemberObject(layer); h != nil;
				 h = gSDK->NextObject(h))
			{
				// クラスで絞る指定があれば、それ以外は飛ばす（空＝全クラス）。
				if (!targetClass.empty())
				{
					const InternalIndex wanted = gSDK->ClassNameToID(TXString(targetClass.c_str()));
					if (wanted == 0 || gSDK->GetObjectClass(h) != wanted)
						continue;
				}

				bool koyazuka = false;
				double width = 0.0;
				double depth = 0.0;
				if (!ColumnSection(h, koyazuka, width, depth))
					continue;

				// 位置は柱のバウンディングボックスの中心（鉛直材なので平面の中心＝柱心）。
				WorldRect bounds;
				gSDK->GetObjectBounds(h, bounds);
				const WorldPt centre((bounds.left + bounds.right) / 2.0,
									 (bounds.top + bounds.bottom) / 2.0);

				DrawMark(plan, symbol, koyazuka, centre, width, depth);
			}
		}
		catch (...)
		{
			// 1 本の異常で記号全体を落とさない（CLAUDE.md「エラーハンドリング」）。
			return kObjectEventNoChange;
		}
		return kObjectEventNoChange;
	}
} // namespace HomeskzIfcImport
