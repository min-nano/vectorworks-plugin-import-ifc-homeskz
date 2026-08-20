//
//	draw/Legend.h
//
//	Phase 2（VW 描画）のグラフィック凡例モジュール。Python 版 vw/sheet.py の draw_legend に
//	対応する（ROADMAP.md M13）。シート命令が持つ凡例（core::SheetCommand::legend）を
//	**そのシートレイヤの上**に VW 標準の "GraphicLegend" PIO で置き、ユーザーが VW 側で
//	用意したグラフィック凡例スタイルを関連付ける。
//
//	【ビューポート注釈ではない】データタグ（draw/Tag）はビューポートの注釈空間に入るが、
//	凡例は**シートレイヤに直接置く**（＝用紙の上に載る）。したがって位置は用紙座標で、
//	ビューポートの中身とは無関係に決まる。
//
//	【中身はスタイルが決める・スタイルは当てるだけでは効かない】凡例に何が並ぶか
//	（ソース定義・集計基準・行レイアウト・ラベル）は PIO のパラメータでは設定できず、
//	スタイルへ焼き込むしかない（理由は core/Document.h の LegendCommand）。しかも
//	**スタイルを関連付けただけでは中身が流し込まれない**——構造材・柱と同じ落とし穴で
//	（Python 版 #56）、置き終えてから使ったスタイルごとに UpdateStyledObjects を 1 回
//	呼んで初めてセル（＝並ぶシンボル）が計算される。呼ばないと凡例は空のまま（幅 0 の
//	線に潰れる）になる。**したがって drawSheetLegend と updateLegendStyles は対で使う。**
//
//	【SDK 型を公開するヘッダ】シートレイヤのハンドルを引数に取るため、draw/Tag.h・
//	draw/DrawUtil.h と同じく**SDK 型を公開する共通ヘッダ**で、自分で PluginPrefix.h を
//	（DrawUtil.h 経由で）取り込む。したがって**要素ごとの draw/*.h から include しては
//	ならない**（あちらは SDK を持たない翻訳単位＝Extensions/ExtMenu からも include される
//	ため。DrawUtil.h 冒頭の約束）。呼び出し元は draw/Sheet.cpp だけ。
//
//	【凡例の作法（Python 版 draw_legend と同じ順）】
//	  シートレイヤをアクティブに → CreateCustomObject("GraphicLegend", 位置, 0) →
//	  スタイルを関連付け → 箱幅 BoxWidth を与える → ResetObject →
//	  線の太さ・塗りをオブジェクトの属性として設定 → （全部置いてから）UpdateStyledObjects
//	  * **箱幅を与えるのは、点で生成すると幅 0 になる**ため（凡例は矩形モードの PIO で、
//	    対話作成ではユーザーが描いた矩形の幅にレイアウトが追従する）。幅 0 のままだと
//	    サイズ 0 でリサイズハンドルも掴めない。
//	  * **見た目はクラスでは制御できない**——凡例 PIO が内部で描く枠線・セルは、本体の
//	    クラスにもカレントクラスにも従わず一般クラスの属性で作図される（Python 版が VW 上で
//	    確認済み）。そこでクラスによる制御は諦め、線の太さ（0.13mm）と塗り（なし）を
//	    **オブジェクトの属性として直接**設定する。
//
//	実描画（凡例の中身・大きさ・位置）はローカルの VectorWorks で目視確認する
//	（ROADMAP.md M13「ローカル確認」）。
//

#pragma once

#include "draw/DrawUtil.h"

#include "core/Document.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	// 凡例描画の集計。**実描画はローカルの VW でしか確認できない**ので、凡例が出ない・
	// 空になるときに原因（スタイルが無い／PIO を作れない／箱幅を書けない）を切り分けられる
	// ように件数で持ち帰る（draw/Tag の TagCounts と同じ流儀）。
	struct LegendCounts
	{
		std::size_t drawn = 0;	   // シートレイヤに置けた凡例
		std::size_t failed = 0;	   // PIO を作れなかった
		std::size_t widthLeft = 0; // 箱幅を書けなかった（幅 0 のまま潰れる）
		bool styleMissing = false; // 命令のスタイルが文書に無かった（中身が空になる）

		// 実際に関連付けたスタイル（重複なし）。**置き終えた後に 1 つずつ
		// UpdateStyledObjects を呼ぶ**ために覚える（ヘッダ冒頭「スタイルは当てるだけでは
		// 効かない」）。
		std::vector<RefNumber> styles;
	};

	// グラフィック凡例 PIO の定義を**設定ダイアログを出さない**で用意する。凡例を 1 つでも
	// 置くフェーズ（伏図）の先頭で 1 回呼ぶ。理由は draw/Tag の prepareDataTagPlugin と同じ
	// （CreateCustomObject が最初の 1 個で定義を作るとき、既定ではダイアログが出て
	// インポートが止まる）。**静的フラグで 1 回だけにはしない**——定義は文書ごとなので、
	// 次の文書へのインポートで抜けてしまう。
	void prepareGraphicLegendPlugin();

	// 凡例 1 つをシートレイヤの上に置く。置けたら true を返し、内訳を counts へ積む
	// （複数のシートぶんを 1 つの counts へ積んでよい）。**カレントレイヤをそのシートレイヤへ
	// 移す**（PIO はカレントレイヤに入るため）ので、呼び出し側は必要なら後で戻すこと。
	bool drawSheetLegend(MCObjectHandle sheetLayer, const core::LegendCommand& command,
						 LegendCounts& counts);

	// 置いた凡例の中身をスタイルから流し込む。**すべての凡例を置き終えてから 1 回だけ**
	// 呼ぶ（使ったスタイルごとに UpdateStyledObjects を 1 回。ヘッダ冒頭）。
	void updateLegendStyles(const LegendCounts& counts);

	// 集計を人が読める 1 行の診断にする（異常が無ければ空文字）。
	std::string legendDiagnostics(const LegendCounts& counts);
} // namespace HomeskzIfcImport::draw
