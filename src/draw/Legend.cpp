//
//	draw/Legend.cpp
//
//	グラフィック凡例の設置の実装。意図・規約は draw/Legend.h と core/Document.h の
//	LegendCommand を参照。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされる。
//
//	使用する SDK API:
//	  * gSDK->DefineCustomObject("GraphicLegend", kCustomObjectPrefNever) … 設定ダイアログ抑止
//	  * gSDK->CreateCustomObject("GraphicLegend", 位置, 0, bInsert)      … 凡例 PIO の生成
//	  * gSDK->SetCurrentLayer(sheetLayer)                                … 置き場所（用紙）の指定
//	  * gSDK->SetPluginObjectStyle(object, style)                        … 凡例スタイルの関連付け
//	  * gSDK->SetLineWeight / SetFillPat                                 … 見た目（線の太さ・塗り）
//	  * gSDK->ResetObject / UpdateStyledObjects                          … 反映・中身の流し込み
//

#include "PluginPrefix.h"
#include "draw/Legend.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"

#include "VWFC/VWObjects/VWParametricObj.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// グラフィック凡例の内部プラグイン名（Python 版 vw/sheet.py _GRAPHIC_LEGEND_PLUGIN）。
		// **表示名「グラフィック凡例」とは別物**で、登録名はスペース無しの "GraphicLegend"
		// （Python 版が VW 上で実オブジェクトのパラメトリックレコードから確認済み）。
		constexpr const char* kGraphicLegendPlugin = "GraphicLegend";

		// 箱幅パラメータ（Python 版 _LEGEND_WIDTH_FIELD / _LEGEND_BOX_WIDTH）。凡例は
		// 矩形モードの PIO なので、点で生成すると幅 0 のまま潰れる（draw/Legend.h）。
		// 用紙上（ドキュメント単位 mm）の適当な幅を与えて可視化し、**ローカルの VW で
		// 最終調整する**。高さは行の内容から自動で決まるので与えない。
		constexpr const char* kFieldBoxWidth = "BoxWidth";
		constexpr double kBoxWidth = 150.0;

		// 見た目（Python 版 _LEGEND_LINE_WEIGHT_MILS / _LEGEND_FILL_NONE）。凡例 PIO が
		// 内部で描く枠線・セルは**クラスでは制御できない**ので、オブジェクトの属性として
		// 直接与える（draw/Legend.h）。線の太さの単位はミル（1/1000 インチ）で、
		// 5 ミル = 0.127mm を VW は 0.13mm と表示する。塗りパターン 0 = なし。
		constexpr short kLineWeightMils = 5;
		constexpr InternalIndex kFillNone = 0;
	} // namespace

	void prepareGraphicLegendPlugin()
	{
		gSDK->DefineCustomObject(TXString(kGraphicLegendPlugin), kCustomObjectPrefNever);
	}

	bool drawSheetLegend(MCObjectHandle sheetLayer, const core::LegendCommand& command,
						 LegendCounts& counts)
	{
		if (sheetLayer == nil)
		{
			++counts.failed;
			return false;
		}

		// 凡例は**シートレイヤの上**に置く（用紙に載る）。PIO は bInsert=true でカレント
		// レイヤへ入るので、先にそのシートレイヤをアクティブにする。
		gSDK->SetCurrentLayer(sheetLayer);

		const MCObjectHandle object =
			gSDK->CreateCustomObject(TXString(kGraphicLegendPlugin),
									 WorldPt(command.position.x, command.position.y), 0.0, true);
		if (object == nil)
		{
			++counts.failed;
			return false;
		}

		// スタイル（"基礎伏図凡例" / "床伏図凡例"）。文書に無ければ**スタイル無しで置く**
		// ——凡例を失うより、空でも箱が残っている方が「スタイルが無い」と分かりやすい
		// （構造材・データタグと同じ方針。draw/DrawUtil の ResolvePluginStyle）。
		const RefNumber style = ResolvePluginStyle(TXString(command.style.c_str()));
		if (style != 0)
		{
			gSDK->SetPluginObjectStyle(object, style);
			// 置き終えてから中身を流し込むために覚える（draw/Legend.h）。
			if (std::ranges::find(counts.styles, style) == counts.styles.end())
				counts.styles.push_back(style);
		}
		else
		{
			counts.styleMissing = true;
		}

		// 箱幅（スタイルの関連付けより**後**に書く。幅は by-instance のジオメトリで、
		// スタイルが決めるものではない）。
		try
		{
			VWParametricObj pio(object);
			if (!SetParamRealChecked(pio, TXString(kFieldBoxWidth), kBoxWidth))
				++counts.widthLeft;
		}
		catch (...)
		{
			// 幅 0 のまま潰れるだけで凡例自体は図面に残るので、失敗しても続ける。
			++counts.widthLeft;
		}

		gSDK->ResetObject(object);

		// 見た目はクラスでは効かないのでオブジェクトの属性として直接与える。**ResetObject の
		// 後・UpdateStyledObjects より前**に置くと by-instance の属性として保たれる
		// （Python 版 draw_legend と同じ順）。
		gSDK->SetLineWeight(object, kLineWeightMils);
		gSDK->SetFillPat(object, kFillNone);

		++counts.drawn;
		return true;
	}

	void updateLegendStyles(const LegendCounts& counts)
	{
		// スタイルが決める中身（ソースから集めたセル＝並ぶシンボル）をインスタンスへ
		// プッシュする。**関連付けただけでは流し込まれない**ので、これを呼ばないと凡例は
		// 空のまま（draw/Legend.h）。by-instance の箱幅・線の太さ・塗りは保たれる。
		for (const RefNumber style : counts.styles)
			gSDK->UpdateStyledObjects(style);
	}

	std::string legendDiagnostics(const LegendCounts& counts)
	{
		if (counts.failed == 0 && counts.widthLeft == 0 && !counts.styleMissing)
			return {};

		std::string text = "伏図のグラフィック凡例の診断: ";
		if (counts.styleMissing)
			text += "グラフィック凡例スタイルが文書にありません（スタイル無しで置いたため"
					"中身が空になります）。";
		if (counts.failed > 0)
			text += "凡例を置けなかった命令 " + std::to_string(counts.failed) + " 件。";
		if (counts.widthLeft > 0)
			text += "箱幅を設定できなかった凡例 " + std::to_string(counts.widthLeft) +
					" 件（幅 0 に潰れます）。";
		return text;
	}
} // namespace HomeskzIfcImport::draw
