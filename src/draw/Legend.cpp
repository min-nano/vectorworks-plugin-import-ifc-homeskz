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
//	  * gSDK->GetObjectInternalIndex(viewport)                           … フィルタ先の参照
//	  * gSDK->TaggedDataCreate / TaggedDataSet                           … フィルタの書き込み
//	  * gSDK->ResetObject / UpdateStyledObjects                          … 反映・中身の流し込み
//

#include "PluginPrefix.h"
#include "draw/Legend.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"

#include "VWFC/VWObjects/VWParametricObj.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// グラフィック凡例の内部プラグイン名。
		// **表示名「グラフィック凡例」とは別物**で、登録名はスペース無しの "GraphicLegend"。
		constexpr const char* kGraphicLegendPlugin = "GraphicLegend";

		// 箱幅パラメータ。凡例は矩形モードの PIO なので、点で生成すると幅 0 のまま潰れる
		// （draw/Legend.h）。ここで与えるのは**生成時の箱の幅**で、用紙をどれだけ空けるかは
		// これではなく**置いた後の実測**が決める（measureLegendWidth）——凡例は図面の内容で
		// 伸び縮みするので、決め打ちの幅を割り付けに使わない。高さは行の内容から自動で
		// 決まるので与えない。
		constexpr const char* kFieldBoxWidth = "BoxWidth";
		// ★**要求した幅がそのまま図の取り分を減らす。** 用紙に空ける幅は実測で決まるので
		// （measureLegendWidth）、ここで広く頼むほど伏図の縮尺が落ちる。実機では並ぶ
		// シンボルが 25mm ほどしか使っておらず、60mm を頼んでいたときは 1/50 に 330mm 要る
		// 建物に対して使える幅が 315mm しか残らず 1/75 へ落ちていた（M16 のローカル確認）。
		// **中身が必要とする幅より少し広い程度**に留める。
		constexpr double kBoxWidth = 40.0;

		// 見た目。凡例 PIO が内部で描く枠線・セルは**クラスでは制御できない**ので、
		// オブジェクトの属性として直接与える（draw/Legend.h）。線の太さの単位はミル（1/1000
		// インチ）で、5 ミル = 0.127mm を VW は 0.13mm と表示する。塗りパターン 0 = なし。
		constexpr short kLineWeightMils = 5;
		constexpr InternalIndex kFillNone = 0;

		// 「ビューポートでフィルタ」の保存先（draw/Legend.h 冒頭・docs/DEV-NOTES.md）。
		//
		// **`'GrLg'` を文字リテラルで書かない**のは、多文字リテラルが処理系定義で警告の
		// 対象になるため。値は 'G'=0x47 / 'r'=0x72 / 'L'=0x4C / 'g'=0x67 を並べたもので、
		// 実機のダンプではデータオブジェクトの中身の +86 にこの並びで入っていた。
		constexpr OSType kFilterContainer = 0x47724C67; // 'GrLg'（Graphic Legend）

		// オブジェクト参照の配列（Kernel/API/MiniCadCallBacks.h の
		// kTaggedDataObjectRefArrayTypeID）と、その中でフィルタが使うタグ。
		constexpr Sint32 kFilterDataType = 15;
		constexpr Sint32 kFilterDataTag = 5;
		constexpr Sint32 kFilterElementCount = 1;

		// 凡例を「そのシートのビューポートに映っているものだけ」に絞る。書けたら true。
		//
		// **UI の「ビューポートでフィルタ…」と同じ状態**を作る。VW はこれを凡例にぶら下がる
		// データオブジェクトのタグ付きデータとして持っており、SDK からは
		// TaggedDataCreate ＋ TaggedDataSet で書ける（読み書きの API はあるが、**凡例の
		// フィルタ専用の呼び出しは SDK にも VectorScript にも無い**——容れ物と型とタグは
		// 実機のバイト列から突き止めた。docs/DEV-NOTES.md「グラフィック凡例」）。
		bool ApplyViewportFilter(MCObjectHandle legend, MCObjectHandle viewport)
		{
			if (legend == nil || viewport == nil)
				return false;

			// フィルタ先は**オブジェクトの内部参照**で持つ。0 は「参照が無い」なので、
			// そのまま書くと「フィルタ無し」と区別が付かない状態を作ってしまう。
			const InternalIndex reference = gSDK->GetObjectInternalIndex(viewport);
			if (reference == 0)
				return false;

			if (!gSDK->TaggedDataCreate(legend, kFilterContainer, kFilterDataType, kFilterDataTag,
										kFilterElementCount))
				return false;
			return gSDK->TaggedDataSet(legend, kFilterContainer, kFilterDataType, kFilterDataTag, 0,
									   &reference) != 0;
		}
	} // namespace

	void prepareGraphicLegendPlugin()
	{
		gSDK->DefineCustomObject(TXString(kGraphicLegendPlugin), kCustomObjectPrefNever);
	}

	bool drawSheetLegend(MCObjectHandle sheetLayer, const core::LegendCommand& command,
						 const core::Vec2& where, MCObjectHandle filterViewport,
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

		// 生成位置は仮（右上に合わせるのは中身が流し込まれて大きさが定まってから＝
		// placeLegends）。
		const MCObjectHandle object = gSDK->CreateCustomObject(
			TXString(kGraphicLegendPlugin), WorldPt(where.x, where.y), 0.0, true);
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

		// そのシートのビューポートで絞る（**ResetObject より前**——凡例の作り直しで
		// 並ぶセルが決まるため）。書けなくても凡例自体は残るので、件数だけ持ち帰って続ける
		// （文書中の全シンボルが並ぶ状態になる）。
		if (ApplyViewportFilter(object, filterViewport))
			++counts.filtered;
		else
			++counts.filterLeft;

		gSDK->ResetObject(object);

		// 見た目はクラスでは効かないのでオブジェクトの属性として直接与える。**ResetObject
		// の後・ UpdateStyledObjects より前**に置くと by-instance の属性として保たれる。
		gSDK->SetLineWeight(object, kLineWeightMils);
		gSDK->SetFillPat(object, kFillNone);

		// 位置合わせのために覚えておく（中身を流し込んだ後に placeLegends が動かす）。
		counts.objects.push_back(object);
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

	double measureLegendWidth(const LegendCounts& counts)
	{
		// **いちばん広いもの**を採る。伏図は全図が同じ位置・同じ縮尺なので、空ける幅は
		// どのシートの凡例も収まる幅でなければならない（core::planLayout）。
		double widest = 0.0;
		for (const MCObjectHandle object : counts.objects)
		{
			if (WorldRect bounds; gSDK->GetObjectBounds(object, bounds))
				widest = std::max(widest, std::abs(bounds.right - bounds.left));
		}
		return widest;
	}

	void placeLegends(const LegendCounts& counts, const core::Vec2& topRight)
	{
		for (const MCObjectHandle object : counts.objects)
		{
			if (WorldRect bounds; gSDK->GetObjectBounds(object, bounds))
				gSDK->MoveObject(object, topRight.x - bounds.right, topRight.y - bounds.top);
		}
	}

	std::string legendDiagnostics(const LegendCounts& counts)
	{
		if (counts.failed == 0 && counts.widthLeft == 0 && counts.filterLeft == 0 &&
			!counts.styleMissing)
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
		if (counts.filterLeft > 0)
			text += "ビューポートで絞れなかった凡例 " + std::to_string(counts.filterLeft) +
					" 件（その図に無いシンボルも並びます）。";
		return text;
	}
} // namespace HomeskzIfcImport::draw
