//
//	CoreFixtureScanTests.cpp
//
//	回帰テストの対象フォルダの走査（src/core/FixtureScan.h）の単体テスト。**Vectorworks も
//	実物の物件も要らない**——ここが押さえるのは「どれを拾い、どれを飛ばし、リンクをどう
//	辿り、同じものをどう畳むか」という規則だけである。
//
//	**ショートカット／エイリアスの解決は偽物を渡す。** 本物は OS の API（Windows の COM /
//	macOS の CoreFoundation。src/draw/Shortcut.h）でしか動かず、Linux の CI では試せない
//	——だから core 側は解決を関数で受け取る形にしてあり、ここではその口に「この名前なら
//	この実体」と答えるだけの偽物を差し込む。**辿ったあとの扱い**（フォルダなら降りる・
//	IFC なら拾う・それ以外は飛ばす）は、それで十分に検証できる。
//

#include "TestFramework.h"
#include "core/FixtureScan.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

using HomeskzIfcImport::core::FixtureEntry;
using HomeskzIfcImport::core::FixtureScan;
using HomeskzIfcImport::core::folderFilePath;
using HomeskzIfcImport::core::kMaxFixtureCount;
using HomeskzIfcImport::core::kMaxFixtureDepth;
using HomeskzIfcImport::core::scanFixtureFolder;
using HomeskzIfcImport::core::ShortcutResolver;

namespace
{
	// テスト 1 件ぶんの作業ディレクトリ（作って、抜けるときに消す）。書き先はビルド
	// ツリー——TMPDIR 由来のパスがファイル操作へ届くと CodeQL が cpp/path-injection として
	// 報告するので、CoreTraceTests / CoreBridgeTests と同じ作法にする。
	class TempDir
	{
	public:
		explicit TempDir(const std::string& tag)
		{
			fPath = std::filesystem::path(HOMESKZ_FIXTURESCAN_TEST_DIR) / ("vw-scan-" + tag);
			std::error_code ec;
			std::filesystem::remove_all(fPath, ec);
			std::filesystem::create_directories(fPath, ec);
		}
		~TempDir()
		{
			std::error_code ec;
			std::filesystem::remove_all(fPath, ec);
		}
		TempDir(const TempDir&) = delete;
		TempDir& operator=(const TempDir&) = delete;

		const std::filesystem::path& path() const
		{
			return fPath;
		}
		std::string utf8() const
		{
			const std::u8string text = fPath.u8string();
			return {text.begin(), text.end()};
		}

		// 中身のあるファイルを 1 つ作る（走査は中身を見ないので、空でなければ何でもよい）。
		std::filesystem::path file(const std::string& relative) const
		{
			const std::filesystem::path target = fPath / relative;
			std::error_code ec;
			std::filesystem::create_directories(target.parent_path(), ec);
			std::ofstream out(target, std::ios::binary | std::ios::trunc);
			out << "ISO-10303-21;\n";
			return target;
		}

		std::filesystem::path dir(const std::string& relative) const
		{
			const std::filesystem::path target = fPath / relative;
			std::error_code ec;
			std::filesystem::create_directories(target, ec);
			return target;
		}

	private:
		std::filesystem::path fPath;
	};

	std::string Utf8Of(const std::filesystem::path& path)
	{
		const std::u8string text = path.u8string();
		return {text.begin(), text.end()};
	}

	// 見出しだけを並べる（期待値は名前で書く——実体のパスはテストごとに変わる）。
	std::vector<std::string> NamesOf(const FixtureScan& scan)
	{
		std::vector<std::string> names;
		names.reserve(scan.entries.size());
		for (const FixtureEntry& entry : scan.entries)
			names.push_back(entry.name);
		return names;
	}

	// 「この名前ならこの実体」と答えるだけの偽の解決関数（本物は src/draw/Shortcut.cpp）。
	ShortcutResolver FakeResolver(const std::map<std::string, std::string>& table)
	{
		return [table](const std::string& path, std::string& target)
		{
			const auto it = table.find(path);
			if (it == table.end())
				return false;
			target = it->second;
			return true;
		};
	}

