//
//	CoreFeedbackScratchTests.cpp
//
//	実機テストの一時ファイルの置き場と片付け（src/core/FeedbackScratch.h）の単体テスト。
//	**ここは利用者の計算機でファイルを消すコード**（CLAUDE.md「開発の基本方針」8）なので、
//	消すことよりも**消さない場面**を重点的に確認する——置き場の外・目印の不一致・
//	フォルダやリンクが混じる・Vectorworks が開いている・PR が閉じたと断定できない。
//

#include "TestFramework.h"
#include "core/FeedbackScratch.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using HomeskzIfcImport::core::cleanUpClosedBranches;
using HomeskzIfcImport::core::describeScratchCleanup;
using HomeskzIfcImport::core::kScratchBranchFile;
using HomeskzIfcImport::core::listScratchDirs;
using HomeskzIfcImport::core::parsePrStates;
using HomeskzIfcImport::core::pathIsInside;
using HomeskzIfcImport::core::prepareBranchScratch;
using HomeskzIfcImport::core::PrState;
using HomeskzIfcImport::core::removeScratchDir;
using HomeskzIfcImport::core::ScratchCleanup;
using HomeskzIfcImport::core::ScratchDir;
using HomeskzIfcImport::core::scratchDirName;

namespace
{
	namespace fs = std::filesystem;

	// テスト 1 件分の置き場（作成し、スコープを抜けるときに削除する）。**一時ディレクトリを
	// 使わない**（CoreBridgeTests と同じ理由: CodeQL の cpp/path-injection）。
	class TempRoot
	{
	public:
		explicit TempRoot(const std::string& tag)
		{
			fPath = fs::path(HOMESKZ_SCRATCH_TEST_DIR) / ("vw-scratch-test-" + tag);
			std::error_code ec;
			fs::remove_all(fPath, ec);
			fs::create_directories(fPath / "homeskz-test", ec);
		}
		~TempRoot()
		{
			std::error_code ec;
			fs::remove_all(fPath, ec);
		}
		TempRoot(const TempRoot&) = delete;
		TempRoot& operator=(const TempRoot&) = delete;

		// 置き場（homeskz-test）。
		std::string root() const
		{
			return (fPath / "homeskz-test").string();
		}
		// 置き場の外（隣）。
		std::string outside() const
		{
			return fPath.string();
		}

	private:
		fs::path fPath;
	};

	void touch(const fs::path& path, const std::string& text = "x")
	{
		std::ofstream out(path, std::ios::binary);
		out << text;
	}

	ScratchDir only(const std::string& root)
	{
		const std::vector<ScratchDir> dirs = listScratchDirs(root);
		return dirs.size() == 1 ? dirs.front() : ScratchDir{};
	}
} // namespace

TEST(scratch_dir_name_keeps_only_safe_characters)
{
	CHECK_EQ(scratchDirName("claude/mcp-local-dev-loop"), std::string("claude-mcp-local-dev-loop"));
	CHECK_EQ(scratchDirName("feature/a b:c@d"), std::string("feature-a-b-c-d"));
	// 置き場そのものや外を指す名前にしない。
	CHECK_EQ(scratchDirName(""), std::string("b-"));
	CHECK_EQ(scratchDirName(".."), std::string("b-.."));
	CHECK_EQ(scratchDirName("."), std::string("b-."));
}

TEST(prepare_branch_scratch_creates_a_marked_folder_and_reuses_it)
{
	const TempRoot temp("prepare");
	const std::string dir = prepareBranchScratch(temp.root(), "claude/x");
	CHECK(!dir.empty());
	CHECK(fs::is_directory(dir));
	CHECK(fs::exists(fs::path(dir) / kScratchBranchFile));
	// 2 度目は同じフォルダ。
	CHECK_EQ(prepareBranchScratch(temp.root(), "claude/x"), dir);
	const ScratchDir listed = only(temp.root());
	CHECK_EQ(listed.branch, std::string("claude/x"));
}

TEST(prepare_branch_scratch_separates_branches_that_map_to_the_same_name)
{
	// `claude/a` と `claude-a` はどちらも `claude-a` に変換される。目印で区別して別のフォルダへ。
	const TempRoot temp("collide");
	const std::string first = prepareBranchScratch(temp.root(), "claude/a");
	const std::string second = prepareBranchScratch(temp.root(), "claude-a");
	CHECK(!first.empty());
	CHECK(!second.empty());
	CHECK(first != second);
	CHECK_EQ(prepareBranchScratch(temp.root(), "claude-a"), second);
	CHECK_EQ(listScratchDirs(temp.root()).size(), static_cast<std::size_t>(2));
}

