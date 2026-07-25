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

		// 1 本の通り芯を描く。始点→終点の直線を引き、それをパスに GridAxis PIO を
		// 生成する。PIO 生成に失敗したら直線のまま残してフォールバックする。生成できた
		// オブジェクト（PIO か直線）へクラス・軸名・基点バブルを設定する。
		// 何か 1 つでも配置できたら true。
		bool DrawOne(const core::GridCommand& grid)
		{
			// パスとなる直線をアクティブレイヤに引く。パスに Z 成分は持たせない（平面）。
			MCObjectHandle path = gSDK->CreateLine(ToWorldPt(grid.start), ToWorldPt(grid.end));
			if (path == nil)
				return false;

			// パスから GridAxis のカスタムオブジェクト（PIO）を生成する。第 3 引数は
			// プロファイルグループ（無し=nil）、第 4 引数は生成後に再計算するか（true）。
			// 'GridAxis' PIO が無い等で失敗（nil）したら、引いた直線をフォールバックにする。
			const TXString kGridAxis("GridAxis");
			MCObjectHandle object = gSDK->CreateCustomObjectPath(kGridAxis, path, nil, true);
			const bool isPio = (object != nil);
			if (!isPio)
				object = path; // フォールバック: 直線を残す

			// クラス分け（X 通り／Y 通り）。AddClass は既存なら索引を返し、無ければ作る。
			if (!grid.drawClass.empty())
			{
				const InternalIndex classID = gSDK->AddClass(TXString(grid.drawClass.c_str()));
				gSDK->SetObjectClass(object, classID);
			}

			// GridAxis PIO のときは軸名ラベルと基点バブルをパラメータで設定する。
			// パラメータ名・値は組み込み GridAxis PIO の仕様（ローカル確認対象。誤りでも
			// 実行時に無視されるだけでビルドは通る。ROADMAP.md M1）。
			if (isPio)
			{
				VWParametricObj pio(object);
				if (!grid.label.empty())
					pio.SetParamString("Label", TXString(grid.label.c_str()));
				pio.SetParamString("ShowBubbleAt", "Start Point");
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
