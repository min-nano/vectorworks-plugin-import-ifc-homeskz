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
//	  * VWPolygon2DObj({p0,p1}) + SetClosed(false)         … パスとなる開いた 2D ポリライン
//	  * VWGroupObj()                                       … 空のプロファイルグループ
//	  * gSDK->CreateCustomObjectPath(name,path,prof,regen) … パス＋プロファイルから PIO を生成
//	  * gSDK->AddClass(name)->InternalIndex / SetObjectClass … クラス分け
//	  * VWParametricObj(h).SetParamString(name,value)      … PIO パラメータ（Label / 基点バブル）
//	  * gSDK->ResetObject(h)                               … パラメータ変更の反映
//
//	Python 版 vw/grid.py（draw_grid）に忠実に写している: BeginPoly/MoveTo/LineTo/EndPoly の
//	開いたポリラインを**パス**に、空グループ（BeginGroup/EndGroup）を**プロファイル**に
//	渡して CreateCustomObjectPath('GridAxis', path, profile) で生成し、SetClass・
//	Label・ShowBubbleAt='Start Point' を設定して ResetObject で反映する。
//
//	【設計上の要点】GridAxis PIO の線の向き・端点・基点バブルの位置は、渡した**パスの
//	頂点**から決まる。したがってパスは頂点を持つ「開いたポリライン」で渡す（頂点を持たない
//	Line で渡すと PIO が端点を取れず、バブルが挿入点に固定され向きも定まらない）。座標は
//	センタリング済みの絶対座標をそのまま頂点にする（Python 版と同じく後段の平行移動は不要）。
//
//	実描画（クラス分け・軸名ラベル・基点バブルの位置と向き）はローカルの VectorWorks で
//	目視確認する（ROADMAP.md M1「ローカル確認」）。GridAxis PIO のパラメータ名や座標単位は
//	VW 実機でのみ最終確認できる。
//

#include "PluginPrefix.h"
#include "draw/Grid.h"
#include "core/Document.h"

// GridAxis PIO の生成・設定に使う VWFC ラッパー（PluginPrefix の umbrella が取り込む
// VWFC::VWObjects の一部。明示 include で可用性を確実にする）。
#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWPolygon2DObj.h"
#include "VWFC/VWObjects/VWGroupObj.h"

#include <cstddef>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// デザインレイヤの種別コード（CreateLayer の layerType 引数）。1 = デザインレイヤ、
		// 2 = シート（プレゼンテーション）レイヤ。通り芯はデザインレイヤに置く。
		constexpr short kDesignLayerType = 1;

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

		// 1 本の通り芯を描く（Python 版 vw/grid.py draw_grid に対応）。開いたポリラインを
		// パスに、空グループをプロファイルにして GridAxis PIO を生成し、クラス・軸名ラベル・
		// 基点バブルを設定して再計算する。PIO 生成に失敗したらパスのポリライン（絶対座標の
		// 直線）をそのままクラス付けしてフォールバックする。何か 1 つでも配置できたら true。
		bool DrawOne(const core::GridCommand& grid)
		{
			// パス: センタリング済み絶対座標の始点→終点を頂点に持つ「開いた」2D ポリライン。
			// PIO はこの頂点から線の向き・端点・基点バブルの位置を決める（要点は冒頭コメント）。
			VWPolygon2DObj path(
				{VWPoint2D(grid.start.x, grid.start.y), VWPoint2D(grid.end.x, grid.end.y)});
			path.SetClosed(false); // ポリゴン（閉）でなくポリライン（開）にする
			const MCObjectHandle pathHandle = path.GetThisObject();
			if (pathHandle == nil)
				return false;

			// プロファイル: 空グループ（Python 版の BeginGroup/EndGroup に対応）。
			const VWGroupObj profileGroup;

			// パス＋プロファイルから GridAxis のカスタムオブジェクト（PIO）を生成する。
			// 第 4 引数は生成後に再計算するか（true）。'GridAxis' PIO が無い等で失敗（nil）
			// したらパスのポリラインをフォールバックにする。
			const TXString kGridAxis("GridAxis");
			MCObjectHandle object = gSDK->CreateCustomObjectPath(
				kGridAxis, pathHandle, profileGroup.GetThisObject(), true);

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
				// フォールバック: PIO 生成に失敗したら、パスのポリライン（絶対座標の直線）を
				// そのままクラス付けして残す（バブルは無いので位置問題は起きない）。
				SetClassByName(pathHandle, grid.drawClass);
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