TEST(prepare_branch_scratch_gives_up_when_it_cannot_create_the_folder)
{
	const TempRoot temp("cannot");
	// 置き場が空・置き場の場所にファイルが既にある・フォルダの場所にファイルが既にある。
	CHECK(prepareBranchScratch("", "claude/x").empty());
	touch(fs::path(temp.outside()) / "not-a-dir");
	CHECK(prepareBranchScratch((fs::path(temp.outside()) / "not-a-dir").string(), "claude/x")
			  .empty());
	touch(fs::path(temp.root()) / "claude-x");
	CHECK(prepareBranchScratch(temp.root(), "claude/x").empty());
}

TEST(list_scratch_dirs_reads_a_marker_written_with_crlf)
{
	// Windows で手作業で編集された目印（CRLF）でも、ブランチ名に CR を混ぜない。
	const TempRoot temp("crlf");
	const fs::path dir = fs::path(temp.root()) / "claude-crlf";
	fs::create_directories(dir);
	touch(dir / kScratchBranchFile, "claude/crlf\r\n");
	CHECK_EQ(only(temp.root()).branch, std::string("claude/crlf"));
}

TEST(list_scratch_dirs_ignores_folders_without_a_marker_and_sorts_by_name)
{
	const TempRoot temp("list");
	(void)prepareBranchScratch(temp.root(), "zeta");
	(void)prepareBranchScratch(temp.root(), "alpha");
	fs::create_directories(fs::path(temp.root()) / "someone-elses"); // 目印なし
	touch(fs::path(temp.root()) / "loose.vwx");						 // フォルダでない
	const std::vector<ScratchDir> dirs = listScratchDirs(temp.root());
	CHECK_EQ(dirs.size(), static_cast<std::size_t>(2));
	if (dirs.size() == 2)
	{
		CHECK_EQ(dirs[0].branch, std::string("alpha"));
		CHECK_EQ(dirs[1].branch, std::string("zeta"));
	}
	// 置き場が無くても空で返る。
	CHECK(listScratchDirs(temp.root() + "-missing").empty());
}

TEST(parse_pr_states_reads_the_script_lines)
{
	const auto states = parsePrStates("pr-state\topen\tclaude/a\n"
									  "pr-state\tclosed\tclaude/b\r\n"
									  "pr-state\tnone\tclaude/c\n"
									  "pr-state\terror\tclaude/d\n"
									  "garbage line\n"
									  "pr-state\topen\n"
									  "pr-state\tclosed\t\n"
									  "error=GitHub が HTTP 500 を返しました。\n");
	CHECK_EQ(states.size(), static_cast<std::size_t>(4));
	CHECK(states.at("claude/a") == PrState::Open);
	CHECK(states.at("claude/b") == PrState::Closed);
	CHECK(states.at("claude/c") == PrState::None);
	CHECK(states.at("claude/d") == PrState::Unknown);
}

TEST(parse_pr_states_leans_towards_keeping_on_conflicting_lines)
{
	// 同じブランチに矛盾する答えが出たら、消さない側を採る（順序に依らない）。
	CHECK(parsePrStates("pr-state\tclosed\tb\npr-state\topen\tb\n").at("b") == PrState::Open);
	CHECK(parsePrStates("pr-state\topen\tb\npr-state\tclosed\tb\n").at("b") == PrState::Open);
	CHECK(parsePrStates("pr-state\tclosed\tb\npr-state\terror\tb\n").at("b") == PrState::Unknown);
}

TEST(remove_scratch_dir_removes_a_plain_marked_folder)
{
	const TempRoot temp("remove");
	const std::string dir = prepareBranchScratch(temp.root(), "claude/done");
	touch(fs::path(dir) / "work-1.vwx");
	touch(fs::path(dir) / "round-2-1.vwx");
	std::string why;
	CHECK(removeScratchDir(temp.root(), only(temp.root()), why));
	CHECK(why.empty());
	CHECK(!fs::exists(dir));
	// 置き場そのものは残す。
	CHECK(fs::is_directory(temp.root()));
}

