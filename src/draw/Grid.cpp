//
//	draw/Grid.cpp
//
//	通り芯描画の実装。Python 版 vw/grid.py に対応する。命令セット（GridCommand）を
//	GridAxis オブジェクトとして配置する。【SDK 依存】PluginPrefix.h（VectorWorks SDK）
//	を include するため、この翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、
//	無 SDK の core/parse ライブラリには入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	実描画（位置・クラス分け・軸名ラベル・基点バブル）はローカルの VectorWorks で
//	目視確認する（ROADMAP.md M1「ローカル確認」）。SDK 側の GridAxis PIO のフィールド名
//	（Label / ShowBubbleAt）や座標単位は VW 上での確認を要する。
//

#include "PluginPrefix.h"
#include "draw/Grid.h"
#include "core/Document.h"

#include <cstddef>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// core::Vec2（平面座標・mm）→ SDK の WorldPt へ。core と SDK の幾何型を
		// つなぐ変換はここ 1 か所に集約する（CLAUDE.md「変換規約を分散させない」）。
		// ホームズ君 IFC の座標は mm。VW ドキュメントも mm 前提で、そのまま渡す。
		WorldPt ToWorldPt(const core::Vec2& p)
		{
			return WorldPt(p.x, p.y);
		}

		// 「共通」デザインレイヤを作成し（無ければ）アクティブにする。以後に生成する
		// オブジェクトはこのレイヤへ入る。既にあれば取得してアクティブ化するだけ。
		// 返り値はレイヤハンドル（取得・生成できなければ nil）。
		MCObjectHandle PrepareLayer(const TXString& layerName)
		{
			// 既存の同名オブジェクト（レイヤ）を探し、無ければデザインレイヤを作る。
			MCObjectHandle layer = gSDK->GetObject(layerName);
			if (layer == nil)
				layer = gSDK->CreateLayer(layerName, 1 /* 1 = デザインレイヤ */);
			if (layer != nil)
				gSDK->SetActiveLayer(layer, false /* リセットせずアクティブ化 */);
			return layer;
		}

		// 1 本の通り芯を描く。始点→終点の直線を引き、それをパスに GridAxis PIO を
		// 生成する。PIO 生成に失敗したら直線のまま残してフォールバックする。生成できた
		// オブジェクト（PIO か直線）へクラス・軸名・基点バブルを設定する。
		// 何か 1 つでも配置できたら true。
		bool DrawOne(const core::GridCommand& grid)
		{
			// パスとなる直線を現在のレイヤに引く。MoveTo/LineTo が現在オブジェクトを
			// 作り、LNewObj でそのハンドルを得る。パスに Z 成分は持たせない（平面）。
			const WorldPt start = ToWorldPt(grid.start);
			const WorldPt end = ToWorldPt(grid.end);
			gSDK->MoveTo(start.x, start.y);
			gSDK->LineTo(end.x, end.y);
			MCObjectHandle path = gSDK->LNewObj();
			if (path == nil)
				return false;

			// パスを使って GridAxis のカスタムオブジェクトを生成する。成功すると PIO の
			// ハンドルが返り、パスは PIO に取り込まれる。'GridAxis' PIO が無い等で失敗
			// （nil）したら、引いた直線をそのままフォールバックとして使う。
			const TXString kGridAxis("GridAxis");
			MCObjectHandle object = gSDK->CreateCustomObjectPath(kGridAxis, path, nil, nil);
			const bool isPio = (object != nil);
			if (!isPio)
				object = path; // フォールバック: 直線を残す

			// クラス分け（X 通り／Y 通り）。SetClass は無ければクラスを作る。
			if (!grid.drawClass.empty())
				gSDK->SetClass(object, TXString(grid.drawClass.c_str()));

			// GridAxis PIO のときは軸名ラベルと基点バブルを設定する。フィールド名は
			// 組み込み GridAxis PIO のパラメータ（ローカル確認対象。ROADMAP.md M1）。
			if (isPio)
			{
				if (!grid.label.empty())
					gSDK->SetRField(object, kGridAxis, TXString("Label"),
									TXString(grid.label.c_str()));
				gSDK->SetRField(object, kGridAxis, TXString("ShowBubbleAt"),
								TXString("Start Point"));
				gSDK->ResetObject(object); // パラメータ変更を PIO に反映
			}
			return true;
		}
	} // namespace

	std::size_t drawGrids(const core::Document& document)
	{
		if (document.grids.empty())
			return 0;

		// 通り芯はすべて「共通」レイヤ（全命令で同一）。最初の命令のレイヤ名で用意する。
		PrepareLayer(TXString(document.grids.front().layer.c_str()));

		std::size_t drawn = 0;
		for (const core::GridCommand& grid : document.grids)
		{
			if (DrawOne(grid))
				++drawn;
		}
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
