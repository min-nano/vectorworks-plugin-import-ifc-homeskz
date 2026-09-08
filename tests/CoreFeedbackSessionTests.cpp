//
//	CoreFeedbackSessionTests.cpp
//
//	実機フィードバックの記憶（src/core/FeedbackSession）の単体テスト。VectorWorks SDK を
//	一切 include せず、無 SDK のテストハーネスで走る（CLAUDE.md「テスト方針」）。
//
//	検証項目（docs/DEV-NOTES.md M23）: 既定は「何もしない」・書いて読んで元に戻る・
//	壊れた行を飛ばして読み続ける・ファイルへの読み書き。**2 周目が走るかどうかはこの
//	記憶だけに懸かっている**ので、往復の要はここを壊さないこと。
//

#include "TestFramework.h"

#include "core/FeedbackSession.h"
#include "core/ImportOptions.h"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using HomeskzIfcImport::core::clearFeedbackSession;
using HomeskzIfcImport::core::defaultFeedbackSessionPath;
using HomeskzIfcImport::core::FeedbackRoundKind;
using HomeskzIfcImport::core::feedbackRoundKind;
using HomeskzIfcImport::core::FeedbackSession;
using HomeskzIfcImport::core::formatFeedbackSession;
using HomeskzIfcImport::core::kSymbolRoleCount;
using HomeskzIfcImport::core::parseFeedbackSession;
using HomeskzIfcImport::core::readFeedbackSession;
using HomeskzIfcImport::core::SymbolRole;
using HomeskzIfcImport::core::writeFeedbackSession;

namespace
{
	// 一通り埋めた記憶（往復で実際に運ぶ値の全部）。
	FeedbackSession sample()
	{
		FeedbackSession session;
		session.send = true;
		session.repo = "min-nano/vectorworks-plugin-import-ifc-homeskz";
		session.pullRequest = 123;
		session.branch = "claude/plugin-feedback-automation-01bi93";
		session.ifcPath = "/Users/someone/Documents/物件A.ifc";
		session.anonymize = false;
		session.round = 3;
		session.lastCommit = "a1b2c3d";
		session.lastTally = "ストーリ:3/3,通り芯:44/44";
		session.lastPostedAt = "2026-09-07T01:02:03Z";
		session.loop = true;
		session.baselineRecorded = true;
		session.baselineLayers = {"共通", "デザイン レイヤ-1"};
		session.lastCreatedLayers = {"1-伏図", "2-伏図"};
		session.lastCreatedSheets = {"A-1", "A-2"};
		session.options.setSymbol(SymbolRole::FloorPost, "床束（特注）");
		session.options.setEnabled(SymbolRole::FireBrace, false);
		return session;
	}

	// テスト用の書き出し先（同じ名前を使い回さない）。
	std::string tempPath(const char* name)
	{
		std::error_code ec;
		const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
		return (dir / name).string();
	}

	// 環境変数の付け外し。**置き場所の決め方は環境変数だけで決まる**ので、これが無いと
	// その分岐（Windows 流・macOS 流・どちらも取れない）を確かめられない。綴りが処理系で
	// 違うのでここに閉じ込める（core/Trace が getenv を 1 か所へ閉じ込めているのと同じ）。
	void setEnv(const char* name, const char* value)
	{
#if defined(_WIN32)
		_putenv_s(name, value != nullptr ? value : "");
#else
		if (value != nullptr)
			setenv(name, value, 1);
		else
			unsetenv(name);
#endif
	}

	// 環境変数を 1 つ預かって、抜けるときに元へ戻す（他のテストへ漏らさない）。
	class ScopedEnv
	{
	public:
		ScopedEnv(const char* name, const char* value) : fName(name)
		{
			const char* const previous = std::getenv(name);
			fHad = previous != nullptr;
			if (fHad)
				fPrevious = previous;
			setEnv(name, value);
		}
		~ScopedEnv()
		{
			setEnv(fName, fHad ? fPrevious.c_str() : nullptr);
		}
		ScopedEnv(const ScopedEnv&) = delete;
		ScopedEnv& operator=(const ScopedEnv&) = delete;

	private:
		const char* fName;
		std::string fPrevious;
		bool fHad = false;
	};
} // namespace

TEST(feedback_session_defaults_do_nothing)
{
	// 記憶が無いとき（＝1 周目）にそのまま使っても、従来どおりの手動の取り込みになる。
	const FeedbackSession session;
	CHECK(!session.send);
	CHECK(session.anonymize); // **公開される側が既定**。伏せるほうを既定にする。
	CHECK_EQ(session.round, 0);
	CHECK_EQ(session.pullRequest, 0);
	CHECK(session.ifcPath.empty());
}

