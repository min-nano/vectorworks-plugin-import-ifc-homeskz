//
//	core/FeedbackScratch.cpp
//
//	実機テストの一時ファイルの置き場と片付け（core/FeedbackScratch.h）。
//

#include "core/FeedbackScratch.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace HomeskzIfcImport::core
{
	namespace
	{
		namespace fs = std::filesystem;

		bool safeChar(char c)
		{
			return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
				   c == '.' || c == '_' || c == '-';
		}

		// 目印に書いてあるブランチ名（読めなければ空）。1 行目だけを採り、行末の CR は落とす。
		std::string readMarker(const fs::path& dir)
		{
			std::ifstream in(dir / kScratchBranchFile, std::ios::binary);
			if (!in)
				return "";
			std::string line;
			std::getline(in, line);
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			return line;
		}

		bool writeMarker(const fs::path& dir, const std::string& branch)
		{
			std::ofstream out(dir / kScratchBranchFile, std::ios::binary | std::ios::trunc);
			if (!out)
				return false;
			out << branch << "\n";
			return static_cast<bool>(out);
		}

		// 正規化した絶対パス（在るものは実体へ。駄目なら素の絶対パス）。
		fs::path canonicalOr(const fs::path& path)
		{
			std::error_code ec;
			fs::path result = fs::weakly_canonical(path, ec);
			if (ec)
				result = fs::absolute(path, ec);
			return result;
		}

		bool endsWith(const std::string& text, const std::string& suffix)
		{
			return text.size() >= suffix.size() &&
				   text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
		}

		void joinTo(std::ostringstream& out, const std::vector<std::string>& items)
		{
			for (std::size_t i = 0; i < items.size(); ++i)
				out << (i == 0 ? "" : "・") << items[i];
		}
	} // namespace

	std::string scratchDirName(const std::string& branch)
	{
		std::string name;
		name.reserve(branch.size());
		for (const char c : branch)
			name += safeChar(c) ? c : '-';
		// `.` / `..` / 空は置き場の外や置き場そのものを指してしまう。
		if (name.find_first_not_of('.') == std::string::npos)
			name = "b-" + name;
		return name;
	}

	std::string prepareBranchScratch(const std::string& root, const std::string& branch)
	{
		if (root.empty())
			return "";
		std::error_code ec;
		fs::create_directories(fs::path(root), ec);
		if (ec)
			return "";
		const std::string base = scratchDirName(branch);
		// **同じ名前に写る別のブランチとは番号で分ける。** 目印が自分のブランチなら
		// それを使い、別のブランチなら次の番号へ。上限に意味は無く、暴走を止めるだけ。
		for (int i = 1; i <= 100; ++i)
		{
			const fs::path dir =
				fs::path(root) / (i == 1 ? base : base + "-" + std::to_string(i));
			if (fs::is_directory(dir, ec))
			{
				if (fs::exists(dir / kScratchBranchFile, ec) && readMarker(dir) == branch)
					return dir.string();
				continue;
			}
			if (!fs::create_directory(dir, ec) || ec)
				return "";
			if (!writeMarker(dir, branch))
				return "";
			return dir.string();
		}
		return "";
	}

	std::vector<ScratchDir> listScratchDirs(const std::string& root)
	{
		std::vector<ScratchDir> dirs;
		if (root.empty())
			return dirs;
		std::error_code ec;
		fs::directory_iterator it(fs::path(root), ec);
		if (ec)
			return dirs;
		for (const fs::directory_entry& entry : it)
		{
			std::error_code entryEc;
			if (entry.is_symlink(entryEc) || !entry.is_directory(entryEc))
				continue;
			if (!fs::exists(entry.path() / kScratchBranchFile, entryEc))
				continue;
			dirs.push_back({entry.path().string(), readMarker(entry.path())});
		}
		std::sort(dirs.begin(), dirs.end(),
				  [](const ScratchDir& a, const ScratchDir& b) { return a.path < b.path; });
		return dirs;
	}

	std::map<std::string, PrState> parsePrStates(const std::string& output)
	{
		std::map<std::string, PrState> states;
		std::istringstream in(output);
		std::string line;
		while (std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			const std::string head = "pr-state\t";
			if (line.compare(0, head.size(), head) != 0)
				continue;
			const std::size_t tab = line.find('\t', head.size());
			if (tab == std::string::npos)
				continue;
			const std::string word = line.substr(head.size(), tab - head.size());
			const std::string branch = line.substr(tab + 1);
			if (branch.empty())
				continue;
			PrState state = PrState::Unknown;
			if (word == "open")
				state = PrState::Open;
			else if (word == "closed")
				state = PrState::Closed;
			else if (word == "none")
				state = PrState::None;
			// 2 度出たら消さない側へ（Closed は他のどれにも負ける）。
			const auto found = states.find(branch);
			if (found == states.end())
				states.emplace(branch, state);
			else if (found->second == PrState::Closed)
				found->second = state;
		}
		return states;
	}

	bool removeScratchDir(const std::string& root, const ScratchDir& dir, std::string& why)
	{
		why.clear();
		std::error_code ec;
		const fs::path target(dir.path);
		if (root.empty() || dir.path.empty() || dir.branch.empty())
		{
			why = "置き場かブランチが空";
			return false;
		}
		// **置き場の直下だけ。** 正規化して親を比べる（`..` やリンクで外へ出させない）。
		if (fs::is_symlink(target, ec) || !fs::is_directory(target, ec))
		{
			why = "フォルダではない";
			return false;
		}
		if (canonicalOr(target).parent_path() != canonicalOr(fs::path(root)))
		{
			why = "置き場の直下ではない";
			return false;
		}
		// **目印が問い合わせたブランチと一致すること。** 一覧を取ってから消すまでの
		// あいだに入れ替わっていないかも、ここで確かめ直す。
		if (readMarker(target) != dir.branch)
		{
			why = "目印のブランチが一致しない";
			return false;
		}

		std::vector<fs::path> files;
		fs::directory_iterator it(target, ec);
		if (ec)
		{
			why = "中身を読めない";
			return false;
		}
		for (const fs::directory_entry& entry : it)
		{
			std::error_code entryEc;
			const std::string name = entry.path().filename().string();
			if (entry.is_symlink(entryEc) || !entry.is_regular_file(entryEc))
			{
				why = "ふつうのファイル以外がある（" + name + "）";
				return false;
			}
			// Vectorworks は開いている図面の隣に `.<名前>.lck` を置く（実機の一時
			// ディレクトリで確認）。在るなら開いている——足元から消さない。
			if (endsWith(name, ".lck"))
			{
				why = "Vectorworks が開いている図面がある";
				return false;
			}
			files.push_back(entry.path());
		}
		// 目印は最後に消す（途中で止まっても、残ったフォルダが次の周でまた候補になる）。
		std::sort(files.begin(), files.end());
		for (const fs::path& file : files)
		{
			if (file.filename() == kScratchBranchFile)
				continue;
			if (!fs::remove(file, ec) || ec)
			{
				why = "消せないファイルがある（" + file.filename().string() + "）";
				return false;
			}
		}
		if (!fs::remove(target / kScratchBranchFile, ec) || ec || !fs::remove(target, ec) || ec)
		{
			why = "フォルダを消せない";
			return false;
		}
		return true;
	}

	ScratchCleanup cleanUpClosedBranches(const std::string& root,
										 const std::vector<ScratchDir>& candidates,
										 const std::map<std::string, PrState>& states)
	{
		ScratchCleanup result;
		for (const ScratchDir& dir : candidates)
		{
			const auto found = states.find(dir.branch);
			if (found == states.end() || found->second != PrState::Closed)
				continue; // 開いている・分からない・PR が無い——どれも残す
			std::string why;
			if (removeScratchDir(root, dir, why))
			{
				result.removedBranches.push_back(dir.branch);
				result.removedPaths.push_back(dir.path);
			}
			else
				result.kept.push_back(dir.branch + "（" + why + "）");
		}
		return result;
	}

	bool pathIsInside(const std::string& path, const std::string& dir)
	{
		if (path.empty() || dir.empty())
			return false;
		const fs::path child = fs::path(path).lexically_normal();
		fs::path parent = fs::path(dir).lexically_normal();
		if (!parent.has_filename())
			parent = parent.parent_path(); // 末尾の区切り（"a/"）を落とす
		auto c = child.begin();
		for (const fs::path& part : parent)
		{
			if (c == child.end() || *c != part)
				return false;
			++c;
		}
		return c != child.end();
	}

	std::string describeScratchCleanup(const ScratchCleanup& cleanup)
	{
		if (cleanup.removedBranches.empty() && cleanup.kept.empty())
			return "";
		std::ostringstream out;
		out << "一時ファイル: ";
		if (!cleanup.removedBranches.empty())
		{
			out << "PR が閉じたブランチの作業ファイルを片付けました（";
			joinTo(out, cleanup.removedBranches);
			out << "）";
		}
		if (!cleanup.kept.empty())
		{
			out << (cleanup.removedBranches.empty() ? "" : "。")
				<< "PR が閉じたブランチのうち片付けなかったもの: ";
			joinTo(out, cleanup.kept);
		}
		return out.str();
	}
} // namespace HomeskzIfcImport::core
