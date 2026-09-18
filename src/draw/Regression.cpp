//
//	draw/Regression.cpp
//
//	回帰テストの運転（意図と制約は draw/Regression.h）。**絵を作るところは本番と同じ**
//	（draw/ImportRun の runImportRound）で、ここが持つのは「どの順で・何件・どう戻して
//	走らせるか」と「基準との突き合わせ」だけである。
//
//	**数え方・言い方はこのファイルに書かない。** 結末も内訳も差分も文言も無 SDK 側
//	（parse/Regression）が持ち、ここは受け取った文字列を出すだけ（完了ダイアログと同じ
//	分担。parse/Summary.h）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "draw/Regression.h"

#include "core/FixtureScan.h"
#include "core/ImportOptions.h"
#include "core/Trace.h"
#include "draw/DocumentFile.h"
#include "draw/ImportRun.h"
#include "draw/ResultDialog.h"
#include "draw/SettingsDialog.h"
#include "draw/Shortcut.h"
#include "parse/Regression.h"
#include "parse/Summary.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// **このコマンド自身の言葉で言う。** 取り込みの完了文言も実機テストの文言も
		// 借りない——借りると、押した人には別のコマンドが動いているように見える
		// （実機の指摘。docs/DEV-NOTES.md M25）。
		constexpr const char* kRegressionTitle = "回帰テスト";

		// 診断ログの置き場所。**本番の取り込みログとは別のファイル**にする——取り込みは
		// 1 件ごとにログを開き直す（＝前の件の中身は残らない）ので、同じ場所へ書くと
		// 総括まで最後の 1 件に上書きされる。
		constexpr const char* kRegressionLogName = "min-nano_structure-regression.log";

		// **1 件と 1 件のあいだに、作業ファイルを開き直して図面を取り込み前へ戻す。**
		// 作法は実機フィードバックの周とまったく同じ（draw/Feedback.cpp の
		// openRoundDocument）——前の件の絵が載った文書を**別の捨て場所へ保存し直してから
		// 閉じ**、作業ファイルを開き直す。基準のファイルには前の件の絵が 1 つも書き戻らない
		// ので、「保存して閉じる」がそのまま「変更を破棄する」になる（未保存の変更がある
		// 文書は `CloseDocument()` が false で閉じられない、という実測への答えでもある。
		// [SDK リファレンス「Documents」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Documents.md)）。
		//
		// 戻れなかったら false。**そのときは走らせない**——前の件に重ねて描くと、数字は
		// 揃うのに絵が壊れる（実機 round 9 で「どこにも属さない状態で描いて全要素 0 件」
		// になった。docs/DEV-NOTES.md M25）。note には何が起きたかを**証拠つきで**入れる。
		bool resetToWorkDocument(const std::string& workPath, std::size_t index, std::string& note)
		{
			note.clear();
			const std::string active = ActiveDocumentPath();
			std::string parkedNote;
			if (SamePath(active, workPath))
			{
				const std::string parked =
					FreshTempPath("homeskz-regression-" + std::to_string(index));
				if (!SaveActiveDocumentAs(parked))
				{
					note = "前の件の図面を退避できませんでした（" +
						   (parked.empty() ? std::string("一時ディレクトリが引けません") : parked) +
						   "）";
					return false;
				}
				// **`CloseDocument` の戻り値で分岐しない。** false を返しても実際には
				// 閉じていることがある（実機 round 9）。閉じられたかは下の読み戻しで判る。
				const bool closeReturned = gSDK->CloseDocument();
				parkedNote = "前の件の図面は " + parked + " へ移しました（閉じる=";
				parkedNote += closeReturned ? "true" : "false";
				parkedNote += "）";
			}
			else
			{
				// 人が別の図面へ移ったあと。**その図面には触らない**（閉じない）。
				parkedNote = "前の件の図面はアクティブではありませんでした（開いたまま残します）";
			}

			const bool returned = OpenDocumentAt(workPath);
			// **戻り値だけを信じない**——開いたかどうかはカレント文書を読み戻して確かめる。
			const std::string opened = ActiveDocumentPath();
			if (SamePath(opened, workPath))
			{
				note = "作業ファイルを開き直しました。" + parkedNote;
				return true;
			}
			note = "作業ファイルを開き直せませんでした（" + workPath + " / OpenDocumentPath=";
			note += returned ? "true" : "false";
			note += " / いまは " + (opened.empty() ? std::string("(取得できず)") : opened) + "）。";
			note += parkedNote;
			return false;
		}

		// 結果を見せる（ダイアログを組めなければ素のアラートへ落とす。draw/ResultDialog.h）。
		void showRegressionResult(const std::string& body)
		{
			if (!draw::showImportResult(kRegressionTitle, body, core::trace::text()))
				gSDK->AlertInform(body.c_str(), "", false);
		}

		// 走らせずに終える（理由を 1 枚で伝える）。**黙って何も起きないと「壊れている」と
		// 読まれる。**
		void bailOut(const std::string& text, const std::string& advice)
		{
			gSDK->AlertInform(text.c_str(), advice.c_str(), false);
		}
	} // namespace

	// -----------------------------------------------------------------------
	void runRegressionCommand()
	{
		// 1. 対象フォルダ。**そのフォルダの中の IFC を 1 つ選んでもらう**（draw/Regression.h）。
		std::string picked;
		if (!chooseFile("回帰テストのフォルダの中にある IFC を 1 つ選択", "ifc",
						"IFC ファイル (*.ifc)", picked))
			return;
		const std::string folder = core::parentFolderOf(picked);
		if (folder.empty())
		{
			bailOut("フォルダが分かりませんでした。", picked);
			return;
		}

		// 2. 走査。ショートカット・エイリアスの解決は OS の API が持つ（draw/Shortcut.h）。
		const core::FixtureScan scan = core::scanFixtureFolder(folder, &resolveShortcut);
		if (scan.entries.empty())
		{
			std::string why = folder;
			for (const std::string& note : scan.notes)
				why += "\n" + note;
			bailOut("そのフォルダに IFC が見つかりませんでした。", why);
			return;
		}

		// 3. 取り込み設定は **1 回だけ**。全件に同じ設定を使う（設定が件ごとに違えば、
		//    基準と引き比べても何が動いたのか分からない）。
		core::ImportOptions options;
		std::string settingsNote;
		const draw::SettingsOutcome settings = draw::showImportSettings(options, &settingsNote);
		if (settings == draw::SettingsOutcome::Cancelled)
			return;
		const bool settingsShown = settings == draw::SettingsOutcome::Accepted;

		// 4. 基準を読み、**走らせる前に**尋ね切る（draw/Regression.h「尋ねるのは始まる前だけ」）。
		const std::string baselinePath = parse::regressionBaselinePath(folder);
		std::string baselineText;
		(void)parse::readRegressionBaseline(baselinePath, baselineText);
		const std::vector<parse::RegressionEntry> baseline =
			parse::parseRegressionBaseline(baselineText);
		const std::string origin = parse::regressionBaselineOrigin(baselineText);
		const bool haveBaseline = !origin.empty() && !baseline.empty();

		// **基準が「あること」と「使えること」を混ぜない。** 中身が 1 件も無い基準ファイル
		// （途中で壊れた・手で空にした）を「あり」として引き比べると、全件が「基準に無し」に
		// なって読む側を惑わせる。そのときは 1 回目として扱う。
		const std::string prompt = parse::formatRegressionPrompt(
			scan.entries.size(), baseline.size(), haveBaseline ? origin : std::string());
		// AlertQuestion は 0=キャンセル / 1=OK / 2,3=追加のボタン（src/Updater.cpp と同じ
		// 作法）。基準が無いときは「作る」しか選べないので、追加のボタンは出さない。
		const short answer =
			gSDK->AlertQuestion(prompt.c_str(), "",
								/*defaultButton*/ 1, haveBaseline ? "引き比べる" : "始める",
								"やめる", haveBaseline ? "引き比べて基準を更新" : "",
								/*customButtonB*/ "");
		if (answer != 1 && answer != 2)
			return;
		// 基準が無ければ必ず書く（それが 1 回目の意味）。あるときは選ばれたときだけ。
		const bool updateBaseline = !haveBaseline || answer == 2;

		// 5. 作業ファイルを採る。**採れなければ走らせない**（draw/Regression.h）。
		const std::string workPath = FreshTempPath("homeskz-regression-work");
		if (workPath.empty() || !SaveActiveDocumentAs(workPath))
		{
			bailOut("作業ファイルを用意できませんでした。",
					(workPath.empty() ? std::string("一時ディレクトリが引けません") : workPath) +
						"\n（いま開いている図面を複製できないと、1 件ごとに図面を"
						"取り込み前へ戻せません）");
			return;
		}

		// 6. 1 件ずつ。**1 件が例外で落ちても次へ進む**——1 件の欠損で全体を止めない
		//    （CLAUDE.md「エラーハンドリング」）。中止（進捗ダイアログのキャンセル）だけは
		//    そこで畳む——人が「もうよい」と言っているのに、残りを回す理由が無い。
		std::vector<parse::RegressionEntry> current;
		current.reserve(scan.entries.size());
		bool cancelled = false;
		bool documentLost = false;
		std::string documentNote;
		for (std::size_t i = 0; i < scan.entries.size(); ++i)
		{
			const core::FixtureEntry& fixture = scan.entries[i];
			std::string prologue = "準備: ";
			if (i == 0)
			{
				prologue +=
					"いま開いている図面を作業ファイルとして保存しました（" + workPath + "）";
			}
			else if (!resetToWorkDocument(workPath, i, documentNote))
			{
				// **描く先が無いなら取り込まない。** ここで止めて、何が起きたかを残す。
				documentLost = true;
				break;
			}
			else
			{
				prologue += documentNote;
			}
			if (fixture.viaShortcut)
				prologue += "。ショートカットを辿りました（" + fixture.path + "）";

			// 進捗ダイアログの見出しに「何件目か」を出す（draw/ImportRun.h）。
			const std::string progressTitle =
				"回帰テスト " + std::to_string(i + 1) + "/" + std::to_string(scan.entries.size());
			const ImportRound round = runImportRound(fixture.path, options, settingsShown,
													 settingsNote, prologue, progressTitle);
			if (round.failed)
			{
				// **落ちた 1 件も記録に残す。** 基準と引き比べれば「前は通っていた」が
				// すぐ分かる——それがこの仕組みで一番拾いたい退行である。
				current.push_back(
					parse::regressionErrorEntry(fixture.name, round.body, round.seconds));
				continue;
			}
			current.push_back(
				parse::regressionEntry(fixture.name, round.document, round.counts, round.seconds));
			if (round.counts.cancelled)
			{
				cancelled = true;
				break;
			}
		}

		// 7. 突き合わせと記録。
		const std::vector<parse::RegressionCompare> compares =
			parse::compareRegression(baseline, current);
		// **最後まで走らなかったのは、中止も「戻せなかった」も同じ**——残りが走っていない
		// ことに変わりはないので、同じ扱いにする（基準も書き換えない）。
		const parse::RegressionSummary summary =
			parse::summarizeRegression(compares, haveBaseline, cancelled || documentLost);

		bool baselineWritten = false;
		std::string baselineNote;
		if (updateBaseline && !cancelled && !documentLost)
		{
			baselineWritten = parse::writeRegressionBaseline(
				baselinePath, parse::formatRegressionBaseline(current, currentBuildInfo(),
															  core::trace::localTimestamp()));
			baselineNote = baselineWritten ? ("基準を書きました: " + baselinePath)
										   : ("基準を書けませんでした: " + baselinePath);
		}
		else if (updateBaseline)
		{
			// **最後まで走らなかった回で基準を書き換えない。** 走っていない件が「消えた」
			// ことにされ、次の回から差分が読めなくなる。
			baselineNote = "最後まで走らなかったので、基準はそのままにしました";
		}

		// 8. ログは**本番の取り込みとは別のファイル**へ（上の kRegressionLogName）。ここで
		//    開き直すと、取り込み 1 件ごとに溜まっていた本文は捨てられる——結果ダイアログの
		//    ログ欄に出したいのは総括のほうである（件ごとの詳しいログが要るなら、その 1 件を
		//    本番の取り込みで走らせ直す）。
		core::trace::open(core::trace::defaultLogPath(kRegressionLogName));
		core::trace::note(parse::formatLogHeader(
			currentBuildInfo(), folder, 0, core::trace::localTimestamp(), core::trace::path()));
		core::trace::note(parse::formatImportOptions(options));
		if (!baselineNote.empty())
			core::trace::note(baselineNote);
		if (documentLost)
			core::trace::note("中断: " + documentNote);
		core::trace::note(parse::formatRegressionLog(compares, summary, scan.notes));
		core::trace::note("作業ファイル: " + workPath);
		core::trace::close();

		std::string body = parse::formatRegressionResult(summary, baselineWritten);
		if (documentLost)
			body = "図面を取り込み前へ戻せなかったので、途中で止めました。\n" + body;
		showRegressionResult(body);
	}
} // namespace HomeskzIfcImport::draw
