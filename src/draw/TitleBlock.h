//
//	draw/TitleBlock.h
//
//	Phase 2（VW 描画）の図面枠（タイトルブロック）モジュール（docs/DEV-NOTES.md M28）。
//	取り込みが**生成したシートレイヤすべて**（伏図＝draw/Sheet・軸組図＝draw/Section）に
//	図面枠 PIO を 1 つずつ置き、取り込み設定で選ばれた**図面枠スタイル**を適用する。
//
//	【命令の列を取らない】置くものは「シートレイヤ 1 枚につき 1 つ」で、IFC の中身では
//	数も内容も変わらない。しかも軸組図のシートレイヤは**何枚になるかが描画時にしか
//	決まらない**（core/Document.h の SectionSheetCommand）ので、命令を 1 枚ずつ並べる
//	ことがそもそもできない。命令セットが持つのはスタイル名ただ 1 つ
//	（core::Document::titleBlockStyle）で、ここはそれを受けて置く。
//
//	【★スタイルは当てるが、作らない】図面枠スタイルは**利用者が自身の図面に用意したもの**
//	を名前で指すだけで、プラグインは生成しない（CLAUDE.md 開発の基本方針 4）。したがって
//	  * 図面に**そのスタイルが無ければ 1 つも置かない**——スタイル無しの図面枠は「線だけの
//	    空の枠」になり、図面に不要な図形を増やすだけで役に立たない。置かずに診断へ残すほうが良い。
//	  * スタイルは `SetPluginObjectStyle` で関連付けるだけでは**中身が反映されない**ので、
//	    全部置いてから `UpdateStyledObjects` を 1 回呼ぶ（横架材・柱にスタイルを適用していた
//	    頃と同じ方法。
//	    [SDK リファレンス「Parametric Objects」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Parametric%20Objects.md)
//	    の「プラグインスタイル」）。
//
//	【★PIO の登録名は `"Title Block Border"`（実機で確定）】SDK リファレンスの `Findings/`
//	に当時載っていたのは「図面枠スタイルはシンボル定義の `GetSymbolDefSubType` が 552 になる」
//	ことだけで
//	（[Findings「Symbols」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Symbols.md)）、
//	**その型番号から登録名を引く呼び出しは知られていない**。そこで当初は候補を順に試して
//	通った名前を採用する形にし（PIO のパラメータ名を universal 名 → ローカライズ名の順で
//	再検索するのと同じ方法。draw/DrawUtil.h の ResolveParamName）、**実機フィードバックの
//	round 1 で `"Title Block Border"` に確定した**（VW 2026 / macOS。PR #129）ので候補は
//	削除してある。この知見はその後 SDK リファレンス側にも載った（Findings「Parametric Objects」の
//	「スタイルは SDK だけで作れる」の表に `Title Block Border` ＝ 552）。
//	置けなかった件数と登録名は診断へ出力するので、別の環境で違っていれば次の周で分かる。
//
//	【置き場所は測って決める】図面枠の挿入点が枠のどこを指すかは分からないので、
//	**置いた後に外形を測って用紙の中心へ寄せる**（データタグ・グラフィック凡例と同じ
//	「置いた後に測って動かす」方法。
//	[Findings「Sheet Layers and Page Layout」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Sheet%20Layers%20and%20Page%20Layout.md)）。
//	用紙の中心は**原点**である（同 Findings「用紙は原点を中心に置かれている前提」。
//	draw/DrawUtil.h の SheetPaperArea）。**印刷可能領域の中心ではない**——余白が左右／上下で
//	違う用紙ではそこが原点からずれるが、図面枠は用紙に属するものなので用紙に合わせる。
//
//	【★ビューポートを仕上げた後に置き、最背面へ回す】図面枠の「縮尺」欄は、**生成時に**
//	用紙に載っているビューポートの縮尺を取得し、その後は自動で再取得しない（ビューポートは
//	1 枚の用紙へ何枚でも置けるので、どれが変わっても枠は追わない）。当初はオブジェクトの
//	重なり（後から生成したものが手前に来る）だけを考慮して**ビューポートより先に**置いていたが、
//	それでは縮尺欄が 1:1 のまま残り、スタイルの反映（`UpdateStyledObjects`）も
//	1 つずつの `ResetObject` も機能しなかった（実機。docs/DEV-NOTES.md M28）。そこで
//	**ビューポートの縮尺を確定させた後**（finishTitleBlocks）に生成し、重なりは
//	`InsertObjectBefore` でシートレイヤの先頭へ差し込んで最背面へ回す（オブジェクト列は
//	背面→前面の順。[Findings「Layers and Stories」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Layers%20and%20Stories.md)
//	のレイヤの並べ替えと同じ API）。呼び出し側（draw/Sheet・draw/Section）は、用意した
//	シートレイヤを addTitleBlockSheet で記録し、ビューポートを仕上げた後に finishTitleBlocks
//	を呼ぶ。
//
//	【SDK 型を公開するヘッダ】シートレイヤのハンドルを引数に取るため、draw/Legend.h・
//	draw/Tag.h と同じく**SDK 型を公開する共通ヘッダ**で、自身で PluginPrefix.h を
//	（DrawUtil.h 経由で）取り込む。したがって**要素ごとの draw/*.h から include しては
//	ならない**（DrawUtil.h 冒頭の約束）。呼び出し元は draw/Sheet.cpp と draw/Section.cpp。
//
//	実描画（枠が用紙のどこに表示されるか・スタイルの中身）はローカルの VectorWorks で目視確認する。
//

#pragma once

#include "draw/DrawUtil.h"

