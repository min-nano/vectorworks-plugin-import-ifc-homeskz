//
//	CoreFeedbackSessionTests.cpp
//
//	実機テストが周をまたいで持ち越すもの（src/core/FeedbackSession）と、MCP の
//	`vw_run_test` の settings の当て方（applyTestSettings）の単体テスト。VectorWorks SDK を
//	一切 include せず、無 SDK のテストハーネスで実行する（CLAUDE.md「テスト方針」）。
//
//	検証項目: 自分で保存した図面の記録が書いて読んで元に戻ること・M42 までの記憶の条件の行を
//	読み飛ばすこと・ファイルへの読み書き・**閉じてよい図面の判定（利用者の図面を閉じない
//	安全弁）**・settings の各項目と、読めない settings を断ること（M43）。
//

#include "TestFramework.h"

#include "core/FeedbackSession.h"
#include "core/ImportOptions.h"
#include "core/Json.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using HomeskzIfcImport::core::applyTestSettings;
using HomeskzIfcImport::core::clearFeedbackSession;
using HomeskzIfcImport::core::defaultFeedbackSessionPath;
using HomeskzIfcImport::core::FeedbackSession;
using HomeskzIfcImport::core::formatFeedbackSession;
using HomeskzIfcImport::core::ImportOptions;
using HomeskzIfcImport::core::isOwnedTestDocument;
using HomeskzIfcImport::core::Json;
using HomeskzIfcImport::core::parseFeedbackSession;
using HomeskzIfcImport::core::PlanLevelKey;
using HomeskzIfcImport::core::readFeedbackSession;
using HomeskzIfcImport::core::SymbolRole;
using HomeskzIfcImport::core::testReportPathFor;
using HomeskzIfcImport::core::writeFeedbackSession;

namespace
{
	// 自分で保存した図面を 2 つ記録したもの。
	FeedbackSession sample()
	{
		FeedbackSession session;
		session.ownedDocuments = {"/tmp/homeskz-test/main/template-1.sta",
								  "/tmp/homeskz-test/main/round-3.vwx"};
		return session;
	}

	// テスト用の書き出し先（同じ名前を使い回さない）。
	std::string tempPath(const char* name)
	{
		std::error_code ec;
		const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
		return (dir / name).string();
	}

	// 環境変数の設定と解除。**置き場所は環境変数だけで決まる**ので、これが無いと
	// その分岐（Windows 流・macOS 流・どちらも取得できない）を確認できない。綴りが
	// 処理系で異なるのでここに閉じ込める（core/Trace が getenv を 1 か所へ閉じ込めて
	// いるのと同じ）。
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

	// 環境変数を 1 つ保存し、スコープを抜けるときに元へ戻す（他のテストへ影響させない）。
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

	// JSON テキストから settings を作る（読めなければ null を返し、呼び出し側の照合で
	// 失敗させる——null の settings は「上書きしない」なので、読めないテキストを渡した
	// テストは既定のままの値と照合して落ちる）。
	Json settingsFrom(const std::string& text)
	{
		Json value;
		std::string error;
		if (!Json::parse(text, value, error))
			return Json::null();
		return value;
	}
} // namespace

TEST(feedback_session_defaults_to_nothing_to_close)
{
	const FeedbackSession session;
	CHECK(session.ownedDocuments.empty());
	CHECK(parseFeedbackSession("").ownedDocuments.empty());
}

TEST(feedback_session_round_trips_through_text)
{
	// **自分で保存した図面**（M39）。欠けると次の周で閉じられず、周の数だけ増えていく
	// （再起動のときに保存の確認が並ぶ）。
	const FeedbackSession before = sample();
	const FeedbackSession after = parseFeedbackSession(formatFeedbackSession(before));
	CHECK_EQ(after.ownedDocuments.size(), before.ownedDocuments.size());
	for (std::size_t i = 0; i < before.ownedDocuments.size(); ++i)
		CHECK_EQ(after.ownedDocuments[i], before.ownedDocuments[i]);
}