	// シンボリックリンクを張れたか（Windows のローカル実行では権限が要るので、張れな
	// かったケースは**確かめずに飛ばす**——CI は Linux なので必ず張れる）。
	bool TryLink(const std::filesystem::path& target, const std::filesystem::path& link,
				 bool directory)
	{
		std::error_code ec;
		if (directory)
			std::filesystem::create_directory_symlink(target, link, ec);
		else
			std::filesystem::create_symlink(target, link, ec);
		return !ec;
	}
} // namespace

// ---------------------------------------------------------------------------
// IFC だけを拾い、名前の昇順で返す。
TEST(scan_picks_ifc_in_name_order)
{
	const TempDir temp("plain");
	temp.file("b.ifc");
	temp.file("a.ifc");
	temp.file("c.pdf"); // 物件フォルダには図面も写真も入っている
	temp.file("d.txt"); // 基準ファイルもここに置かれる。拾ってはならない
	temp.file("e.IFC"); // 拡張子の大小は問わない

	const FixtureScan scan = scanFixtureFolder(temp.utf8(), {});
	const std::vector<std::string> names = NamesOf(scan);
	CHECK_EQ(names.size(), std::size_t{3});
	if (names.size() == 3)
	{
		CHECK_EQ(names[0], "a.ifc");
		CHECK_EQ(names[1], "b.ifc");
		CHECK_EQ(names[2], "e.IFC");
	}
	// 実体のパスは、そのまま取り込みへ渡せるもの。
	if (!scan.entries.empty())
		CHECK_EQ(scan.entries[0].path, Utf8Of(temp.path() / "a.ifc"));
}

// 物件ごとのサブフォルダへも降りる（1 フォルダに実体を集め直さなくてよい）。
TEST(scan_descends_into_subfolders)
{
	const TempDir temp("nested");
	temp.file("top.ifc");
	temp.file("案件A/plan.ifc");
	temp.file("案件A/さらに奥/plan2.ifc");

	const FixtureScan scan = scanFixtureFolder(temp.utf8(), {});
	const std::vector<std::string> names = NamesOf(scan);
	CHECK_EQ(names.size(), std::size_t{3});
	if (names.size() == 3)
	{
		CHECK_EQ(names[0], "plan.ifc");
		CHECK_EQ(names[1], "plan2.ifc");
		CHECK_EQ(names[2], "top.ifc");
	}
}

// シンボリックリンク（ファイル・フォルダのどちらも）を辿る。
TEST(scan_follows_symlinks)
{
	const TempDir temp("symlink");
	const std::filesystem::path real = temp.file("実体/物件1.ifc");
	const std::filesystem::path realDir = temp.dir("実体2");
	temp.file("実体2/物件2.ifc");

	const bool fileLink = TryLink(real, temp.path() / "リンク.ifc", /*directory*/ false);
	const bool dirLink = TryLink(realDir, temp.path() / "フォルダリンク", /*directory*/ true);

	const FixtureScan scan = scanFixtureFolder(temp.utf8(), {});
	if (fileLink)
	{
		// **リンクそのものも実体も同じファイルを指す**ので、1 つに畳まれている。
		bool sawLinked = false;
		for (const FixtureEntry& entry : scan.entries)
			sawLinked = sawLinked || entry.name == "リンク.ifc" || entry.name == "物件1.ifc";
		CHECK(sawLinked);
	}
	if (dirLink)
	{
		bool sawThroughDir = false;
		for (const FixtureEntry& entry : scan.entries)
			sawThroughDir = sawThroughDir || entry.name == "物件2.ifc";
		CHECK(sawThroughDir);
	}
}

// Windows のショートカット（.lnk）を辿る——IFC を指すもの・フォルダを指すもの・
// 辿れないものの 3 通り。
TEST(scan_follows_shortcuts)
{
	const TempDir temp("shortcut");
	const std::filesystem::path real = temp.file("外/物件.ifc");
	const std::filesystem::path realDir = temp.dir("外2");
	temp.file("外2/中の物件.ifc");
	temp.file("物件.ifc.lnk");
	temp.file("フォルダ.lnk");
	temp.file("切れている.lnk");

	const std::map<std::string, std::string> table = {
		{Utf8Of(temp.path() / "物件.ifc.lnk"), Utf8Of(real)},
		{Utf8Of(temp.path() / "フォルダ.lnk"), Utf8Of(realDir)},
	};
	const FixtureScan scan = scanFixtureFolder(temp.utf8(), FakeResolver(table));

	const std::vector<std::string> names = NamesOf(scan);
	// 「外」の実体は走査の対象フォルダの中にあるので、ショートカット経由と直接の両方で
	// 見つかるが、**同じ実体なので 1 つに畳まれる**。
	CHECK_EQ(names.size(), std::size_t{2});
	bool sawThroughFolderShortcut = false;
	for (const FixtureEntry& entry : scan.entries)
		sawThroughFolderShortcut = sawThroughFolderShortcut || entry.name == "中の物件.ifc";
	CHECK(sawThroughFolderShortcut);

	// **辿れなかったショートカットは黙って落とさない**（あるはずの物件が回っていない、に
	// 気付ける唯一の手掛かり）。
	bool complained = false;
	for (const std::string& note : scan.notes)
		complained = complained || note.find("切れている.lnk") != std::string::npos;
	CHECK(complained);
}