TEST(feedback_session_without_a_baseline_reads_as_not_recorded)
{
	// 古い版が書いた記憶（baseline の行が無い）。**空の基準と取り違えない**——
	// 取り違えると、基準を採る前の周を「戻っています」と報告してしまう。
	const FeedbackSession session = parseFeedbackSession("round=2\nbuild=a1b2c3d\n");
	CHECK(!session.baselineRecorded);
	CHECK(session.baselineLayers.empty());
	// 古い版には「前の周が作ったレイヤ」の行も無い。**空＝消す相手が分からない**ので、
	// そのときは図面に触らない（draw/Feedback の prepareDrawingForRound）。
	CHECK(session.lastCreatedLayers.empty());
	CHECK(session.lastCreatedSheets.empty());
}

TEST(feedback_session_keeps_an_empty_baseline_distinct_from_none)
{
	// まっさらな図面で始めた 1 周目は「基準は採ったが 0 枚」。真偽が無いと区別できない。
	FeedbackSession empty;
	empty.baselineRecorded = true;
	const FeedbackSession after = parseFeedbackSession(formatFeedbackSession(empty));
	CHECK(after.baselineRecorded);
	CHECK(after.baselineLayers.empty());
}

TEST(feedback_session_round_trips_through_text)
{
	const FeedbackSession before = sample();
	const FeedbackSession after = parseFeedbackSession(formatFeedbackSession(before));

	CHECK_EQ(after.send, before.send);
	CHECK_EQ(after.repo, before.repo);
	CHECK_EQ(after.pullRequest, before.pullRequest);
	CHECK_EQ(after.branch, before.branch);
	CHECK_EQ(after.ifcPath, before.ifcPath);
	CHECK_EQ(after.anonymize, before.anonymize);
	CHECK_EQ(after.round, before.round);
	CHECK_EQ(after.lastCommit, before.lastCommit);
	CHECK_EQ(after.lastTally, before.lastTally);
	// モードレスの往復（M24）が持ち越すもの: 直近の投稿の時刻と、回っているか。
	CHECK_EQ(after.lastPostedAt, before.lastPostedAt);
	CHECK_EQ(after.loop, before.loop);
	// 1 周目に採った基準（レイヤの顔ぶれ）。**ここが落ちると次の周で図面が戻っているかを
	// 判定できなくなる**（テンプレートのレイヤと前の周の残りを区別できない）。
	CHECK_EQ(after.baselineRecorded, before.baselineRecorded);
	CHECK_EQ(after.baselineLayers.size(), before.baselineLayers.size());
	for (std::size_t i = 0; i < before.baselineLayers.size(); ++i)
		CHECK_EQ(after.baselineLayers[i], before.baselineLayers[i]);
	// **前の周が作ったレイヤ**（M25）。次の周の前にこれ**だけ**を図面から取り除くので、
	// ここが落ちると「消してよいもの」を見失う——見失ったまま別の基準で消す作りに
	// してはならない（利用者が足したレイヤを巻き込む）。デザインとシートは混ぜない
	// （消す順序が違う: シートが先）。
	CHECK_EQ(after.lastCreatedLayers.size(), before.lastCreatedLayers.size());
	for (std::size_t i = 0; i < before.lastCreatedLayers.size(); ++i)
		CHECK_EQ(after.lastCreatedLayers[i], before.lastCreatedLayers[i]);
	CHECK_EQ(after.lastCreatedSheets.size(), before.lastCreatedSheets.size());
	for (std::size_t i = 0; i < before.lastCreatedSheets.size(); ++i)
		CHECK_EQ(after.lastCreatedSheets[i], before.lastCreatedSheets[i]);
	// 取り込み設定も 1 周目のまま運ばれる（ここが落ちると 2 周目が別の条件で走る）。
	for (std::size_t i = 0; i < kSymbolRoleCount; ++i)
	{
		const auto role = static_cast<SymbolRole>(i);
		CHECK_EQ(after.options.symbol(role), before.options.symbol(role));
		CHECK_EQ(after.options.isEnabled(role), before.options.isEnabled(role));
	}
}

TEST(feedback_session_without_loop_lines_reads_as_not_looping)
{
	// 古い版（M23）が書いた記憶には posted / loop の行が無い。**自動の往復は回って
	// いない**と読むのが正しい——立てて読むと、古い記憶でパレットが勝手に回り出す。
	const FeedbackSession session = parseFeedbackSession("send=1\nround=2\nbuild=a1b2c3d\n");
	CHECK(!session.loop);
	CHECK(session.lastPostedAt.empty());
}

