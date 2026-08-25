//
//	draw/ColumnMark.h
//
//	Phase 2（VW 描画）の断面記号・伏図記号モジュール（docs/DEV-NOTES.md M12）。命令
//	1 つにつき **PIO を 1 つ**置き、検索対象レイヤ・クラス・記号スタイル・シンボルをパラメータ
//	へ書いてリセットする。記号そのものを描くのは PIO 本体（Extensions/ExtColumnMark）で、
//	ここはその設置だけを担う。
//
//	【PIO は本プラグインが提供する】記号を描く PIO は**同じモジュールへ同梱**する
//	（Extensions/ExtColumnMark。ModuleMain がメニューコマンドと一緒に登録する）。
//	インストールも自動更新も 1 つで済み、記号の規約（構造用途・レイヤ名・クラス名）を解析側と
//	1 か所で共有できる。登録名はこのプラグイン固有にしてあるので、同種の記号 PIO
//	を持つ環境でも衝突しない。
//
//	【リセットが追随の契機】記号は PIO のリセット時に対象レイヤを検索して描き直される。
//	柱を動かした瞬間に動くわけではないが、リセット・ファイル再オープンのたびに実物から
//	導き直されるので、**古い記号が間違った内容のまま残ることがない**（静的なジオメトリや
//	データタグでは、断面が変わったときに嘘の記号が残る）。
//
//	【レイヤの扱いが 2 つで違う】
//	  * 断面記号 … 配置先は柱と同じ span レイヤ。ストーリが作るレイヤなので**無ければ
//	    スキップ**（ActivateExistingLayer の規約）。
//	  * 伏図記号 … "{to}-柱伏図記号" はストーリに属さない独立レイヤで story 命令が作らない。
//	    通り芯の "共通" と同じく**無ければ作る**（PrepareLayer）。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	// 記号（columnMark 命令）を PIO として置く。置けた数を返す。配置先レイヤが用意
	// できない命令・PIO を作れない命令はスキップし、その件数を outNote に残す
	// （完了ダイアログの診断。draw/Symbol と同じ流儀）。
	//
	// **柱の描画後に呼ぶこと。** PIO はリセット時に対象レイヤの構造材を検索するので、
	// 柱が置かれていないと記号 0 個で確定してしまう。
	std::size_t drawColumnMarks(const core::Document& document, core::ProgressReporter& progress,
								std::string* outNote = nullptr);

	// 伏図記号レイヤ名を**重複を除いて命令の出現順**で返す。draw/Story の
	// reorderStoryLayers が、ストーリに属さない独立レイヤとして希望スタック順の
	// 上段（通り芯 "共通" の直下）へ差し込むのに使う。
	std::vector<std::string> planMarkLayerNames(const core::Document& document);

	// 伏図記号レイヤ（"{to}-柱伏図記号"）を**記号を置くより前に**用意する（作れた数を返す）。
	//
	// 【なぜ前倒しするのか】レイヤの重ね順の並べ替え（draw/Story の reorderStoryLayers）は
	// **要素を 1 つも描く前**に済ませたい（同ヘッダ「できるだけ早く呼ぶこと」）。並べ替えの
	// 対象になるにはレイヤが実在していないといけないので、記号レイヤだけは先に作る。
	//
	// 「空のレイヤを先に作らない」（CLAUDE.md 開発の基本方針 5）には反しない——作るのは
	// planMarkLayerNames が返すレイヤ、つまり**この取り込みで記号を置くことが命令セットから
	// 確定している**ものだけ。
	std::size_t preparePlanMarkLayers(const core::Document& document);
} // namespace HomeskzIfcImport::draw