TEST(feedback_session_drops_the_remembered_conditions_of_m41)
{
	// M42 までの記憶（条件・前の周の内訳・基準のレイヤ・PR の行・作業ファイル）。**通知せずに
	// 読み飛ばし、自分で保存した図面だけを読む**——更新しただけで前の周の図面を閉じられなく
	// なると、再起動のときに保存の確認が並ぶ。
	const FeedbackSession session = parseFeedbackSession(
		"version=1\nifc=/tmp/a.ifc\nround=4\nbuild=a1b2c3d\ntally=柱:2/2\n"
		"template=/tmp/homeskz-test/main/template-1.sta\n"
		"owned.doc=/tmp/homeskz-test/main/round-4-1.vwx\nbaseline=1\nbaseline.layer=共通\n"
		"role.0.symbol=アンカーボルト_M12\nrole.0.on=1\ntitleblock=図面枠 A3\n"
		"send=1\npr=137\nwork=/tmp/homeskz-test/main/work-1.vwx\ncreated.layer=1-伏図\n");
	CHECK_EQ(session.ownedDocuments.size(), std::size_t(1));
	CHECK_EQ(session.ownedDocuments[0], std::string("/tmp/homeskz-test/main/round-4-1.vwx"));
	// 書き直すと古い行は残らない。
	const std::string text = formatFeedbackSession(session);
	for (const char* key : {"ifc=", "round=", "template=", "baseline", "role.",
							"titleblock=", "send=", "work=", "created."})
		CHECK(text.find(key) == std::string::npos);
}

TEST(feedback_session_parse_skips_broken_lines)
{
	// 見出し・空行・"=" の無い行・空の値は通知せずに読み飛ばし、読める行だけを取得する。
	const FeedbackSession session = parseFeedbackSession("# コメント\n"
														 "\n"
														 "イコールがまったく無い行\n"
														 "owned.doc=   \n"
														 "owned.doc= /tmp/a.vwx \r\n");
	CHECK_EQ(session.ownedDocuments.size(), std::size_t(1));
	CHECK_EQ(session.ownedDocuments[0], std::string("/tmp/a.vwx"));
}

TEST(feedback_session_file_round_trip)
{
	const std::string path = tempPath("homeskz-feedback-test.txt");
	clearFeedbackSession(path);

	FeedbackSession missing;
	CHECK(!readFeedbackSession(path, missing)); // 無ければ false（＝閉じる相手が無い）

	CHECK(writeFeedbackSession(path, sample()));
	FeedbackSession loaded;
	CHECK(readFeedbackSession(path, loaded));
	CHECK_EQ(loaded.ownedDocuments.size(), std::size_t(2));

	clearFeedbackSession(path);
	FeedbackSession gone;
	CHECK(!readFeedbackSession(path, gone));
	// 二度削除しても異常終了しない。
	clearFeedbackSession(path);
}

TEST(feedback_session_write_reports_a_place_it_cannot_write)
{
	// 書けなくても取り込みは続けられるので、**例外ではなく false**。ファイルの下のパスは、
	// ディレクトリとしても作れないので確実に失敗する。
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
	// **置き場所は環境変数だけで決まる。** ここが誤ると、前の周の図面を閉じられないまま
	// 周の数だけ溜まる。
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
		// どれも取れない環境では諦める（呼び出し側は記録を使わない）。
		const ScopedEnv custom("HOMESKZ_IFC_FEEDBACK_STATE", nullptr);
		const ScopedEnv local("LOCALAPPDATA", nullptr);
		const ScopedEnv home("HOME", nullptr);
		CHECK(defaultFeedbackSessionPath().empty());
	}
}

TEST(feedback_session_empty_path_is_refused)
{
	// 置き場所が決まらない環境では、通知なく諦める。
	FeedbackSession session;
	CHECK(!readFeedbackSession("", session));
	CHECK(!writeFeedbackSession("", session));
	clearFeedbackSession("");
}