TEST(remove_scratch_dir_refuses_while_vectorworks_has_a_drawing_open)
{
	const TempRoot temp("lock");
	const std::string dir = prepareBranchScratch(temp.root(), "claude/open");
	touch(fs::path(dir) / "work-1.vwx");
	touch(fs::path(dir) / ".work-1.lck");
	std::string why;
	CHECK(!removeScratchDir(temp.root(), only(temp.root()), why));
	CHECK(!why.empty());
	// 1 つも消していない。
	CHECK(fs::exists(fs::path(dir) / "work-1.vwx"));
}

TEST(remove_scratch_dir_refuses_folders_with_anything_but_plain_files)
{
	const TempRoot temp("nested");
	const std::string dir = prepareBranchScratch(temp.root(), "claude/nested");
	touch(fs::path(dir) / "work-1.vwx");
	fs::create_directories(fs::path(dir) / "sub");
	std::string why;
	CHECK(!removeScratchDir(temp.root(), only(temp.root()), why));
	CHECK(fs::exists(fs::path(dir) / "work-1.vwx"));

	// シンボリックリンクも（リンク先を消しに行かない）。作成できない環境では省略する。
	const TempRoot linked("symlink");
	const std::string ldir = prepareBranchScratch(linked.root(), "claude/link");
	touch(fs::path(linked.outside()) / "precious.vwx");
	std::error_code ec;
	fs::create_symlink(fs::path(linked.outside()) / "precious.vwx", fs::path(ldir) / "link.vwx",
					   ec);
	if (!ec)
	{
		CHECK(!removeScratchDir(linked.root(), only(linked.root()), why));
		CHECK(fs::exists(fs::path(linked.outside()) / "precious.vwx"));
	}
}

TEST(remove_scratch_dir_refuses_a_mismatched_marker_or_a_folder_outside_the_root)
{
	const TempRoot temp("mismatch");
	const std::string dir = prepareBranchScratch(temp.root(), "claude/real");
	touch(fs::path(dir) / "work-1.vwx");
	std::string why;
	// 目印と違うブランチを名乗っても消さない。
	CHECK(!removeScratchDir(temp.root(), ScratchDir{dir, "claude/other"}, why));
	CHECK(fs::exists(dir));
	// 置き場の直下ではないフォルダ（目印を真似ていても）は消さない。
	const fs::path stray = fs::path(temp.outside()) / "stray";
	fs::create_directories(stray);
	touch(stray / kScratchBranchFile, "claude/stray\n");
	CHECK(!removeScratchDir(temp.root(), ScratchDir{stray.string(), "claude/stray"}, why));
	CHECK(fs::exists(stray));
	// `..` で置き場の外へ出る道も。
	const std::string sneaky = temp.root() + "/../stray";
	CHECK(!removeScratchDir(temp.root(), ScratchDir{sneaky, "claude/stray"}, why));
	CHECK(fs::exists(stray));
	// 空のブランチ名（目印が読めなかった）は消さない。
	CHECK(!removeScratchDir(temp.root(), ScratchDir{dir, ""}, why));
	CHECK(fs::exists(dir));
	// 目印の無いフォルダは、名乗りが何であれ消さない。
	const fs::path unmarked = fs::path(temp.root()) / "unmarked";
	fs::create_directories(unmarked);
	touch(unmarked / "work-1.vwx");
	CHECK(!removeScratchDir(temp.root(), ScratchDir{unmarked.string(), "claude/x"}, why));
	CHECK(fs::exists(unmarked / "work-1.vwx"));
	// フォルダでないもの（ファイル・無いパス）も。
	CHECK(!removeScratchDir(temp.root(), ScratchDir{(unmarked / "work-1.vwx").string(), "x"}, why));
	CHECK(!removeScratchDir(temp.root(), ScratchDir{temp.root() + "/missing", "x"}, why));
}