TEST(feedback_session_parse_skips_broken_lines)
{
	// 見出し・空行・"=" の無い行・知らないキー・番号にならない／範囲外の役割は黙って
	// 飛ばし、読める行だけを拾う（古い版が書いたファイルで往復を止めない）。
	const std::string text = "# コメント\n"
							 "\n"
							 "イコールがまったく無い行\n"
							 "unknown=なにか\n"
							 "role.99.symbol=存在しない役割\n"
							 "role.x.on=1\n"
							 "roleでもドットが続かない=1\n"
							 "pr=77\n"
							 "send=yes\n"
							 "auto=off\n"; // 昔の版が書いた行。知らない鍵は黙って飛ばす
	const FeedbackSession session = parseFeedbackSession(text);
	CHECK_EQ(session.pullRequest, 77);
	CHECK(session.send);
}

TEST(feedback_session_parse_keeps_defaults_for_unreadable_values)
{
	// 真偽にならない綴り・空の数・桁あふれは**既定のまま**（0 に潰さない・例外を投げない）。
	const FeedbackSession session = parseFeedbackSession("send=たぶん\n"
														 "anon=たぶん\n"
														 "pr=\n"
														 "round=99999999999\n");
	CHECK(!session.send);	  // 既定（false）のまま
	CHECK(session.anonymize); // 既定（true）のまま
	CHECK_EQ(session.pullRequest, 0);
	CHECK_EQ(session.round, 0);
}

TEST(feedback_session_parse_trims_blank_values)
{
	// 値が空白だけの行は「空」として読む（前後の空白を落とすので何も残らない）。
	const FeedbackSession session = parseFeedbackSession("repo=   \nbranch= feature/x \n");
	CHECK(session.repo.empty());
	CHECK_EQ(session.branch, std::string("feature/x"));
}

TEST(feedback_session_parse_ignores_bad_numbers)
{
	// 数字でない周回数・PR 番号は既定のまま（例外を投げない・0 に潰さない）。
	const FeedbackSession session = parseFeedbackSession("pr=abc\nround=-1\n");
	CHECK_EQ(session.pullRequest, 0);
	CHECK_EQ(session.round, 0);
}

TEST(feedback_session_reads_crlf)
{
	// Windows で手直しされたファイル（CRLF）も読める。
	const FeedbackSession session = parseFeedbackSession("pr=5\r\nbranch=feature/x\r\n");
	CHECK_EQ(session.pullRequest, 5);
	CHECK_EQ(session.branch, std::string("feature/x"));
}

TEST(feedback_session_file_round_trip)
{
	const std::string path = tempPath("homeskz-feedback-test.txt");
	clearFeedbackSession(path);

	FeedbackSession missing;
	CHECK(!readFeedbackSession(path, missing)); // 無ければ false（＝1 周目）

	CHECK(writeFeedbackSession(path, sample()));
	FeedbackSession loaded;
	CHECK(readFeedbackSession(path, loaded));
	CHECK_EQ(loaded.pullRequest, 123);
	CHECK_EQ(loaded.ifcPath, std::string("/Users/someone/Documents/物件A.ifc"));

	clearFeedbackSession(path);
	FeedbackSession gone;
	CHECK(!readFeedbackSession(path, gone));
	// 二度消しても落ちない。
	clearFeedbackSession(path);
}

TEST(feedback_session_write_reports_a_place_it_cannot_write)
{
	// 書けなくても往復は続けられる（2 周目が走らないだけ）ので、**例外ではなく false**。
	// ファイルの下のパスは、ディレクトリとしても作れないので確実に失敗する。
	const std::string file = tempPath("homeskz-feedback-not-a-dir.txt");
	{
		std::ofstream out(file, std::ios::trunc);
		out << "これはファイルであってディレクトリではない\n";
	}
	CHECK(!writeFeedbackSession(file + "/child/feedback.txt", sample()));
	std::error_code ec;
	std::filesystem::remove(file, ec);
}

TEST(feedback_session_default_path_follows_the_platform)
{
	// **置き場所は環境変数だけで決まる。** 一時ディレクトリには置かない（消えると
	// 2 周目が走らない）ので、ここが狂うと往復が静かに 1 周で終わる。
	{
		// 差し替え（試験用）が最優先。
		const ScopedEnv custom("HOMESKZ_IFC_FEEDBACK_STATE", "/tmp/custom-feedback.txt");
		CHECK_EQ(defaultFeedbackSessionPath(), std::string("/tmp/custom-feedback.txt"));
	}
	{
		// Windows は %LOCALAPPDATA% の下。
		const ScopedEnv custom("HOMESKZ_IFC_FEEDBACK_STATE", nullptr);
		const ScopedEnv local("LOCALAPPDATA", "C:\\Users\\Taro\\AppData\\Local");
		CHECK_EQ(defaultFeedbackSessionPath(),
				 std::string("C:\\Users\\Taro\\AppData\\Local\\HomeskzIfcImport\\feedback.txt"));
	}
	{
		// macOS は $HOME/Library/Application Support の下。
		const ScopedEnv custom("HOMESKZ_IFC_FEEDBACK_STATE", nullptr);
		const ScopedEnv local("LOCALAPPDATA", nullptr);
		const ScopedEnv home("HOME", "/Users/hanako");
		CHECK_EQ(defaultFeedbackSessionPath(),
				 std::string("/Users/hanako/Library/Application Support/HomeskzIfcImport/"
							 "feedback.txt"));
	}
	{
		// どれも取れない環境では諦める（呼び出し側は記憶を持たずに 1 周で終わる）。
		const ScopedEnv custom("HOMESKZ_IFC_FEEDBACK_STATE", nullptr);
		const ScopedEnv local("LOCALAPPDATA", nullptr);
		const ScopedEnv home("HOME", nullptr);
		CHECK(defaultFeedbackSessionPath().empty());
	}
}