TEST(test_report_sits_next_to_the_record)
{
	// 報告は記録と同じフォルダに置く（MCP の vw_test_report が本体を入れ替えたあとも読める）。
	const std::string report = testReportPathFor("/Users/hanako/Library/Application Support/"
												 "HomeskzIfcImport/feedback.txt");
	CHECK_EQ(std::filesystem::path(report).filename().string(), std::string("last-round.md"));
	CHECK_EQ(std::filesystem::path(report).parent_path().filename().string(),
			 std::string("HomeskzIfcImport"));
	// 置き場所が分からなければ空（呼び出し側は報告を書かずに結末へ添える）。
	CHECK(testReportPathFor("").empty());
	// 区切りの無いパス（環境変数でファイル名だけを渡された）なら、同じ場所に置く。
	CHECK_EQ(testReportPathFor("feedback.txt"), std::string("last-round.md"));
}

// ---------------------------------------------------------------------------
// **閉じてよい図面か**（M39。core/FeedbackSession.h の isOwnedTestDocument）。
// `CloseDocument()` は確認なしに変更を捨てるので、ここが利用者の図面を守る安全弁になる。

TEST(owned_test_document_is_the_one_saved_in_the_scratch_root)
{
	FeedbackSession session;
	session.ownedDocuments = {"/tmp/homeskz-test/main/round-2-1.vwx"};
	CHECK(
		isOwnedTestDocument(session, "/tmp/homeskz-test/main/round-2-1.vwx", "/tmp/homeskz-test"));
	// 名指しに無い図面は、置き場の中でも閉じない。
	CHECK(
		!isOwnedTestDocument(session, "/tmp/homeskz-test/main/round-3-1.vwx", "/tmp/homeskz-test"));
	// 利用者の図面は閉じない。
	CHECK(!isOwnedTestDocument(session, "/Users/someone/物件A.vwx", "/tmp/homeskz-test"));
	// 空のパス・置き場が分からないときは閉じない。
	CHECK(!isOwnedTestDocument(session, "", "/tmp/homeskz-test"));
	CHECK(!isOwnedTestDocument(session, "/tmp/homeskz-test/main/round-2-1.vwx", ""));
}

TEST(owned_test_document_outside_the_scratch_root_is_never_closed)
{
	// **記憶が置き場の外を指していたら、一致していても閉じない**（壊れた記憶・手で書き
	// 換えた記憶から利用者の図面へ届かせない）。
	FeedbackSession session;
	session.ownedDocuments = {"/Users/someone/物件A.vwx", "/tmp/homeskz-test-other/x.vwx"};
	CHECK(!isOwnedTestDocument(session, "/Users/someone/物件A.vwx", "/tmp/homeskz-test"));
	CHECK(!isOwnedTestDocument(session, "/tmp/homeskz-test-other/x.vwx", "/tmp/homeskz-test"));
}

