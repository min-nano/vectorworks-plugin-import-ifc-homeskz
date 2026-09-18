//
//	core/FixtureScan.cpp
//
//	回帰テストの対象フォルダの走査（意図と制約は core/FixtureScan.h）。
//
//	**`file_size` / `last_write_time` は使わない。** MSVC の <filesystem> はその中でフラグの
//	enum を範囲外へキャストしており、clang-tidy の静的解析がそれを我々の呼び出しの経路として
//	報告する（src/PayloadHost.cpp と src/core/Bridge.cpp に同じ但し書きがある）。ここが要るのは
//	「ディレクトリか・普通のファイルか・同じ実体か」だけなので、どちらも使わずに済む。
//
//	**パスの往復は UTF-8 で行う**（`u8string` / `std::u8string` からの構築）。Windows の
//	`path::string()` は**実行環境のコードページ**へ落とすので、物件名に日本語が入っていると
//	そこで壊れる——このリポジトリが相手にする物件名は、ほぼ必ず日本語である。
//

#include "core/FixtureScan.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::core
{
	namespace
	{
		namespace fs = std::filesystem;

		// UTF-8 の文字列 → パス。**`path(std::string)` を直に使わない**（Windows では
		// コードページ扱いになる。ファイル冒頭の但し書き）。
		fs::path PathFromUtf8(const std::string& text)
		{
			return {std::u8string(text.begin(), text.end())};
		}

		// パス → UTF-8 の文字列（上の逆）。
		std::string Utf8Of(const fs::path& path)
		{
			const std::u8string text = path.u8string();
			return {text.begin(), text.end()};
		}

		// 小文字に畳んだ拡張子（".ifc" / ".lnk" / 空）。**ASCII だけを畳む**——拡張子の
		// 比較にしか使わないので、日本語を含むところへ手を出す理由が無い。
		std::string LowerExtension(const fs::path& path)
		{
			std::string ext = Utf8Of(path.extension());
			for (char& c : ext)
			{
				if (c >= 'A' && c <= 'Z')
					c = static_cast<char>(c - 'A' + 'a');
			}
			return ext;
		}

		// 同じ実体かを判じるための鍵。**実在すれば正規化した絶対パス**、辿れなければ
		// 素のパスを綴りだけ整えたもの（判定が甘くなるだけで、壊れはしない）。
		std::string IdentityOf(const fs::path& path)
		{
			std::error_code ec;
			const fs::path resolved = fs::weakly_canonical(path, ec);
			if (!ec && !resolved.empty())
				return Utf8Of(resolved);
			return Utf8Of(path.lexically_normal());
		}

		// 走査の途中で持ち回る状態。**関数を跨いで同じものを渡す**ので 1 つに畳んである
		// （引数が増えるほど、どれが入力でどれが出力か読めなくなる）。
		struct Walk
		{
			FixtureScan result;
			// これから降りるフォルダ（実体パスと深さ）。
			std::vector<std::pair<fs::path, int>> pending;
			// もう降りたフォルダ（リンクの輪を踏まないため）。
			std::set<std::string> visitedDirs;
			// もう拾った IFC（実体が同じものを 2 度取り込まないため）。
			std::set<std::string> seenFiles;
			bool capped = false;
		};

		// フォルダを降りる先に足す（既に降りた・深すぎるなら足さない）。
		void PushDirectory(Walk& walk, const fs::path& dir, int depth)
		{
			if (depth > kMaxFixtureDepth)
			{
				walk.result.notes.push_back("深いので降りませんでした: " + Utf8Of(dir.filename()));
				return;
			}
			const std::string key = IdentityOf(dir);
			if (!walk.visitedDirs.insert(key).second)
				return; // 同じ実体をもう見ている（リンクの輪もここで止まる）
			walk.pending.emplace_back(dir, depth);
		}

		// IFC を 1 件拾う（同じ実体を 2 度拾わない）。
		void PushFixture(Walk& walk, const fs::path& target, const std::string& name,
						 bool viaShortcut)
		{
			if (walk.result.entries.size() >= kMaxFixtureCount)
			{
				walk.capped = true;
				return;
			}
			const std::string key = IdentityOf(target);
			if (!walk.seenFiles.insert(key).second)
			{
				walk.result.notes.push_back("同じファイルを指していたので 1 つにまとめました: " +
											name);
				return;
			}
			FixtureEntry entry;
			entry.path = Utf8Of(target);
			entry.name = name;
			entry.viaShortcut = viaShortcut;
			walk.result.entries.push_back(std::move(entry));
		}

		// ショートカット／エイリアスを解決する（解決関数が無ければ常に false）。
		bool Resolve(const ShortcutResolver& resolve, const fs::path& path, fs::path& target)
		{
			if (!resolve)
				return false;
			std::string resolved;
			if (!resolve(Utf8Of(path), resolved) || resolved.empty())
				return false;
			target = PathFromUtf8(resolved);
			return true;
		}

		// 子 1 つを仕分ける（フォルダなら降りる先へ、IFC なら拾う、それ以外は飛ばす）。
		void ClassifyChild(Walk& walk, const ShortcutResolver& resolve, const fs::path& child,
						   int depth)
		{
			std::error_code ec;
			// **シンボリックリンクとジャンクションはここで透過する**（`is_directory` /
			// `is_regular_file` はリンクを辿った先を見る）。
			if (fs::is_directory(child, ec) && !ec)
			{
				PushDirectory(walk, child, depth + 1);
				return;
			}

			const std::string name = Utf8Of(child.filename());
			const std::string ext = LowerExtension(child);
			const bool couldBeShortcut = ext == ".ifc" || ext == ".lnk" || ext.empty();
			if (!couldBeShortcut)
				return; // 物件フォルダには PDF も画像も入っている。黙って飛ばす

			// **`.ifc` にも解決を掛ける。** macOS の Finder エイリアスは**元の拡張子を
			// 持ったまま**なので、拡張子では素のファイルと見分けられない（Windows 側の
			// 実装はここで必ず false を返すので、無駄になるのは呼び出し 1 回だけ）。
			fs::path target;
			if (Resolve(resolve, child, target))
			{
				std::error_code targetEc;
				if (fs::is_directory(target, targetEc) && !targetEc)
				{
					PushDirectory(walk, target, depth + 1);
					return;
				}
				if (LowerExtension(target) == ".ifc")
				{
					PushFixture(walk, target, name, /*viaShortcut*/ true);
					return;
				}
				// IFC でもフォルダでもないものを指すショートカット。回帰の対象ではない。
				return;
			}

			if (ext == ".ifc")
			{
				std::error_code fileEc;
				if (fs::is_regular_file(child, fileEc) && !fileEc)
				{
					PushFixture(walk, child, name, /*viaShortcut*/ false);
					return;
				}
				// **切れたリンクは黙って落とさない。** 「あるはずの物件が回っていない」に
				// 気付けるのはこの 1 行だけである。
				walk.result.notes.push_back("辿れませんでした（リンク切れ？）: " + name);
				return;
			}

			if (ext == ".lnk")
				walk.result.notes.push_back("ショートカットを辿れませんでした: " + name);
			// 拡張子の無いファイルは、解決できなければただのファイル。黙って飛ばす。
		}
	} // namespace

	// -----------------------------------------------------------------------
	std::string parentFolderOf(const std::string& path)
	{
		if (path.empty())
			return {};
		return Utf8Of(PathFromUtf8(path).parent_path());
	}

	std::string folderFilePath(const std::string& directory, const std::string& fileName)
	{
		if (directory.empty() || fileName.empty())
			return {};
		return Utf8Of(PathFromUtf8(directory) / PathFromUtf8(fileName));
	}

	// -----------------------------------------------------------------------
	FixtureScan scanFixtureFolder(const std::string& directory, const ShortcutResolver& resolve)
	{
		Walk walk;
		if (directory.empty())
		{
			walk.result.notes.emplace_back("フォルダが指定されていません");
			return std::move(walk.result);
		}

		const fs::path root = PathFromUtf8(directory);
		std::error_code ec;
		if (!fs::is_directory(root, ec) || ec)
		{
			walk.result.notes.push_back("フォルダを開けませんでした: " + directory);
			return std::move(walk.result);
		}
		PushDirectory(walk, root, 0);

		// **幅優先で降りる。** 指定したフォルダの直下にあるものが先に並ぶので、リンクを
		// 並べただけの使い方では順序が読みやすい（最後に名前で並べ直すので結果は同じ）。
		for (std::size_t at = 0; at < walk.pending.size(); ++at)
		{
			// **参照で受けない**（下で pending へ足すと再確保で無効になる）。
			const fs::path dir = walk.pending[at].first;
			const int depth = walk.pending[at].second;

			std::error_code dirEc;
			const fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied,
											dirEc);
			if (dirEc)
			{
				walk.result.notes.push_back("フォルダを読めませんでした: " +
											Utf8Of(dir.filename()));
				continue;
			}

			// **並べてから仕分ける。** `directory_iterator` の順序は規定されていないので、
			// ここで決めないと走査の順も notes の順も環境によって変わる（CLAUDE.md
			// 「決定性を守る」）。
			std::vector<fs::path> children;
			for (const fs::directory_entry& entry : it)
				children.push_back(entry.path());
			std::sort(children.begin(), children.end(),
					  [](const fs::path& a, const fs::path& b) { return a.native() < b.native(); });

			for (const fs::path& child : children)
				ClassifyChild(walk, resolve, child, depth);
		}

		if (walk.capped)
			walk.result.notes.push_back("多すぎるので " + std::to_string(kMaxFixtureCount) +
										" 件で打ち切りました");

		// 最後にもう一度、**見出しの昇順**へ整える（同名は実体のパスで決める）。
		std::sort(walk.result.entries.begin(), walk.result.entries.end(),
				  [](const FixtureEntry& a, const FixtureEntry& b)
				  {
					  if (a.name != b.name)
						  return a.name < b.name;
					  return a.path < b.path;
				  });
		return std::move(walk.result);
	}
} // namespace HomeskzIfcImport::core