TEST(remove_scratch_dir_stops_without_partial_damage_when_permissions_refuse)
{
	// 権限で読めない・消せないフォルダは、途中まで消して壊さずに止まる。**root で実行すると
	// 権限が機能しない**ので、そのときは確認できず省略する（CI のランナーは root ではない）。
	const auto perms = [](const std::string& path, fs::perms p)
	{
		std::error_code ec;
		fs::permissions(path, p, fs::perm_options::replace, ec);
	};
	const fs::perms all = fs::perms::owner_all;
	std::string why;

	// 読めない（中身を数えられない）。
	const TempRoot unreadable("unreadable");
	const std::string dir1 = prepareBranchScratch(unreadable.root(), "claude/unreadable");
	touch(fs::path(dir1) / "work-1.vwx");
	perms(dir1, fs::perms::owner_write | fs::perms::owner_exec);
	std::error_code probe;
	const fs::directory_iterator readable(dir1, probe);
	if (probe)
	{
		CHECK(!removeScratchDir(unreadable.root(), ScratchDir{dir1, "claude/unreadable"}, why));
		perms(dir1, all);
		CHECK(fs::exists(fs::path(dir1) / "work-1.vwx"));
	}
	perms(dir1, all);

	// 書けない（中のファイルを消せない）。
	const TempRoot locked("locked");
	const std::string dir2 = prepareBranchScratch(locked.root(), "claude/locked");
	touch(fs::path(dir2) / "work-1.vwx");
	perms(dir2, fs::perms::owner_read | fs::perms::owner_exec);
	const ScratchDir target2{dir2, "claude/locked"};
	if (!removeScratchDir(locked.root(), target2, why))
	{
		perms(dir2, all);
		CHECK(!why.empty());
		CHECK(fs::exists(fs::path(dir2) / kScratchBranchFile)); // 目印は残る＝次の周でまた候補
	}
	perms(dir2, all);

	// 目印しか残っていないのに消せない（最後の 1 枚で止まる）。
	const TempRoot last("last");
	const std::string dir3 = prepareBranchScratch(last.root(), "claude/last");
	perms(dir3, fs::perms::owner_read | fs::perms::owner_exec);
	if (!removeScratchDir(last.root(), ScratchDir{dir3, "claude/last"}, why))
	{
		perms(dir3, all);
		CHECK(!why.empty());
		CHECK(fs::exists(dir3));
	}
	perms(dir3, all);
}

TEST(clean_up_removes_only_branches_whose_pr_is_closed)
{
	const TempRoot temp("cleanup");
	const std::string closed = prepareBranchScratch(temp.root(), "claude/closed");
	const std::string open = prepareBranchScratch(temp.root(), "claude/open");
	const std::string none = prepareBranchScratch(temp.root(), "claude/none");
	const std::string unknown = prepareBranchScratch(temp.root(), "claude/unknown");
	const std::string busy = prepareBranchScratch(temp.root(), "claude/busy");
	touch(fs::path(busy) / ".work-1.lck");
	const auto states = parsePrStates("pr-state\tclosed\tclaude/closed\n"
									  "pr-state\topen\tclaude/open\n"
									  "pr-state\tnone\tclaude/none\n"
									  "pr-state\tclosed\tclaude/busy\n");
	const ScratchCleanup result =
		cleanUpClosedBranches(temp.root(), listScratchDirs(temp.root()), states);
	CHECK_EQ(result.removedBranches.size(), static_cast<std::size_t>(1));
	CHECK(!fs::exists(closed));
	CHECK(fs::exists(open));
	CHECK(fs::exists(none));
	CHECK(fs::exists(unknown)); // 答えが無いものも残す
	CHECK(fs::exists(busy));	// 閉じていても開いている図面があれば残す
	CHECK_EQ(result.kept.size(), static_cast<std::size_t>(1));

	const std::string note = describeScratchCleanup(result);
	CHECK(note.find("claude/closed") != std::string::npos);
	CHECK(note.find("claude/busy") != std::string::npos);
	// 何もしなかった周は 1 行も増やさない。
	CHECK(describeScratchCleanup(ScratchCleanup{}).empty());
}

TEST(path_is_inside_compares_whole_components)
{
	CHECK(pathIsInside("/tmp/homeskz-test/a/work-1.vwx", "/tmp/homeskz-test/a"));
	CHECK(pathIsInside("/tmp/homeskz-test/a/work-1.vwx", "/tmp/homeskz-test/a/"));
	// 名前の頭が同じだけの隣のフォルダは中ではない。
	CHECK(!pathIsInside("/tmp/homeskz-test/ab/work-1.vwx", "/tmp/homeskz-test/a"));
	CHECK(!pathIsInside("/tmp/homeskz-test/a", "/tmp/homeskz-test/a"));
	CHECK(!pathIsInside("", "/tmp"));
}

TEST_MAIN();
