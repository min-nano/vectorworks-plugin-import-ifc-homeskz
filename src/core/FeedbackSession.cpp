//
//	core/FeedbackSession.cpp
//
//	実機フィードバックの記憶の実装（意図は core/FeedbackSession.h 参照）。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない。
//

#include "core/FeedbackSession.h"
#include "core/FeedbackScratch.h"
#include "core/ImportOptions.h"
#include "core/Trace.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
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
		out << "# HomeskzIfcImport 実機テストの記憶（自動生成。手で消してよい）\n";
		out << "version=1\n";
		out << "ifc=" << sanitize(session.ifcPath) << "\n";
		out << "round=" << session.round << "\n";
		out << "build=" << sanitize(session.lastCommit) << "\n";
		out << "tally=" << sanitize(session.lastTally) << "\n";
		out << "template=" << sanitize(session.templatePath) << "\n";
		// 自分で保存した図面（次の周の頭で閉じる相手）。**1 つ 1 行**。
		for (const std::string& doc : session.ownedDocuments)
			out << "owned.doc=" << sanitize(doc) << "\n";
		// 1 周目に採った基準。**レイヤ 1 枚につき 1 行**にしてあるのは、名前へ入れて
		// よい文字を区切り記号で縛らないため（"," も "\t" もレイヤ名に使える）。
		out << "baseline=" << boolText(session.baselineRecorded) << "\n";
		for (const std::string& layer : session.baselineLayers)
			out << "baseline.layer=" << sanitize(layer) << "\n";
		// 取り込み設定は役割の表の順に並べる（core/ImportOptions.h の symbolRoles）。
		for (std::size_t i = 0; i < kSymbolRoleCount; ++i)
		{
			const auto role = static_cast<SymbolRole>(i);
			out << roleKey(i, "symbol") << "=" << sanitize(session.options.symbol(role)) << "\n";
			out << roleKey(i, "on") << "=" << boolText(session.options.isEnabled(role)) << "\n";
		}
		// M28 図面枠のスタイル（空＝置かない）。**役割の表の外にある設定も漏らさず書く**——
		// 2 周目以降は設定ダイアログを出さずにここから復元するので、書き落とすと 1 周目と
		// 違う条件（図面枠なし）で警告なしに実行される（PR #133 の round 2 で実際に起きた）。
		out << "titleblock=" << sanitize(session.options.titleBlockStyle()) << "\n";
		// M31 寸法規格（空＝入れない）。図面枠と同じ理由で漏らさず書く。
		out << "dimension=" << sanitize(session.options.dimensionStandard()) << "\n";
		// 伏図のまとめ方（前のレベルと同じ伏図にまとめる高さ）。図面枠と同じ理由で漏らさず
		// 書く。**1 つにつき 1 行**で "<階の番号>:<高さ mm>"（core::PlanLevelKey）。
		for (const PlanLevelKey& key : session.options.mergedPlanLevels)
			out << "merge.level=" << key.story << ":" << key.height << "\n";
		// M34 軸組図から除外する通り（図番）。図面枠と同じ理由で漏らさず書く——書き落とすと
		// 続きの周が除外したはずの通りまで描画する。レイヤと同じく**1 本 1 行**。
		for (const std::string& number : session.options.skippedSections)
			out << "section.skip=" << sanitize(number) << "\n";
		// 垂木の断面（mm）。図面枠と同じ理由で漏らさず書く——書き落とすと続きの周が
		// 既定の 45×45 で描画する。読み戻せる表記（core::formatRafterSize）で書く。
		out << "rafter.width=" << formatRafterSize(session.options.rafterWidth) << "\n";
		out << "rafter.height=" << formatRafterSize(session.options.rafterHeight) << "\n";
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

			// M37 までの記憶にある send / repo / pr / branch / anon / posted / loop は、
			// 下の「知らない行」として通知せずに読み飛ばす（PR への投稿をやめた。M38）。
			// M38 までの work（作業ファイル。.vwx）と created.layer / created.sheet
			// （レイヤ削除の相手）も同じく読み飛ばす——作業ファイルは開くと**その
			// ファイル自体**が開いてしまい、テンプレートの代わりにならない（M39）。
			if (key == "ifc")
				session.ifcPath = value;
			else if (key == "round")
				session.round = parseInt(value, session.round);
			else if (key == "build")
				session.lastCommit = value;
			else if (key == "tally")
				session.lastTally = value;
			else if (key == "template")
				session.templatePath = value;
			else if (key == "owned.doc")
			{
				// **重ねて読む**。空の行は閉じる相手にならないので捨てる。
				if (!value.empty())
					session.ownedDocuments.push_back(value);
			}
			else if (key == "baseline")
				session.baselineRecorded = parseBool(value, session.baselineRecorded);
			else if (key == "baseline.layer")
			{
				// **重ねて読む**（行の数だけレイヤがある）。空行は基準にならないので捨てる。
				if (!value.empty())
					session.baselineLayers.push_back(value);
			}
			else if (key == "titleblock")
			{
				// 古い記憶（M28 より前）には行が無い——既定の空（置かない）のまま読む。
				session.options.setTitleBlockStyle(value);
			}
			else if (key == "dimension")
			{
				// 古い記憶（M31 より前）には行が無い——既定の空（入れない）のまま読む。
				session.options.setDimensionStandard(value);
			}
			else if (key == "rafter.width" || key == "rafter.height")
			{
				// 古い記憶には行が無い——既定（45×45）のまま読む。読めない値も既定のまま。
				const std::optional<double> size = parseRafterSize(value);
				if (size.has_value())
				{
					if (key == "rafter.width")
						session.options.setRafterSize(*size, session.options.rafterHeight);
					else
						session.options.setRafterSize(session.options.rafterWidth, *size);
				}
			}
			else if (key == "merge.level")
			{
				// "<階の番号>:<高さ mm>"。読めない行は通知せずに読み飛ばす（古い記憶には行が無く、
				// まとめない＝既定のまま読む）。GL より下の横架材は無いので、高さも負を
				// 読まない parseInt で足りる。
				const std::string::size_type colon = value.find(':');
				if (colon == std::string::npos)
					continue;
				const int story = parseInt(value.substr(0, colon), -1);
				const int height = parseInt(value.substr(colon + 1), -1);
				if (story >= 0 && height >= 0)
					session.options.setMergeWithPrevious(PlanLevelKey{story, height}, true);
			}
			else if (key == "section.skip")
			{
				// **重ねて読む**（行の数だけ通りがある）。古い記憶（M34 より前）には行が
				// 無い——既定の空（全部描画する）のまま読む。
				if (!value.empty())
				{
					std::vector<std::string> skipped = session.options.skippedSections;
					skipped.push_back(value);
					session.options.setSkippedSections(skipped);
				}
			}
			else if (key.starts_with("role."))
			{
				// "role.<n>.symbol" / "role.<n>.on"。表に無い番号は通知せずに読み飛ばす
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
		//
		// **フォルダ名（HomeskzIfcImport）は識別子なので、プラグインの改名に追随させない。**
		// 付け替えると、実機テストの記憶（周回数・前の周の内訳・1 周目の選択）が警告なしに
		// 参照できなくなる。同梱スクリプトが同じフォルダから読むトークンも同じ理由で据え置いて
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

	bool feedbackSessionRemembered(const FeedbackSession& session)
	{
		// 1 周は済んでいて（round>0）、その周の IFC が分かっている（ifcPath）——どちらか
		// 欠けていれば続きの周は組み立てられないので、1 周目として扱う。
		return session.round > 0 && !session.ifcPath.empty();
	}

	FeedbackRoundKind feedbackRoundKind(const FeedbackSession& session, bool allowDialogs,
										bool ifcRequested)
	{
		if (!allowDialogs && ifcRequested)
		{
			// **IFC を名指しされたら、記憶があっても新しい 1 周目**（別の IFC で試し直すのに
			// 人の手を要らなくする）。描画先はテンプレートからしか作らない（Refuse の doc）。
			return session.templatePath.empty() ? FeedbackRoundKind::Refuse
												: FeedbackRoundKind::AutoFirstRound;
		}
		if (feedbackSessionRemembered(session))
		{
			// **MCP の周はテンプレートが無ければ走らない**（Refuse の doc コメント）。
			if (!allowDialogs && session.templatePath.empty())
				return FeedbackRoundKind::Refuse;
			return FeedbackRoundKind::ContinueRound;
		}
		return allowDialogs ? FeedbackRoundKind::FirstRound : FeedbackRoundKind::Refuse;
	}

	bool isOwnedTestDocument(const FeedbackSession& session, const std::string& openPath,
							 const std::string& scratchRoot)
	{
		if (openPath.empty() || scratchRoot.empty())
			return false;
		for (const std::string& owned : session.ownedDocuments)
		{
			// **置き場の外を指す記憶は、一致していても対象にしない**（安全弁の 2 つ目）。
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
