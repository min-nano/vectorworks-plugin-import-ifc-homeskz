//
//	core/FeedbackScratch.h
//
//	**実機テストの一時ファイルの置き場と、その片付け。** 実機テスト（draw/Feedback）は
//	図面を別名保存する——1 周目に採るテンプレート（`template-<n>.sta`）と、各周の描き上がり
//	（`round-<周>-<n>.vwx`）である（M39。M38 までは 1 周目の「作業ファイル」と、前の周の
//	図面を退避した「捨て場所」だった）。
//	どれも元の図面と同じ大きさがあり（実機で 1 つ 1.7 GB のものがあった）、M38 までは
//	一時ディレクトリの直下へ置いたきり誰も消さなかった（PR #188 の実機確認で 3.4 GB を
//	超えて溜まっていた）。
//
//	【置き場】`<一時ディレクトリ>/homeskz-test/<ブランチ>/`。**ブランチごとに分ける**のは、
//	片付けの単位が「PR が閉じたブランチ」だから（利用者のご要望。PR が close／merge
//	されたら、その PR の実機テストに使った図面はもう要らない）。フォルダ名はブランチ名を
//	綴りの安全な形へ写したもので、写す前の名前は中の目印（kScratchBranchFile）が持つ
//	——写し方は戻せない（`claude/a` も `claude-a` も同じ名前になりうる）ので、GitHub へ
//	問い合わせるときは目印の名前を使う。
//
//	【いつ消すか】**実機テストの周の頭**（メニューからも MCP の `vw_run_test` からも）。
//	いま動いているビルドのブランチ**以外**のフォルダについて GitHub に PR の状態を尋ね
//	（同梱スクリプト `vw-update` の `q-pr-state`）、**開いている PR が無く、閉じた PR が
//	在る**ものだけを消す。問い合わせに失敗した・PR が 1 つも見つからない（＝閉じたとは
//	言えない）ものは残す。**分からなければ消さない側へ倒す。**
//
//	【消してよいもの】ここは CLAUDE.md「開発の基本方針」8 の 3 か所目の「消すコード」で
//	ある。安全弁は removeScratchDir の 1 か所に集め、緩めない:
//	  * 置き場（root）の**直下**のフォルダであること（パスを正規化して確かめる）。
//	  * 中に目印があり、**目印の名前が問い合わせたブランチと一致する**こと。
//	  * 中身が**ふつうのファイルだけ**であること（フォルダ・シンボリックリンクが 1 つでも
//	    あれば触らない。`remove_all` は使わない）。
//	  * **Vectorworks が開いている図面が無い**こと（`*.lck` が在れば触らない——開いている
//	    図面を足元から消さない）。
//
//	【SDK 非依存】標準ライブラリだけで完結するので、無 SDK で単体テストする
//	（tests/CoreFeedbackScratchTests.cpp）。PR の状態を GitHub へ尋ねるのは描画側
//	（draw/Feedback が殻に借りた同梱スクリプト）で、ここはその出力を読むだけ。
//

#pragma once

#include <map>
#include <string>
#include <vector>

namespace HomeskzIfcImport::core
{
	// 一時ディレクトリの直下に作る置き場の名前。
	constexpr const char* kScratchRootName = "homeskz-test";
	// ブランチのフォルダに置く目印（中身は写す前のブランチ名 1 行）。
	constexpr const char* kScratchBranchFile = "branch.txt";

	// ブランチ名をフォルダ名へ写す（英数字と `.` `_` `-` 以外は `-`。空や `.` だけの名前は
	// 前に `b-` を付ける）。同じ名前に写る別のブランチは prepareBranchScratch が番号で
	// 分ける。
	std::string scratchDirName(const std::string& branch);

	// そのブランチのフォルダを用意して絶対パスを返す（無ければ作って目印を書く）。
	// 作れなければ空。root も無ければ作る。
	std::string prepareBranchScratch(const std::string& root, const std::string& branch);

	// 置き場にあるブランチのフォルダ 1 つ。
	struct ScratchDir
	{
		std::string path; // 絶対パス
		std::string branch; // 目印に書いてあるブランチ名（空なら読めなかった）
	};

	// 置き場の直下にある、**目印を持つ**フォルダを名前順に並べる（決定性）。目印の無い
	// フォルダは数えない（自分で作ったものだと言えないので）。
	std::vector<ScratchDir> listScratchDirs(const std::string& root);

	// GitHub 上の、そのブランチの PR の状態。
	enum class PrState
	{
		Unknown, // 尋ねられなかった・答えが読めなかった
		Open,	 // 開いている PR が 1 つでも在る
		Closed,	 // PR は在るが、どれも閉じている（merge を含む）
		None,	 // PR が 1 つも無い
	};

	// 同梱スクリプトの `q-pr-state` の出力（1 行 1 ブランチ。
	// `pr-state<TAB><open|closed|none|error><TAB><branch>`）を読む。読めない行は捨てる。
	// 同じブランチが 2 度出たら**開いている側へ倒す**（消さない側）。
	std::map<std::string, PrState> parsePrStates(const std::string& output);

	// 安全弁つきでブランチのフォルダを消す（冒頭「消してよいもの」）。消したら true、
	// 消さなかったら false と、その理由を why へ。
	bool removeScratchDir(const std::string& root, const ScratchDir& dir, std::string& why);

	// 片付けの顛末。
	struct ScratchCleanup
	{
		std::vector<std::string> removedBranches; // 消したブランチ
		std::vector<std::string> removedPaths; // 消したフォルダ（記憶のテンプレート・図面の照合用）
		std::vector<std::string> kept; // 閉じていたが消さなかったもの（理由つき）
	};

	// candidates（いま動いているブランチを除いたフォルダ）のうち、PR が閉じたものを消す。
	ScratchCleanup cleanUpClosedBranches(const std::string& root,
										 const std::vector<ScratchDir>& candidates,
										 const std::map<std::string, PrState>& states);

	// path が dir の中を指しているか（字面で比べる。記憶のテンプレート・図面を消したかの照合用）。
	bool pathIsInside(const std::string& path, const std::string& dir);

	// 報告と診断ログへ出す 1 行（何もしなかったら空）。
	std::string describeScratchCleanup(const ScratchCleanup& cleanup);
} // namespace HomeskzIfcImport::core
