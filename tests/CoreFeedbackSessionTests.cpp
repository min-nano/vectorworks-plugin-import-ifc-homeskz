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
		session.autoContinue = true;
		session.anonymize = false;
		session.round = 3;
		session.lastCommit = "a1b2c3d";
		session.lastTally = "ストーリ:3/3,通り芯:44/44";
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
	CHECK(!session.autoContinue);
	CHECK(session.anonymize); // **公開される側が既定**。伏せるほうを既定にする。
	CHECK_EQ(session.round, 0);
	CHECK_EQ(session.pullRequest, 0);
	CHECK(session.ifcPath.empty());
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
	CHECK_EQ(after.autoContinue, before.autoContinue);
	CHECK_EQ(after.anonymize, before.anonymize);
	CHECK_EQ(after.round, before.round);
	CHECK_EQ(after.lastCommit, before.lastCommit);
	CHECK_EQ(after.lastTally, before.lastTally);
	// 取り込み設定も 1 周目のまま運ばれる（ここが落ちると 2 周目が別の条件で走る）。
	for (std::size_t i = 0; i < kSymbolRoleCount; ++i)
	{
		const auto role = static_cast<SymbolRole>(i);
		CHECK_EQ(after.options.symbol(role), before.options.symbol(role));
		CHECK_EQ(after.options.isEnabled(role), before.options.isEnabled(role));
	}
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
							 "auto=off\n";
	const FeedbackSession session = parseFeedbackSession(text);
	CHECK_EQ(session.pullRequest, 77);
	CHECK(session.send);
	CHECK(!session.autoContinue);
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

TEST_MAIN();
