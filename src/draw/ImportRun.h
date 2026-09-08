//
//	draw/ImportRun.h
//
//	**取り込み 1 周ぶんの部品**（ファイル選択・ビルドの素性・解析＋描画＋集計）。
//	**本番の取り込みコマンドと実機テストのコマンドが、ここだけを共有する。**
//
//	【なぜ分けたか】M23〜M24 では往復（実機フィードバック）の都合が本番の取り込み
//	コマンドの中へ食い込んでいた——記憶を読んでファイル選択を飛ばす分岐、「次は更新を
//	尋ねずに入れてよいか」を意味する戻り値、投稿できたら結果ダイアログを出さない分岐。
//	`#ifdef VW_DEV_BUILD` の外にあるので**安定版にもそのまま入っており**、本番の経路に
//	バグを混ぜる余地になっていた（M25。docs/DEV-NOTES.md）。
//
//	そこで**絵を作るところだけ**をここへ出し、その上に 2 つの入口を並べた:
//
//	    draw/ImportCommand … 本番。ファイル選択 → 設定 → runImportRound → 結果ダイアログ
//	    draw/Feedback      … 実機テスト。記憶した条件 → runImportRound → PR へ投稿
//
//	**どちらも同じ runImportRound を通る**ので、テストで走るのは本番と同じコードである。
//	往復のことを知っているのは draw/Feedback だけになり、本番の経路からは往復の分岐が
//	1 つも無くなった。
//
//	【出るダイアログ】`runImportRound` は**進捗ダイアログだけ**を出す（数百回の SDK 呼び
//	出しで固まって見えないため。draw/ProgressDialog.h）。ファイル選択（`chooseIfcFile`）
//	と設定・結果のダイアログは呼び出し側が持つ——**テストの周は 1 枚も出さずに回れる**
//	ことが、この分け方の要件である。
//

#pragma once

#include "core/Document.h"
#include "core/ImportOptions.h"
#include "parse/Summary.h"

#include <string>

namespace HomeskzIfcImport::draw
{
	// 取り込み 1 周ぶんの結果。**完了ダイアログの本文だけでは足りない**——実機
	// フィードバック（draw/Feedback）は命令セットと描画結果そのものを見て内訳と差分を
	// 組み立てるので、それらをここから持ち帰る。
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
	// 何も描かず静かに終える）。
	bool chooseIfcFile(std::string& outPath);

	// 同じ「開く」ダイアログを、拡張子と見出しだけ差し替えて開く（`chooseIfcFile` の実体）。
	// **ダイアログの作法を 2 か所に書かないため**に公開してある——実機テストの周が「毎周
	// 開き直す図面」を選ばせるのに使う（draw/Feedback）。extension は "ifc" のように点を
	// 含めない綴りで、複数なら空白区切り。
	// 動かしているビルドの素性（診断ログの見出しと、往復の記憶の突き合わせに使う）。
	// **ここで詰めるのは、BuildConfig.h のマクロを見られるのが SDK 側だけ**だから
	// ——parse/Summary は受け取った文字列を並べるだけで、ビルド種別を知らない。
	parse::BuildInfo currentBuildInfo();

	// **取り込み本体。** 診断ログを開き、解析（Phase 1）→ 描画（Phase 2）を通して結果
	// 一式を返す。例外はここで受け止め、`failed=true` と説明つきの `body` で返る
	// ——**SDK コールバックの外へ例外を漏らさない**（CLAUDE.md「エラーハンドリング」）。
	//
	// prologue は**取り込みの前に何をしたか**を診断ログの見出しの次へ 1 行だけ書き添える
	// もの（空なら何も書かない）。実機テストの周が「図面をどう用意したか」を残すための口
	// で、本番のコマンドは空を渡す（draw/Feedback.cpp の prepareDrawingForRound）。
	ImportRound runImportRound(const std::string& ifcPath, const core::ImportOptions& options,
							   bool settingsShown, const std::string& settingsNote,
							   const std::string& prologue);
} // namespace HomeskzIfcImport::draw