TEST(feedback_session_empty_path_is_refused)
{
	// 置き場所が決まらない環境では、黙って諦める（往復は 1 周で終わる）。
	FeedbackSession session;
	CHECK(!readFeedbackSession("", session));
	CHECK(!writeFeedbackSession("", session));
	clearFeedbackSession("");
}

// ---------------------------------------------------------------------------
// **実機テストの周がどれになるか**（M25。core/FeedbackSession.h の feedbackRoundKind）。
// この場合分けが M25 の要点なので、描画側に散らさず無 SDK でここに固定する。

namespace
{
	// 「1 周投稿できた」記憶（この 3 つが揃って初めて続きの周を組み立てられる）。
	FeedbackSession postedOnce()
	{
		FeedbackSession session;
		session.send = true;
		session.round = 1;
		session.ifcPath = "/tmp/model.ifc";
		session.lastCommit = "aaaaaaa";
		return session;
	}
} // namespace

TEST(feedback_round_kind_continues_when_a_new_build_is_running)
{
	// 記憶があり、動いているビルドがそれと違う——**これだけが続きの周**である。
	CHECK(feedbackRoundKind(postedOnce(), "bbbbbbb", /*allowDialogs*/ true) ==
		  FeedbackRoundKind::ContinueRound);
	// パレットの周（ダイアログ禁止）でも同じ。ここへ来るのは新しいビルドを入れた直後だけ。
	CHECK(feedbackRoundKind(postedOnce(), "bbbbbbb", /*allowDialogs*/ false) ==
		  FeedbackRoundKind::ContinueRound);
}

TEST(feedback_round_kind_does_not_import_again_on_the_same_build)
{
	// **同じビルドでは取り込まない。** 取り込んでも前の周と同じ数字が並ぶだけで、その
	// 1 分は最初から無駄である。M24 まではここを「新しい 1 周目」として取り込み直して
	// おり、パレットが開いている最中にメニューを押した人が同じ round を二重に投稿した
	// （実機で発生。docs/DEV-NOTES.md M25）。
	CHECK(feedbackRoundKind(postedOnce(), "aaaaaaa", /*allowDialogs*/ true) ==
		  FeedbackRoundKind::RearmOnly);
}

TEST(feedback_round_kind_starts_a_first_round_without_memory)
{
	CHECK(feedbackRoundKind(FeedbackSession{}, "aaaaaaa", /*allowDialogs*/ true) ==
		  FeedbackRoundKind::FirstRound);

	// **記憶として使えるのは 3 つ揃っているときだけ。** どれが欠けても 1 周目に戻る
	// ——欠けたまま「続き」を組み立てると、宛先も IFC も無いまま走ることになる。
	FeedbackSession notSending = postedOnce();
	notSending.send = false;
	CHECK(feedbackRoundKind(notSending, "bbbbbbb", true) == FeedbackRoundKind::FirstRound);

	FeedbackSession neverPosted = postedOnce();
	neverPosted.round = 0;
	CHECK(feedbackRoundKind(neverPosted, "bbbbbbb", true) == FeedbackRoundKind::FirstRound);

	FeedbackSession noFile = postedOnce();
	noFile.ifcPath.clear();
	CHECK(feedbackRoundKind(noFile, "bbbbbbb", true) == FeedbackRoundKind::FirstRound);
}

TEST(feedback_round_kind_refuses_when_it_would_have_to_ask)
{
	// パレットの周はダイアログを 1 枚も出せない。尋ねないと始められない場面では
	// **何もしない**——黙ってファイル選択を出すことも、勝手に始めることもしない。
	CHECK(feedbackRoundKind(FeedbackSession{}, "aaaaaaa", /*allowDialogs*/ false) ==
		  FeedbackRoundKind::Refuse);
	CHECK(feedbackRoundKind(postedOnce(), "aaaaaaa", /*allowDialogs*/ false) ==
		  FeedbackRoundKind::Refuse);
}

TEST_MAIN();
