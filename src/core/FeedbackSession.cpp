//
//	core/FeedbackSession.cpp
//
//	実機フィードバックの記憶の実装（意図は core/FeedbackSession.h 参照）。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない。
//

#include "core/FeedbackSession.h"
#include "core/ImportOptions.h"
#include "core/Trace.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace HomeskzIfcImport::core
{
	namespace
	{
		// 値に混ざってはいけないもの（改行）を落とし、前後の空白を削る。**行の書式が
		// key=value 1 行きりである以上、改行を含む値は書けない**——切り詰めるより
		// 落としたほうが、読み直したときに「途中で切れた行」を値と誤読せずに済む。
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

		// "1" / "0"。真偽は綴りを揺らさない（読む側の場合分けを増やさないため）。
		const char* boolText(bool value)
		{
			return value ? "1" : "0";
		}

		// 真とみなす綴り。書くのは常に "1" だが、人が手で直すこともあるので寛容に読む。
		bool parseBool(const std::string& value, bool fallback)
		{
			if (value == "1" || value == "true" || value == "yes" || value == "on")
				return true;
			if (value == "0" || value == "false" || value == "no" || value == "off")
				return false;
			return fallback;
		}

		// 10 進の整数（負・桁あふれ・数字以外は fallback）。**例外を投げない**——
		// 壊れた 1 行で往復が止まるのは割に合わない。
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

		// 役割 1 つぶんのキー接頭辞（"role.0."）。
		std::string roleKey(std::size_t index, const char* suffix)
		{
			return "role." + std::to_string(index) + "." + suffix;
		}
	} // namespace

	std::string formatFeedbackSession(const FeedbackSession& session)
	{
		std::ostringstream out;
		// 先頭に版を置く。**形を変えるときはここを上げ、読む側で分岐する**（いまは 1 だけ）。
		out << "# HomeskzIfcImport 実機フィードバックの記憶（自動生成。手で消してよい）\n";
		out << "version=1\n";
		out << "send=" << boolText(session.send) << "\n";
		out << "repo=" << sanitize(session.repo) << "\n";
		out << "pr=" << session.pullRequest << "\n";
		out << "branch=" << sanitize(session.branch) << "\n";
		out << "ifc=" << sanitize(session.ifcPath) << "\n";
		out << "auto=" << boolText(session.autoContinue) << "\n";
		out << "anon=" << boolText(session.anonymize) << "\n";
		out << "round=" << session.round << "\n";
		out << "build=" << sanitize(session.lastCommit) << "\n";
		out << "tally=" << sanitize(session.lastTally) << "\n";
		// 取り込み設定は役割の表の順に並べる（core/ImportOptions.h の symbolRoles）。
		for (std::size_t i = 0; i < kSymbolRoleCount; ++i)
		{
			const auto role = static_cast<SymbolRole>(i);
			out << roleKey(i, "symbol") << "=" << sanitize(session.options.symbol(role)) << "\n";
			out << roleKey(i, "on") << "=" << boolText(session.options.isEnabled(role)) << "\n";
		}
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

			if (key == "send")
				session.send = parseBool(value, session.send);
			else if (key == "repo")
				session.repo = value;
			else if (key == "pr")
				session.pullRequest = parseInt(value, session.pullRequest);
			else if (key == "branch")
				session.branch = value;
			else if (key == "ifc")
				session.ifcPath = value;
			else if (key == "auto")
				session.autoContinue = parseBool(value, session.autoContinue);
			else if (key == "anon")
				session.anonymize = parseBool(value, session.anonymize);
			else if (key == "round")
				session.round = parseInt(value, session.round);
			else if (key == "build")
				session.lastCommit = value;
			else if (key == "tally")
				session.lastTally = value;
			else if (key.starts_with("role."))
			{
				// "role.<n>.symbol" / "role.<n>.on"。表に無い番号は黙って飛ばす
				// （役割が増減しても古いファイルを読める）。
				const std::string::size_type dot = key.find('.', 5);
				if (dot == std::string::npos)
					continue;
				const int index = parseInt(key.substr(5, dot - 5), -1);
				if (index < 0 || static_cast<std::size_t>(index) >= kSymbolRoleCount)
					continue;
				const auto role = static_cast<SymbolRole>(index);
				const std::string field = key.substr(dot + 1);
				if (field == "symbol")
					session.options.setSymbol(role, value);
				else if (field == "on")
					session.options.setEnabled(role, parseBool(value, true));
			}
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
		// 投げない版を使う**——記憶を残せないだけで取り込み自体は続けられる。
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
} // namespace HomeskzIfcImport::core