TEST(owned_test_document_matches_another_spelling_of_the_same_file)
{
	// 字面が違っても同じファイルなら同じとみなす（std::filesystem::equivalent）。ここでは
	// "." を挟んだ綴りで確認する（実在するファイルが要る）。macOS の一時ディレクトリは
	// /var と /private/var の 2 通りで返るため。
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path root = fs::temp_directory_path(ec) / "homeskz-owned-doc-test";
	fs::create_directories(root / "main", ec);
	const fs::path file = root / "main" / "round-1-1.vwx";
	{
		std::ofstream(file) << "x";
	}
	FeedbackSession session;
	session.ownedDocuments = {file.string()};
	const std::string other = (root / "main" / "." / "round-1-1.vwx").string();
	CHECK(isOwnedTestDocument(session, other, root.string()));
	fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// **MCP の `vw_run_test` の settings**（M43。core/FeedbackSession.h の applyTestSettings）。
// 既定（テンプレートの図面から組んだもの）に、書かれた項目だけを上書きする。

TEST(test_settings_absent_leaves_the_defaults)
{
	ImportOptions options;
	options.setTitleBlockStyle("図面枠 A3");
	std::string error;
	CHECK(applyTestSettings(Json::null(), options, error));
	CHECK_EQ(options.titleBlockStyle(), std::string("図面枠 A3"));
	CHECK(applyTestSettings(Json::object(), options, error));
	CHECK_EQ(options.titleBlockStyle(), std::string("図面枠 A3"));
}

TEST(test_settings_override_every_item_of_the_dialog)
{
	ImportOptions options;
	options.setTitleBlockStyle("図面枠 A3");
	std::string error;
	CHECK(applyTestSettings(settingsFrom(R"js({"symbols":{"床束":"床束（特注）","火打":false,)js"
										 R"js("仕口":true,"継手":""},)js"
										 R"js("title_block":"","dimension":"構造図 寸法",)js"
										 R"js("merge_levels":["1:3531","2:6374"],)js"
										 R"js("skip_sections":["X1","1(2)"],)js"
										 R"js("rafter":{"width":60.5,"height":105}})js"),
							options, error));
	CHECK_EQ(error, std::string());
	// 文字列はそのシンボルで取り込む・false と空文字列は取り込まない・true は既定のまま取り込む。
	CHECK_EQ(options.symbol(SymbolRole::FloorPost), std::string("床束（特注）"));
	CHECK(options.isEnabled(SymbolRole::FloorPost));
	CHECK(!options.isEnabled(SymbolRole::FireBrace));
	CHECK(options.isEnabled(SymbolRole::Joint));
	CHECK_EQ(options.symbol(SymbolRole::Joint), std::string("仕口"));
	CHECK(!options.isEnabled(SymbolRole::Splice));
	// 空文字列は「置かない／入れない」。
	CHECK(!options.hasTitleBlock());
	CHECK_EQ(options.dimensionStandard(), std::string("構造図 寸法"));
	CHECK(options.mergesWithPrevious(PlanLevelKey{1, 3531}));
	CHECK(options.mergesWithPrevious(PlanLevelKey{2, 6374}));
	CHECK_EQ(options.mergedPlanLevels.size(), std::size_t(2));
	CHECK(options.isSectionSkipped("X1"));
	CHECK(options.isSectionSkipped("1(2)"));
	CHECK(std::abs(options.rafterWidth - 60.5) < 1e-9);
	CHECK(std::abs(options.rafterHeight - 105.0) < 1e-9);
}

TEST(test_settings_rafter_keeps_the_side_that_is_not_given)
{
	ImportOptions options;
	std::string error;
	CHECK(applyTestSettings(settingsFrom(R"js({"rafter":{"height":90}})js"), options, error));
	CHECK(std::abs(options.rafterWidth - 45.0) < 1e-9);
	CHECK(std::abs(options.rafterHeight - 90.0) < 1e-9);
}

TEST(test_settings_refuse_what_they_cannot_read)
{
	// **頼んだ条件と違う条件で黙って走らせない**——知らない項目・型の違う値・範囲外の値は
	// 理由を付けて断る。
	for (const char* text : {
			 R"js([])js",
			 R"js({"symbol":{}})js",
			 R"js({"symbols":{"存在しない役割":"x"}})js",
			 R"js({"symbols":{"床束":1}})js",
			 R"js({"symbols":[]})js",
			 R"js({"title_block":false})js",
			 R"js({"dimension":null})js",
			 R"js({"merge_levels":["1"]})js",
			 R"js({"merge_levels":["x:1"]})js",
			 R"js({"merge_levels":"1:3531"})js",
			 R"js({"skip_sections":[1]})js",
			 R"js({"rafter":{"width":0}})js",
			 R"js({"rafter":{"width":"45"}})js",
			 R"js({"rafter":{"depth":45}})js",
		 })
	{
		const Json settings = settingsFrom(text);
		CHECK(!settings.isNull()); // テキスト自体は JSON として読める
		ImportOptions options;
		std::string error;
		CHECK(!applyTestSettings(settings, options, error));
		CHECK(!error.empty());
	}
}

TEST_MAIN();