// macOS の Finder エイリアスは**拡張子が元のまま**なので、`.ifc` に見えるファイルも
// 解決に掛ける。
TEST(scan_resolves_alias_that_looks_like_ifc)
{
	const TempDir temp("alias");
	const std::filesystem::path real = temp.file("実体/本物.ifc");
	temp.file("エイリアス.ifc"); // 中身はエイリアス、という体

	const std::map<std::string, std::string> table = {
		{Utf8Of(temp.path() / "エイリアス.ifc"), Utf8Of(real)},
	};
	const FixtureScan scan = scanFixtureFolder(temp.utf8(), FakeResolver(table));

	// 実体（実体/本物.ifc）とエイリアスは同じものを指すので 1 つ。**見出しは並べた名前**
	// （どちらが先に拾われるかは名前順で決まる: "エイリアス.ifc" < "本物.ifc"）。
	CHECK_EQ(scan.entries.size(), std::size_t{1});
	if (!scan.entries.empty())
	{
		CHECK_EQ(scan.entries[0].name, "エイリアス.ifc");
		CHECK_EQ(scan.entries[0].path, Utf8Of(real));
		CHECK(scan.entries[0].viaShortcut);
	}
}

// リンクの輪に落ちない（同じ実体のフォルダは 1 度しか降りない）。
TEST(scan_stops_on_link_cycles)
{
	const TempDir temp("cycle");
	const std::filesystem::path inner = temp.dir("中");
	temp.file("中/物件.ifc");
	if (!TryLink(temp.path(), inner / "戻る", /*directory*/ true))
		return; // シンボリックリンクを張れない環境（Windows のローカル実行）

	const FixtureScan scan = scanFixtureFolder(temp.utf8(), {});
	CHECK_EQ(scan.entries.size(), std::size_t{1});
}

// 件数の上限で打ち切り、断りを残す（1 件 1 分以上かかるので、際限なく回さない）。
TEST(scan_caps_the_count)
{
	const TempDir temp("cap");
	for (std::size_t i = 0; i < kMaxFixtureCount + 5; ++i)
	{
		std::string name = std::to_string(1000 + i);
		temp.file(name + ".ifc");
	}

	const FixtureScan scan = scanFixtureFolder(temp.utf8(), {});
	CHECK_EQ(scan.entries.size(), kMaxFixtureCount);
	bool complained = false;
	for (const std::string& note : scan.notes)
		complained = complained || note.find("打ち切りました") != std::string::npos;
	CHECK(complained);
}

// 深すぎるところへは降りない（リンクの輪は実体パスで弾くが、深さでも歯止めを持つ）。
TEST(scan_caps_the_depth)
{
	const TempDir temp("depth");
	// 対象フォルダ自身が深さ 0。上限ちょうどの深さには届き、その 1 つ先には届かない。
	std::string shallow;
	for (int i = 0; i < kMaxFixtureDepth; ++i)
		shallow += "d/";
	temp.file(shallow + "届く.ifc");
	temp.file(shallow + "d/届かない.ifc");

	const FixtureScan scan = scanFixtureFolder(temp.utf8(), {});
	const std::vector<std::string> names = NamesOf(scan);
	CHECK_EQ(names.size(), std::size_t{1});
	if (!names.empty())
		CHECK_EQ(names[0], "届く.ifc");
}

