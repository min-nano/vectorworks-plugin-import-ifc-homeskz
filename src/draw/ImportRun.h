//
//	draw/ImportRun.h
//
//	**取り込み 1 周ぶんの部品**（ファイル選択・ビルドの素性・解析＋描画＋集計）。
//	**本番の取り込みコマンドと実機テストのコマンドが、ここだけを共有する。** その上に
//	2 つの入口が並ぶ:
//
//	    draw/ImportCommand … 本番。ファイル選択 → 設定 → runImportRound → 結果ダイアログ
//	    draw/Feedback      … 実機テスト。頼まれた条件 → runImportRound → 報告を記録する
//
//	**どちらも同じ runImportRound を通る**ので、テストで実行されるのは本番と同じコードである。
//	実機テストのことを知っているのは draw/Feedback だけで、本番の経路には実機テストの
//	分岐が 1 つも無い。
//
//	【注意: 表示するダイアログ】`runImportRound` は**進捗ダイアログだけ**を出す（数百回の
//	SDK 呼び出しで固まって見えないため。draw/ProgressDialog.h）。ファイル選択
//	（`chooseIfcFile`）と設定・結果のダイアログは呼び出し側が持つ——**テストの周は 1 枚も
//	出さずに実行できる**ことが、この分け方の要件である。
//
//	【なぜ分けたか】M23〜M24 では往復（実機フィードバック）の都合が本番の取り込み
//	コマンドの中へ入り込んでいた——記憶を読んでファイル選択を省く分岐、「次は更新を
//	尋ねずに入れてよいか」を意味する戻り値、投稿できたら結果ダイアログを出さない分岐。
//	`#ifdef VW_DEV_BUILD` の外にあるので**安定版にもそのまま入っており**、本番の経路に
//	バグを混ぜる余地になっていた。そこで**描画結果を作るところだけ**をここへ分離した
//	（M25。docs/DEV-NOTES.md）。
//
#pragma once

#include "core/Document.h"
#include "core/ImportOptions.h"
#include "parse/Summary.h"

#include <string>

namespace HomeskzIfcImport::draw
{
	// 取り込み 1 周ぶんの結果。**完了ダイアログの本文だけでは足りない**——実機
	// テスト（draw/Feedback）は命令セットと描画結果そのものを参照して内訳と差分を
	// 組み立てるので、それらをここで返す。
	struct ImportRound
	{
		std::string body;			  // 完了ダイアログの短い本文
		core::Document document;	  // 命令セット
		core::DrawCounts counts;	  // 描画結果
		unsigned long long bytes = 0; // 対象ファイルの大きさ
		double seconds = 0.0;		  // 所要
		std::string startedAt;		  // 壁時計（ログの見出しと同じもの）
		bool failed = false;		  // 例外で中断した（body はその説明）
	};

	// ネイティブの「開く」ダイアログで IFC ファイルを 1 つ選ばせる。選ばれたらその絶対
	// パス（UTF-8）を outPath に入れて true。キャンセルや取得失敗は false（呼び出し側は
	// 何も描画せずに終える）。
	bool chooseIfcFile(std::string& outPath);

	// **取り込みの診断ログの在り処**（本番の取り込みと実機テストで同じ 1 つ）。
	// HOMESKZ_IFC_TRACE があればそれ、無ければ一時ディレクトリの既定の名前。MCP の
	// `vw_log` が本体を入れ替えたあとも同じファイルを読めるよう、**決め方はここ 1 か所**
	// （draw/McpBridge.cpp）。
	std::string importLogPath();

	// 実行中のビルドの素性（診断ログの見出しと、実機テストの報告の見出しに使う）。
	// **ここで組み立てるのは、BuildConfig.h のマクロを参照できるのが SDK 側だけ**だから
	// ——parse/Summary は受け取った文字列を並べるだけで、ビルド種別を知らない。
	parse::BuildInfo currentBuildInfo();

	// **取り込み本体。** 診断ログを開き、解析（Phase 1）→ 描画（Phase 2）を通して結果
	// 一式を返す。例外はここで受け止め、`failed=true` と説明つきの `body` で返る
	// ——**SDK コールバックの外へ例外を漏らさない**（CLAUDE.md「エラーハンドリング」）。
	//
	// prologue は**取り込みの前に何をしたか**を診断ログの見出しの次へ 1 行だけ書き添える
	// もの（空なら何も書かない）。実機テストの周が「図面をどう用意したか」を残すための
	// 引数で、本番のコマンドは空を渡す（draw/Feedback.cpp の openRoundDocument）。
	ImportRound runImportRound(const std::string& ifcPath, const core::ImportOptions& options,
							   bool settingsShown, const std::string& settingsNote,
							   const std::string& prologue);
} // namespace HomeskzIfcImport::draw
