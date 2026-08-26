//
//	draw/Legend.h
//
//	Phase 2（VW 描画）のグラフィック凡例モジュール（docs/DEV-NOTES.md M13）。シート命令が持つ
//	凡例（core::SheetCommand::legend）を
//	**そのシートレイヤの上**に VW 標準の "GraphicLegend" PIO で置き、ユーザーが VW 側で
//	用意したグラフィック凡例スタイルを関連付ける。
//
//	【そのシートのビューポートでフィルタする】凡例に並ぶのは**そのシートのビューポートに
//	映っているシンボルだけ**にする（＝OIP の「ビューポートでフィルタ…」を取り込み時に
//	済ませる）。これを設定する API は SDK にも VectorScript にも無く、**凡例にぶら下がる
//	データオブジェクトのタグ付きデータ**に保存されている——実機のバイト列で突き止めた
//	（経緯と読み方は docs/DEV-NOTES.md「グラフィック凡例」）。書き込みに要るのは 4 つ:
//	  * 容れ物 `'GrLg'`（0x47724C67。**データオブジェクトのタグ `'DMDT'` ではない**——
//	    あれは入れ物の入れ物で、本当の容れ物 ID は中身の +86 に入っている）
//	  * 型 15（`kTaggedDataObjectRefArrayTypeID` ＝ オブジェクト参照の配列）
//	  * タグ 5
//	  * 値＝フィルタ先ビューポートの `GetObjectInternalIndex`
//	`ISDK::TaggedDataCreate` ＋ `TaggedDataSet` で書き、**`ResetObject` より前**に済ませる
//	（凡例の作り直しでセルが決まるため）。
//
//	【ビューポート注釈ではない】データタグ（draw/Tag）はビューポートの注釈空間に入るが、
//	凡例は**シートレイヤに直接置く**（＝用紙の上に載る）。したがって位置は用紙座標で、
//	ビューポートの中身とは無関係に決まる。**置き場所は命令ではなく用紙の割り付けが決める**
//	（M16。core::planLayout が凡例のために空けた右の 1 列の右上へ寄せる）。
//
//	【大きさは実測して割り付けへ返す】凡例の幅は**その図面に何が並ぶか**で決まるので、
//	「これだけ空けておけば足りる」という定数を置かない（core/Layout.h「凡例の幅は定数で
//	持たない」）。置いて・流し込んで・measureLegendWidth で測った幅を伏図の縮尺の計算へ
//	渡し、最後に placeLegends で右上へ揃える。
//
//	【中身はスタイルが決める・スタイルは当てるだけでは効かない】凡例に何が並ぶか
//	（ソース定義・集計基準・行レイアウト・ラベル）は PIO のパラメータでは設定できず、
//	スタイルへ焼き込むしかない（理由は core/Document.h の LegendCommand）。しかも
//	**スタイルを関連付けただけでは中身が流し込まれない**——構造材・柱と同じ落とし穴で、
//	置き終えてから使ったスタイルごとに UpdateStyledObjects を 1 回呼んで初めてセル（＝並ぶ
//	シンボル）が計算される。呼ばないと凡例は空のまま（幅 0 の線に潰れる）になる。**したがって
//	drawSheetLegend と updateLegendStyles は対で使う。**
//
//	【SDK 型を公開するヘッダ】シートレイヤのハンドルを引数に取るため、draw/Tag.h・
//	draw/DrawUtil.h と同じく**SDK 型を公開する共通ヘッダ**で、自分で PluginPrefix.h を
//	（DrawUtil.h 経由で）取り込む。したがって**要素ごとの draw/*.h から include しては
//	ならない**（あちらは SDK を持たない翻訳単位＝Extensions/ExtMenu からも include される
//	ため。DrawUtil.h 冒頭の約束）。呼び出し元は draw/Sheet.cpp だけ。
//
//	【凡例の作法】シートレイヤをアクティブに → CreateCustomObject("GraphicLegend", 位置, 0)
//	→スタイルを関連付け → 箱幅 BoxWidth を与える → ResetObject →線の太さ・塗りをオブジェクトの
//	属性として設定 → （全部置いてから）UpdateStyledObjects
//	  * **箱幅を与えるのは、点で生成すると幅 0 になる**ため（凡例は矩形モードの PIO で、
//	    対話作成ではユーザーが描いた矩形の幅にレイアウトが追従する）。幅 0 のままだと
//	    サイズ 0 でリサイズハンドルも掴めない。
//	  * **見た目はクラスでは制御できない**——凡例 PIO が内部で描く枠線・セルは、本体のクラスに
//	    もカレントクラスにも従わず一般クラスの属性で作図される。そこでクラスによる制御は諦め、
//	    線の太さ（0.13mm）と塗り（なし）を**オブジェクトの属性として直接**設定する。
//
//	実描画（凡例の中身・大きさ・位置）はローカルの VectorWorks で目視確認する
//	（docs/DEV-NOTES.md M13「ローカル確認」）。
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
		std::size_t filtered = 0;  // そのシートのビューポートでフィルタできた
		std::size_t filterLeft = 0; // フィルタを書けなかった（文書中の全シンボルが並ぶ）
		bool styleMissing = false; // 命令のスタイルが文書に無かった（中身が空になる）

		// 実際に関連付けたスタイル（重複なし）。**置き終えた後に 1 つずつ
		// UpdateStyledObjects を呼ぶ**ために覚える（ヘッダ冒頭「スタイルは当てるだけでは
		// 効かない」）。
		std::vector<RefNumber> styles;

		// 置いた凡例そのもの。**中身が流し込まれた後に大きさが決まる**ので、位置合わせは
		// updateLegendStyles の最後にまとめて行う（下記）。
		std::vector<MCObjectHandle> objects;
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
	//
	// where は**生成位置**（用紙 mm）。仮の点でよい——**最終的な位置合わせは placeLegends
	// が行う**（凡例の大きさは並ぶ中身が決まって初めて定まるため。下記）。
	//
	// filterViewport には**そのシートに載せたビューポート**を渡す（nil なら絞り込まない）。
	// 凡例はそのビューポートに映っているシンボルだけを並べる（ヘッダ冒頭「そのシートの
	// ビューポートでフィルタする」）。
	bool drawSheetLegend(MCObjectHandle sheetLayer, const core::LegendCommand& command,
						 const core::Vec2& where, MCObjectHandle filterViewport,
						 LegendCounts& counts);

	// 置いた凡例の中身をスタイルから流し込む。**すべての凡例を置き終えてから**呼ぶ
	// （使ったスタイルごとに UpdateStyledObjects を 1 回。ヘッダ冒頭）。
	//
	// **2 度呼んでよい**（同じスタイルへ何度プッシュしても結果は同じ）。伏図では
	// 「凡例を置く → 流し込む → 幅を測って縮尺を決める → ビューポートを仕上げる →
	// もう一度流し込む」という順で使う——凡例に並ぶのはビューポートに映るシンボルなので、
	// 図が仕上がった後にもう一度流し込んで取りこぼしを無くす（draw/Sheet）。
	void updateLegendStyles(const LegendCounts& counts);

	// 置いた凡例のうち**いちばん広いものの幅**（用紙 mm）。凡例が 1 つも無い・どれも
	// 測れないときは 0。
	//
	// **必ず updateLegendStyles の後に呼ぶ**——中身が流し込まれるまで凡例の大きさは
	// 決まらない。この実測が伏図の縮尺を決める（core::planLayout の legendWidth）。
	// 凡例の幅を定数で決め打ちにしない理由は core/Layout.h「凡例の幅は定数で持たない」。
	double measureLegendWidth(const LegendCounts& counts);

	// 置いた凡例の**右上を topRight へ揃える**（用紙 mm。core::planLayout の
	// legendTopRight）。中身が流し込まれて大きさが定まった後に呼ぶこと。測れないものは
	// その場に残す（凡例は出る。位置だけずれる）。
	void placeLegends(const LegendCounts& counts, const core::Vec2& topRight);

	// 集計を人が読める 1 行の診断にする（異常が無ければ空文字）。
	std::string legendDiagnostics(const LegendCounts& counts);
} // namespace HomeskzIfcImport::draw