// 辿れないものは黙って落とさない（切れたリンク・IFC でもフォルダでもない先を指す
// ショートカット）。**「あるはずの物件が回っていない」に気付ける唯一の手掛かり**である。
TEST(scan_reports_what_it_could_not_follow)
{
	const TempDir temp("dangling");
	temp.file("生きている.ifc");
	temp.file("ただのメモ.txt");
	temp.file("メモへ.lnk");
	const bool linked =
		TryLink(temp.path() / "どこにも無い.ifc", temp.path() / "切れ.ifc", /*directory*/ false);

	// ショートカットの先が IFC でもフォルダでもなければ、回帰の対象ではないので黙って飛ばす。
	const std::map<std::string, std::string> table = {
		{Utf8Of(temp.path() / "メモへ.lnk"), Utf8Of(temp.path() / "ただのメモ.txt")},
	};
	const FixtureScan scan = scanFixtureFolder(temp.utf8(), FakeResolver(table));

	const std::vector<std::string> names = NamesOf(scan);
	CHECK_EQ(names.size(), std::size_t{1});
	if (!names.empty())
		CHECK_EQ(names[0], "生きている.ifc");

	if (linked)
	{
		// 拡張子は .ifc なのに実体へ辿り着けない＝リンク切れ。断りを残す。
		bool complained = false;
		for (const std::string& note : scan.notes)
			complained = complained || note.find("切れ.ifc") != std::string::npos;
		CHECK(complained);
	}
}

// 同じ名前の物件が別のフォルダに在っても、順序が決まる（見出しが同じなら実体のパスで並ぶ）。
TEST(scan_orders_same_names_by_path)
{
	const TempDir temp("samename");
	temp.file("b/同じ名前.ifc");
	temp.file("a/同じ名前.ifc");

	const FixtureScan scan = scanFixtureFolder(temp.utf8(), {});
	CHECK_EQ(scan.entries.size(), std::size_t{2});
	if (scan.entries.size() == 2)
	{
		CHECK_EQ(scan.entries[0].name, scan.entries[1].name);
		CHECK(scan.entries[0].path < scan.entries[1].path);
	}
}

// フォルダの中のパスの組み立て。**フォルダ選択ダイアログが返すパスは末尾に区切りが付く**
// ので、そのまま渡しても `//` にならないことを押さえる。
TEST(folder_file_path_joins_without_doubling)
{
	const TempDir temp("paths");
	const std::string dir = Utf8Of(temp.path());
	const std::string expected = Utf8Of(temp.path() / "基準.txt");

	CHECK_EQ(folderFilePath(dir, "基準.txt"), expected);
	// **末尾に区切りが付いていても同じ結果**（`//` にならない）。
	CHECK_EQ(folderFilePath(dir + "/", "基準.txt"), expected);
	// 空は空（呼び出し側が「決められなかった」と読む）。
	CHECK(folderFilePath("", "基準.txt").empty());
	CHECK(folderFilePath(dir, "").empty());
}

// 走査するフォルダも、末尾に区切りが付いたまま渡せる（呼び出し側に整形を求めない）。
TEST(scan_accepts_a_trailing_separator)
{
	const TempDir temp("trailing");
	temp.file("物件.ifc");

	const FixtureScan scan = scanFixtureFolder(temp.utf8() + "/", {});
	CHECK_EQ(scan.entries.size(), std::size_t{1});
	if (!scan.entries.empty())
		CHECK_EQ(scan.entries[0].name, "物件.ifc");

	// 読めないフォルダの断りには、末尾の区切りを落とした名前が出る（空にならない）。
	const FixtureScan gone = scanFixtureFolder(Utf8Of(temp.path() / "無い") + "/", {});
	CHECK(gone.entries.empty());
	CHECK_EQ(gone.notes.size(), std::size_t{1});
	if (!gone.notes.empty())
		CHECK(gone.notes[0].find("無い") != std::string::npos);
}

// 無いフォルダ・空の指定は、例外ではなく断りで返る。
TEST(scan_reports_a_missing_folder)
{
	const FixtureScan empty = scanFixtureFolder("", {});
	CHECK(empty.entries.empty());
	CHECK(!empty.notes.empty());

	const TempDir temp("missing");
	const FixtureScan gone = scanFixtureFolder(Utf8Of(temp.path() / "無い"), {});
	CHECK(gone.entries.empty());
	CHECK(!gone.notes.empty());
}

TEST_MAIN();
