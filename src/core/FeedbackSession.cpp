//
//	core/FeedbackSession.cpp
//
//	実機テストが周をまたいで持ち越すものの実装（意図は core/FeedbackSession.h 参照）。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない。
//

#include "core/FeedbackSession.h"
#include "core/FeedbackScratch.h"
#include "core/ImportOptions.h"
#include "core/Json.h"
#include "core/Trace.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace HomeskzIfcImport::core
{
	namespace
	{
		// 値に混ざってはいけないもの（改行）を取り除き、前後の空白を削る。**行の書式が
		// key=value 1 行きりである以上、改行を含む値は書けない**——切り詰めるより
		// 取り除いたほうが、再度読み込んだときに「途中で切れた行」を値と誤読せずに済む。
		std::string sanitize(const std::string& value)
		{
			std::string out;
			out.reserve(value.size());
			for (const char c : value)
			{
				if (c != '\n' && c != '\r')
					out += c;
			}
			const std::string::size_type b = out.find_first_not_of(" \t");
			if (b == std::string::npos)
				return "";
			const std::string::size_type e = out.find_last_not_of(" \t");
			return out.substr(b, e - b + 1);
		}

		// 10 進の整数（負・桁あふれ・数字以外は fallback）。**例外を投げない**。
		int parseInt(const std::string& value, int fallback)
		{
			if (value.empty())
				return fallback;
			int result = 0;
			for (const char c : value)
			{
				if (c < '0' || c > '9')
					return fallback;
				if (result > 214748363) // これ以上進めると int があふれる
					return fallback;
				result = result * 10 + (c - '0');
			}
			return result;
		}

		// 文字列の配列（配列でない・文字列でない要素があれば false）。
		bool stringList(const Json& value, std::vector<std::string>& out)
		{
			if (!value.isArray())
				return false;
			out.clear();
			for (const Json& item : value.items())
			{
				if (item.kind() != Json::Kind::String)
					return false;
				out.push_back(item.asString());
			}
			return true;
		}

		// 表示名から役割を引く（無ければ false）。
		bool roleByLabel(const std::string& label, SymbolRole& out)
		{
			for (const SymbolRoleInfo& info : symbolRoles())
			{
				if (label == info.label)
				{
					out = info.role;
					return true;
				}
			}
			return false;
		}

		// 役割の表示名の一覧（理由の文言に添える）。
		std::string roleLabels()
		{
			std::string text;
			for (const SymbolRoleInfo& info : symbolRoles())
			{
				if (!text.empty())
					text += " / ";
				text += info.label;
			}
			return text;
		}

		bool applySymbols(const Json& symbols, ImportOptions& options, std::string& error)
		{
			if (!symbols.isObject())
			{
				error = "settings.symbols はオブジェクトで渡してください";
				return false;
			}
			for (const auto& [label, value] : symbols.members())
			{
				SymbolRole role = SymbolRole::AnchorBoltM12;
				if (!roleByLabel(label, role))
				{
					error = "settings.symbols に知らない役割があります（" + label +
							"。使えるのは " + roleLabels() + "）";
					return false;
				}
				if (value.kind() == Json::Kind::String)
				{
					const std::string name = sanitize(value.asString());
					if (!name.empty())
						options.setSymbol(role, name);
					options.setEnabled(role, !name.empty());
				}
				else if (value.kind() == Json::Kind::Bool)
					options.setEnabled(role, value.asBool());
				else
				{
					error = "settings.symbols の値はシンボル名か true / false で渡してください（" +
							label + "）";
					return false;
				}
			}
			return true;
		}

		bool applyMergeLevels(const Json& levels, ImportOptions& options, std::string& error)
		{
			std::vector<std::string> keys;
			if (!stringList(levels, keys))
			{
				error = "settings.merge_levels は \"<階の番号>:<高さ mm>\" の配列で渡してください";
				return false;
			}
			options.mergedPlanLevels.clear();
			for (const std::string& key : keys)
			{
				const std::string::size_type colon = key.find(':');
				const int story =
					colon == std::string::npos ? -1 : parseInt(sanitize(key.substr(0, colon)), -1);
				const int height =
					colon == std::string::npos ? -1 : parseInt(sanitize(key.substr(colon + 1)), -1);
				if (story < 0 || height < 0)
				{
					error = "settings.merge_levels の値を読めません（" + key +
							"。\"<階の番号>:<高さ mm>\" で渡してください）";
					return false;
				}
				options.setMergeWithPrevious(PlanLevelKey{story, height}, true);
			}
			return true;
		}

		bool applyRafter(const Json& rafter, ImportOptions& options, std::string& error)
		{
			if (!rafter.isObject())
			{
				error = "settings.rafter は { \"width\": mm, \"height\": mm } で渡してください";
				return false;
			}
			double width = options.rafterWidth;
			double height = options.rafterHeight;
			for (const auto& [key, value] : rafter.members())
			{
				if ((key != "width" && key != "height") || value.kind() != Json::Kind::Number ||
					!isValidRafterSize(value.asNumber()))
				{
					error = "settings.rafter の値を読めません（" + key +
							"。width / height を mm の正の数で渡してください）";
					return false;
				}
				if (key == "width")
					width = value.asNumber();
				else
					height = value.asNumber();
			}
			options.setRafterSize(width, height);
			return true;
		}
	} // namespace

	bool applyTestSettings(const Json& settings, ImportOptions& options, std::string& error)
	{
		if (settings.isNull())
			return true;
		if (!settings.isObject())
		{
			error = "settings はオブジェクトで渡してください";
			return false;
		}
		for (const auto& [key, value] : settings.members())
		{
			bool ok = true;
			if (key == "symbols")
				ok = applySymbols(value, options, error);
			else if (key == "title_block" || key == "dimension")
			{
				if (value.kind() != Json::Kind::String)
				{
					error = "settings." + key + " は文字列で渡してください（空＝" +
							(key == "title_block" ? "置かない" : "入れない") + "）";
					return false;
				}
				if (key == "title_block")
					options.setTitleBlockStyle(sanitize(value.asString()));
				else
					options.setDimensionStandard(sanitize(value.asString()));
			}
			else if (key == "merge_levels")
				ok = applyMergeLevels(value, options, error);
			else if (key == "skip_sections")
			{
				std::vector<std::string> numbers;
				if (!stringList(value, numbers))
				{
					error = "settings.skip_sections は図番（文字列）の配列で渡してください";
					return false;
				}
				options.setSkippedSections(numbers);
			}
			else if (key == "rafter")
				ok = applyRafter(value, options, error);
			else
			{
				error = "settings に知らない項目があります（" + key +
						"。使えるのは symbols / title_block / dimension / merge_levels / "
						"skip_sections / rafter）";
				return false;
			}
			if (!ok)
				return false;
		}
		return true;
	}

	std::string formatFeedbackSession(const FeedbackSession& session)
	{
		std::ostringstream out;
		// 先頭に版を置く。**形を変えるときはここを上げ、読む側で分岐する**。
		out << "# HomeskzIfcImport 実機テストが保存した図面（自動生成。手で消してよい）\n";
		out << "version=2\n";
		// 自分で保存した図面（次の周の頭で閉じる相手）。**1 つ 1 行**。
		for (const std::string& doc : session.ownedDocuments)
			out << "owned.doc=" << sanitize(doc) << "\n";
		return out.str();
	}

	FeedbackSession parseFeedbackSession(const std::string& text)
	{
		FeedbackSession session;
		std::istringstream in(text);
		std::string line;
		while (std::getline(in, line))
		{
			// CRLF で書かれたファイル（Windows で手直しされたもの）も読めるように。
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.empty() || line[0] == '#')
				continue;
			const std::string::size_type eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			const std::string key = sanitize(line.substr(0, eq));
			const std::string value = sanitize(line.substr(eq + 1));

			// M42 までの記憶にある条件の行（ifc / round / build / tally / template /
			// baseline / role.* など）は、知らない行として通知せずに読み飛ばす（M43）。
			// **重ねて読む**。空の行は閉じる相手にならないので捨てる。
			if (key == "owned.doc" && !value.empty())
				session.ownedDocuments.push_back(value);
		}
		return session;
	}

	std::string defaultFeedbackSessionPath()
	{
		// 試験用の差し替え（無 SDK テストと、実機で置き場所を変えたいとき）。
		std::string custom = trace::envValue("HOMESKZ_IFC_FEEDBACK_STATE");
		if (!custom.empty())
			return custom;

		// Windows は LOCALAPPDATA、macOS は HOME/Library/Application Support。どちらの
		// 環境変数も GUI アプリの子プロセスに必ず入っている。
		//
		// **フォルダ名（HomeskzIfcImport）は識別子なので、プラグインの改名に追随させない。**
		// 付け替えると、実機テストが保存した図面の記録と直近の報告が警告なしに参照できなく
		// なる。同梱スクリプトが同じフォルダから読むトークンも同じ理由で据え置いて
		// ある（scripts/vw-token.ps1 の Get-TokenFilePath）。
		const std::string localAppData = trace::envValue("LOCALAPPDATA");
		if (!localAppData.empty())
			return localAppData + "\\HomeskzIfcImport\\feedback.txt";

		const std::string home = trace::envValue("HOME");
		if (!home.empty())
			return home + "/Library/Application Support/HomeskzIfcImport/feedback.txt";

		return "";
	}

	bool readFeedbackSession(const std::string& path, FeedbackSession& out)
	{
		if (path.empty())
			return false;
		const std::ifstream in(path, std::ios::binary);
		if (!in)
			return false;
		std::ostringstream buffer;
		buffer << in.rdbuf();
		out = parseFeedbackSession(buffer.str());
		return true;
	}

	bool writeFeedbackSession(const std::string& path, const FeedbackSession& session)
	{
		if (path.empty())
			return false;

		// 置き場所（…/HomeskzIfcImport/）はまだ無いのが普通なので用意する。**例外を
		// 投げない版を使う**——記録を残せないだけで取り込み自体は続けられる。
		std::error_code ec;
		const std::filesystem::path file(path);
		if (file.has_parent_path())
			std::filesystem::create_directories(file.parent_path(), ec);

		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;
		const std::string text = formatFeedbackSession(session);
		out.write(text.data(), static_cast<std::streamsize>(text.size()));
		return out.good();
	}

	void clearFeedbackSession(const std::string& path)
	{
		if (path.empty())
			return;
		std::error_code ec;
		std::filesystem::remove(std::filesystem::path(path), ec);
	}

	std::string testReportPathFor(const std::string& sessionPath)
	{
		// **文字列のまま差し替える。** std::filesystem::path へ通すと、Windows では UTF-8 の
		// パスが ANSI のコードページとして読まれ、日本語のユーザー名が文字化けする。
		if (sessionPath.empty())
			return "";
		const std::string::size_type slash = sessionPath.find_last_of("/\\");
		if (slash == std::string::npos)
			return "last-round.md";
		return sessionPath.substr(0, slash + 1) + "last-round.md";
	}

	bool isOwnedTestDocument(const FeedbackSession& session, const std::string& openPath,
							 const std::string& scratchRoot)
	{
		if (openPath.empty() || scratchRoot.empty())
			return false;
		for (const std::string& owned : session.ownedDocuments)
		{
			// **置き場の外を指す記録は、一致していても対象にしない**（安全弁の 2 つ目）。
			if (!pathIsInside(owned, scratchRoot))
				continue;
			if (owned == openPath)
				return true;
			std::error_code ec;
			if (std::filesystem::equivalent(std::filesystem::path(owned),
											std::filesystem::path(openPath), ec) &&
				!ec)
				return true;
		}
		return false;
	}
} // namespace HomeskzIfcImport::core
