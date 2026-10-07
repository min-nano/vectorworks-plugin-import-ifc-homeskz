//
//	draw/ColumnMarkPio.cpp
//
//	柱・小屋束の記号 PIO のリセット本体（意図は draw/ColumnMarkPio.h）。
//	**Extensions/ExtColumnMark.cpp から本体（ペイロード）側へ移したもの**で、中身は移設前と
//	同じ。リセットのたびに、
//	  1. パラメータの対象レイヤを名前で引き、
//	  2. そのレイヤの構造材（StructuralMember）を走査して構造用途 4/5 だけを採り、
//	  3. 1 本ごとに記号を描画する（断面＝実断面の対角線／平面＝シンボル 1 つ）
//	という流れで作図する。**記号の大きさ・位置・本数は毎回実物から導く**ので、柱を編集
//	しても（リセットが走れば）記号が実物と食い違わない。
//
//	使用する SDK API は ci-debug の sdk-grep で実在を確認したもの:
//	  gSDK->GetNamedLayer / FirstMemberObj / NextObject / GetObjectBounds /
//	  ClassNameToID / GetObjectClass / CreateLine、VWSymbolObj、
//	  VWParametricObj（パラメータの読み）。
//	実際の見え方（線の太さ・シンボルの向き・リセットの契機）はローカルの VectorWorks で
//	目視確認する（docs/DEV-NOTES.md M12）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "draw/ColumnMarkPio.h"
#include "Extensions/ExtColumnMark.h"
#include "draw/DrawUtil.h"
#include "draw/StructuralMember.h"

#include "core/Document.h"

#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWSymbolObj.h"

#include <string>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 対象オブジェクトが構造材で、構造用途が柱／小屋束なら true。併せて断面寸法も返す。
		//
		// **寸法は draw/DrawUtil の ResolveParamName を通して読む。**
		// 理由: 構造材ツールのパラメータは universal 名で取得できないことがあり（日本語環境。
		// M6 の垂木で実証済み）、draw/StructuralMember は書くときに同じ解決でローカライズ名へ
		// フォールバックする。読む側が universal 名だけを参照すると、書けているのに 0 が返って
		// 全部の柱が除外される。
		bool ColumnSection(MCObjectHandle object, bool& outKoyazuka, double& outWidth,
						   double& outDepth)
		{
			try
			{
				const VWParametricObj pio(object);
				const std::string use = draw::PioParamString(pio, draw::kFieldStructuralUse);
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

		// 記号 1 つ分を描画する。断面記号は実断面の対角線（柱＝×・小屋束＝／）、平面記号は
		// シンボル 1 つ。作成した図形は PIO（host）のジオメトリとして取り込まれる。
		//
		// 【シンボルは置いただけでは図面に現れない】シンボルは AddObjectToContainer で PIO
		// 自身（host）へ入れる。**AddObjectToContainer を省くと警告なしに配置されなくなる**
		// （draw/Symbol.cpp 冒頭）。線（CreateLine）は自動で入るので、この処置が要るのは
		// シンボルだけ。
		// 理由: VWSymbolObj の構築子はレガシーの PlaceSymbol を包んでおり、できたインスタンスは
		// どのコンテナにも入らない——M11 のアンカーボルトで「シンボルがひとつも配置できない」
		// ところから切り分けた落とし穴。
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
			// ／（左下→右上）。小屋束はこの 1 本だけ、柱はもう 1 本追加して ×。
			gSDK->CreateLine(WorldPt(centre.x - halfW, centre.y - halfD),
							 WorldPt(centre.x + halfW, centre.y + halfD));
			if (!koyazuka)
				gSDK->CreateLine(WorldPt(centre.x + halfW, centre.y - halfD),
								 WorldPt(centre.x - halfW, centre.y + halfD));
		}
	} // namespace

	// -------------------------------------------------------------------
	EObjectEvent recalculateColumnMark(MCObjectHandle object)
	{
		// リセット以外の経路で空のまま呼ばれても落とさないよう nil を確認しておく。
		if (object == nil)
			return kObjectEventNoErr;

		try
		{
			const VWParametricObj self(object);
			const std::string targetLayer = draw::PioParamString(self, kParamTargetLayer);
			if (targetLayer.empty())
				return kObjectEventNoErr; // 対象未指定なら何も描画しない

			const std::string style = draw::PioParamString(self, kParamMarkStyle);
			const bool plan = style == kMarkStylePlan;
			const TXString symbol(draw::PioParamString(self, kParamMarkSymbol).c_str());
			const std::string targetClass = draw::PioParamString(self, kParamTargetClass);

			const MCObjectHandle layer = gSDK->GetNamedLayer(TXString(targetLayer.c_str()));
			if (layer == nil)
				return kObjectEventNoErr; // レイヤが無い＝その階が生成されていない

			// **PIO のジオメトリは PIO 自身のローカル座標で持たれる。** 柱はワールド座標で
			// 見つかるので、描画する前に必ずローカル座標へ変換する。InversePointTransform
			// は回転も含めて戻すので、PIO を回転しても記号は柱の上に残る。
			// これをしないと PIO を動かした量だけ記号がまるごとずれ、しかもリセットしても同じ
			// 相対位置に再描画するので解消しない（実機で確認。ResetOnMove を立てただけでは
			// 解決しない）。
			VWTransformMatrix toWorld;
			self.GetObjectToWorldTransform(toWorld);

			for (MCObjectHandle h = gSDK->FirstMemberObj(layer); h != nil; h = gSDK->NextObject(h))
			{
				// クラスで絞る指定があれば、それ以外はスキップする（空＝全クラス）。
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
				// 求まるのはワールド座標なので、PIO のローカル座標へ変換してから描画する（上記）。
				WorldRect bounds;
				gSDK->GetObjectBounds(h, bounds);
				const VWPoint2D centre = toWorld.InversePointTransform(VWPoint2D(
					(bounds.left + bounds.right) / 2.0, (bounds.top + bounds.bottom) / 2.0));

				DrawMark(object, plan, symbol, koyazuka, WorldPt(centre.x, centre.y), width, depth);
			}
		}
		catch (...)
		{
			// 1 本の異常で記号全体を失敗させない（CLAUDE.md「エラーハンドリング」）。
			// kObjectEventHadError を返すと VW がオブジェクトをエラー表示にするので、
			// ここまでに描画できた記号を残したまま正常終了として抜ける。
			return kObjectEventNoErr;
		}
		return kObjectEventNoErr;
	}
} // namespace HomeskzIfcImport::draw