#include "core/Document.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	// 図面枠の設置の集計。**実描画はローカルの VW でしか確認できない**ので、枠が表示されない・
	// 位置がおかしいときに原因（スタイルが無い／PIO を生成できない／測れない）を切り分けられる
	// ように件数で記録する（draw/Legend の LegendCounts と同じ方式）。
	//
	// 呼び出し側は prepareTitleBlocks で 1 つ生成し、シートレイヤごとに addTitleBlockSheet へ
	// 渡し、ビューポートを仕上げた後に finishTitleBlocks を呼ぶ。**伏図と軸組図は別々の集計を持つ**（フェーズが
	// 分かれており、診断行も別々に出力されるため）。
	struct TitleBlockCounts
	{
		// 適用するスタイル名（命令セットの複製。空なら「置かない」）。
		std::string style;
		// 解決したスタイルの RefNumber。0 なら**その名前のスタイルが図面に無い**ので、
		// 1 つも置かない（ヘッダ冒頭の ★）。
		RefNumber styleRef = 0;
		// 実際にオブジェクトを生成できた PIO の登録名（空なら 1 つも通っていない）。
		// ヘッダ冒頭の ★「登録名は候補から実地に決める」。
		std::string plugin;

		std::size_t drawn = 0; // 置けた図面枠
		std::size_t failed = 0; // どの候補名でも PIO を生成できなかったシートレイヤ
		std::size_t placeLeft = 0; // 外形を測れず、用紙の中心へ寄せられなかった
		std::size_t frontLeft = 0; // 最背面へ回せず、図を覆っているかもしれない
		// 本置きした 1 つ目の外形（用紙 mm。測れなければ 0）。割り付けの前に仮に測った
		// 大きさ（measureTitleBlockFrame）と照合するために診断ログへ出力する。
		core::Vec2 placedSize;

		// 図面枠を置くシートレイヤ（記録した順）。**軸組図は同じシートレイヤへ複数の命令が
		// 載る**ので、重複して記録しない（1 枚の用紙に図面枠が何重にも積まれないように）。
		std::vector<MCObjectHandle> sheets;
	};

	// 命令セットの図面枠の設定を読み、スタイルを解決する。置かない（スタイル名が空・
	// その名前のスタイルが図面に無い）ときは styleRef が 0 のまま返り、以降の
	// addTitleBlockSheet / finishTitleBlocks は何もしない。**図面枠を置くフェーズの先頭で
	// 1 回**呼ぶ。
	TitleBlockCounts prepareTitleBlocks(const core::Document& document);

	// 図面枠を置くなら、**置いたときの外形**（用紙 mm。用紙の中心＝原点へ寄せた矩形）を
	// 返す。置かない（スタイルが無い）・生成できない・測れないときは nullopt。
	//
	// 【なぜ要るか】軸組図は印刷可能領域いっぱいに 2 段で並べるので、印刷可能領域が用紙
	// いっぱい（余白 0）の用紙では**図の下端が図面枠と重なった**（ご要望）。枠の内側へ
	// 並べるには枠の大きさが要るが、図面枠はビューポートを仕上げた後にしか生成できない
	// （ヘッダ冒頭の ★ 縮尺欄）。そこで**割り付けの前に 1 つ仮に置いて測り、すぐ削除する**
	// ——外形はスタイルと用紙で決まり、ビューポートには依らない。本物は従来どおり
	// finishTitleBlocks が置く。
	//
	// 返るのは外形だけで、それが「用紙を囲む枠」なのか「枠線の無い表題欄の帯」なのかは
	// 呼び出し側が大きさで分ける（core::frameCoversPaper。実機のスタイルは後者で、
	// 235 × 19mm が返った。PR #176 round 1）。
	// カレントレイヤは呼ぶ前の状態へ戻す。
	std::optional<core::PaperArea> measureTitleBlockFrame(const TitleBlockCounts& counts,
														  MCObjectHandle sheetLayer);

	// 図面枠を置くシートレイヤとして記録する（まだ置かない。ヘッダ冒頭の ★）。同じシート
	// レイヤは重複して記録しない（TitleBlockCounts::sheets）。
	void addTitleBlockSheet(MCObjectHandle sheetLayer, TitleBlockCounts& counts);

	// 記録したシートレイヤへ図面枠を 1 つずつ置き、スタイルを反映し（`UpdateStyledObjects`
	// を 1 回）、最背面へ回し、外形を測って用紙の中心＝**原点**へ寄せる。**ビューポートの
	// 縮尺を確定させた後に**呼ぶ（縮尺欄は生成時のビューポートの縮尺を取得する）。
	// **カレントレイヤを動かす**（PIO はカレントレイヤに入るため）ので、呼び出し側は
	// 後で戻すこと。
	void finishTitleBlocks(TitleBlockCounts& counts);

	// 集計を人が読める 1 行の診断にする（異常が無ければ空）。
	std::string titleBlockDiagnostics(const TitleBlockCounts& counts);

	// 平常でも出力される内訳（適用したスタイル名・使った登録名・置いた枚数）。
	// 診断ログにだけ出力する（draw/Sheet・draw/Section の outInfo と同じ出力先）。
	//
	// **伏図と軸組図で別々に出力する。** 枚数が違う（伏図は命令の数、軸組図は用紙の数）ので、
	// 片方だけを出力すると「全シートレイヤへ置けたか」が確認できない——利用者が求めているのが
	// まさにそこなので、`what`（"伏図" / "軸組図"）を添えて 2 行並べる。
	std::string titleBlockInfo(const char* what, const TitleBlockCounts& counts);
} // namespace HomeskzIfcImport::draw
