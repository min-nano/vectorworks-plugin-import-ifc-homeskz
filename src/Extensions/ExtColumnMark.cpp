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
//	  gSDK->GetNamedLayer / FirstMemberObj / NextObject / GetObjectBounds /
//	  ClassNameToID / GetObjectClass / CreateLine、VWSymbolObj、
//	  VWParametricObj（パラメータの読み）。自分自身のハンドルは基底
//	  VWParametric_EventSink の protected メンバ fhObject から取る
//	  （VWFC/PluginSupport/VWExtensionParametric.h:173。IParametricEventSink には
//	  ハンドルを返す公開 API が無く、Recalculate() も引数を取らないため、VWFC が
//	  Execute で詰めるこのメンバが唯一の入口）。
//	実際の見え方（線の太さ・シンボルの向き・リセットの契機）はローカルの VectorWorks で
//	目視確認する（ROADMAP.md M12「ローカル確認」）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtColumnMark.h"
#include "draw/DrawUtil.h"
#include "draw/StructuralMember.h"

#include "core/Document.h"

#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWSymbolObj.h"

#include <string>

namespace HomeskzIfcImport
{
	namespace
	{
		// 記号が読む構造材ツールのフィールド名（draw::kField*・draw::kLocalized*）と
		// 構造用途の値（core::kStructuralUse*）は**定義を共有する**。ここで綴りを書き
		// 写すと、書き手側で名前や値を変えたときに記号だけが黙って何も描かなくなる。

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
		//
		// **寸法は draw/DrawUtil の ResolveParamName を通して読む。** 構造材ツールの
		// パラメータは universal 名で引けないことがあり（日本語環境。M6 の垂木で実証済み）、
		// draw/StructuralMember は書くときに同じ解決でローカライズ名へ落ちる。読み手が
		// universal 名だけを見ると、書けているのに 0 が返って全部の柱が弾かれる。
		bool ColumnSection(MCObjectHandle object, bool& outKoyazuka, double& outWidth,
						   double& outDepth)
		{
			try
			{
				VWParametricObj pio(object);
				const std::string use = ParamString(pio, draw::kFieldStructuralUse);
				if (use != core::kStructuralUseColumn && use != core::kStructuralUseKoyazuka)
					return false;
				outKoyazuka = use == core::kStructuralUseKoyazuka;
				outWidth = pio.GetParamReal(
					draw::ResolveParamName(pio, draw::kFieldMajorBreadth, draw::kLocalizedBreadth));
				outDepth = pio.GetParamReal(
					draw::ResolveParamName(pio, draw::kFieldMajorDepth, draw::kLocalizedDepth));
				return outWidth > 0.0 && outDepth > 0.0;
			}
			catch (...)
			{
				return false;
			}
		}

		// 記号 1 つ分を描く。断面記号は実断面の対角線（柱＝×・小屋束＝／）、平面記号は
		// シンボル 1 つ。作った図形は PIO（host）のジオメトリとして取り込まれる。
		//
		// 【シンボルは置いただけでは図面に現れない】VWSymbolObj の構築子はレガシーの
		// PlaceSymbol を包んでおり、できたインスタンスはどのコンテナにも入らない——
		// M11 のアンカーボルトで「シンボルがひとつも配置できない」ところから切り分けた
		// 落とし穴で、**AddObjectToContainer を外すと静かに壊れる**（draw/Symbol.cpp 冒頭）。
		// PIO の中では配置先が PIO 自身（host）になる。線（CreateLine）は自動で入るので
		// この手当てが要るのはシンボルだけ。
		void DrawMark(MCObjectHandle host, bool plan, const TXString& symbol, bool koyazuka,
					  const WorldPt& centre, double width, double depth)
		{
			if (plan)
			{
				if (symbol.IsEmpty())
					return;
				const VWSymbolObj instance(symbol, VWPoint2D(centre.x, centre.y), 0.0);
				const MCObjectHandle handle = instance.GetThisObject();
				// 非 nil を成功判定にしない（定義が無くても空のハンドルが返る。
				// draw/Symbol.cpp の 2 番目の作法）。
				if (handle == nil || !VWSymbolObj::IsSymbolObject(handle, symbol))
					return;
				gSDK->AddObjectToContainer(handle, host);
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
		// 自分自身のハンドルは基底の protected メンバ（VWFC が Execute で詰める）。
		// リセット以外の経路で空のまま呼ばれても落とさないよう nil を見ておく。
		if (this->fhObject == nil)
			return kObjectEventNoErr;

		try
		{
			VWParametricObj self(this->fhObject);
			const std::string targetLayer = ParamString(self, kParamTargetLayer);
			if (targetLayer.empty())
				return kObjectEventNoErr; // 対象未指定なら何も描かない

			const std::string style = ParamString(self, kParamMarkStyle);
			const bool plan = style == kMarkStylePlan;
			const TXString symbol(ParamString(self, kParamMarkSymbol).c_str());
			const std::string targetClass = ParamString(self, kParamTargetClass);

			const MCObjectHandle layer = gSDK->GetNamedLayer(TXString(targetLayer.c_str()));
			if (layer == nil)
				return kObjectEventNoErr; // レイヤが無い＝その階が生成されていない

			for (MCObjectHandle h = gSDK->FirstMemberObj(layer); h != nil; h = gSDK->NextObject(h))
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

				DrawMark(this->fhObject, plan, symbol, koyazuka, centre, width, depth);
			}
		}
		catch (...)
		{
			// 1 本の異常で記号全体を落とさない（CLAUDE.md「エラーハンドリング」）。
			// kObjectEventHadError を返すと VW がオブジェクトをエラー表示にするので、
			// ここまでに描けた記号を残したまま正常終了として抜ける。
			return kObjectEventNoErr;
		}
		return kObjectEventNoErr;
	}
} // namespace HomeskzIfcImport
