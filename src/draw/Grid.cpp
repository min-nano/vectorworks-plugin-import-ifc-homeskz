//
//	draw/Grid.cpp
//
//	通り芯描画の実装。Python 版 vw/grid.py に対応する。命令セット（GridCommand）を
//	GridAxis オブジェクトとして配置する。【SDK 依存】PluginPrefix.h（VectorWorks SDK）
//	を include するため、この翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、
//	無 SDK の core/parse ライブラリには入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	使用する SDK API はすべて ISDK（gSDK）／VWFC の実在シグネチャに合わせている
//	（Vectorworks 2026 SDK の Interfaces/VectorWorks/ISDK.h・VWFC/VWObjects）:
//	  * gSDK->GetNamedLayer / CreateLayer / SetCurrentLayer … レイヤの取得・作成・アクティブ化
//	  * gSDK->CreateLine(WorldPt,WorldPt)                  … パスとなる直線
//	  * gSDK->CreateCustomObjectPath(name,path,prof,regen) … パスから PIO を生成
//	  * gSDK->AddClass(name)->InternalIndex / SetObjectClass … クラス分け
//	  * VWParametricObj(h).SetParamString(name,value)      … PIO パラメータ
//
//	実描画（位置・クラス分け・軸名ラベル・基点バブル）はローカルの VectorWorks で
//	目視確認する（ROADMAP.md M1「ローカル確認」）。特に GridAxis PIO のパラメータ
//	ユニバーサル名（Label / ShowBubbleAt）と選択肢値（"Start Point"）、座標単位は
//	VW 実機でのみ最終確認できる（誤りは実行時に無効化されるだけでビルドは通る）。
//

#include "PluginPrefix.h"
#include "draw/Grid.h"
#include "core/Document.h"

// GridAxis PIO のパラメータ設定に使う VWFC ラッパー（PluginPrefix の umbrella が
// 取り込む VWFC::VWObjects の一部。明示 include で可用性を確実にする）。
#include "VWFC/VWObjects/VWParametricObj.h"

#include <cstddef>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// デザインレイヤの種別コード（CreateLayer の layerType 引数）。1 = デザインレイヤ、
		// 2 = シート（プレゼンテーション）レイヤ。通り芯はデザインレイヤに置く。
		constexpr short kDesignLayerType = 1;

		// core::Vec2（平面座標・mm）→ SDK の WorldPt へ。core と SDK の幾何型を
		// つなぐ変換はここ 1 か所に集約する（CLAUDE.md「変換規約を分散させない」）。
		// ホームズ君 IFC の座標は mm。VW ドキュメントも mm 前提で、そのまま渡す。
		WorldPt ToWorldPt(const core::Vec2& p)
		{
			return WorldPt(p.x, p.y);
		}

		// 「共通」デザインレイヤを取得（無ければ作成）し、アクティブにする。以後に生成する
		// オブジェクトはこのアクティブレイヤへ入る。取得・生成できなければ何もしない。
		void PrepareLayer(const TXString& layerName)
		{
			MCObjectHandle layer = gSDK->GetNamedLayer(layerName);
			if (layer == nil)
				layer = gSDK->CreateLayer(layerName, kDesignLayerType);
			if (layer != nil)
				gSDK->SetCurrentLayer(layer);
		}

		// オブジェクトのクラスを名前で設定する（AddClass は既存なら索引を返し、無ければ作る）。
		// Python 版 vw/grid.py の vs.SetClass(handle, class) に対応する。
		void SetClassByName(MCObjectHandle object, const std::string& className)
		{
			if (className.empty())
				return;
			const InternalIndex classID = gSDK->AddClass(TXString(className.c_str()));
			gSDK->SetObjectClass(object, classID);
		}

		// 1 本の通り芯を描く（Python 版 vw/grid.py draw_grid に対応）。始点→終点の直線を
		// パスに GridAxis PIO を生成し、クラス・軸名ラベル・基点バブルを設定して再計算する。
		// PIO 生成に失敗したら通常の直線へフォールバックする。何か 1 つでも配置できたら true。
		bool DrawOne(const core::GridCommand& grid)
		{
			const WorldPt start = ToWorldPt(grid.start);
			const WorldPt end = ToWorldPt(grid.end);

			// パスとなる直線をアクティブレイヤに引く。パスに Z 成分は持たせない（平面）。
			MCObjectHandle path = gSDK->CreateLine(start, end);
			if (path == nil)
				return false;

			// パスから GridAxis のカスタムオブジェクト（PIO）を生成する。第 3 引数の
			// プロファイルグループは nil。Python 版は空グループを渡すが、GridAxis は
			// 断面を押し出す種の PIO ではなく（2D の通り芯＋バブル）、空グループと nil は
			// 等価に働く（ローカル確認対象）。第 4 引数は生成後に再計算するか（true）。
			const TXString kGridAxis("GridAxis");
			MCObjectHandle object = gSDK->CreateCustomObjectPath(kGridAxis, path, nil, true);

			if (object != nil)
			{
				// クラス分け（X 通り／Y 通り）。
				SetClassByName(object, grid.drawClass);
				// 軸名ラベルと基点バブルをパラメータで設定する（Python 版の
				// SetRField(handle,'GridAxis','Label'/'ShowBubbleAt',…) に対応。PIO の
				// ユニバーサルパラメータ名でアクセスする）。設定後に再計算して反映する。
				VWParametricObj pio(object);
				if (!grid.label.empty())
					pio.SetParamString("Label", TXString(grid.label.c_str()));
				pio.SetParamString("ShowBubbleAt", "Start Point");
				gSDK->ResetObject(object);
			}
			else
			{
				// フォールバック: 'GridAxis' PIO が無い等で生成に失敗したら通常の直線を
				// 引く。Python 版と同じく新規に引く（パスは CreateCustomObjectPath に
				// 取り込まれ得るため再利用しない）。
				MCObjectHandle fallback = gSDK->CreateLine(start, end);
				if (fallback != nil)
					SetClassByName(fallback, grid.drawClass);
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
