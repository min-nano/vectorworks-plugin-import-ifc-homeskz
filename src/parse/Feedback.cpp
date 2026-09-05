//
//	parse/Feedback.cpp
//
//	実機フィードバック本文の実装（意図は parse/Feedback.h 参照）。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない。
//

#include "parse/Feedback.h"
#include "core/Document.h"
#include "parse/Summary.h"

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::parse
{
	namespace
	{
		// text の中の from を to へ全部置き換える（from が空なら何もしない）。
		std::string replaceAll(std::string text, const std::string& from, const std::string& to)
		{
			if (from.empty())
				return text;
			std::string::size_type pos = 0;
			while ((pos = text.find(from, pos)) != std::string::npos)
			{
				text.replace(pos, from.size(), to);
				pos += to.size();
			}
			return text;
		}

		// 末尾のファイル名（区切りは POSIX と Windows の両方を見る）。
		std::string fileNameOf(const std::string& path)
		{
			const std::string::size_type pos = path.find_last_of("/\\");
			if (pos == std::string::npos)
				return path;
			return path.substr(pos + 1);
		}

		// FNV-1a（32bit）。**暗号用途ではない**——同じ入力なら同じ仮名になり、仮名から
		// 元の名前が読めない、という 2 つだけが要るので、短くて依存の無いものを使う。
		std::uint32_t fnv1a(const std::string& text)
		{
			std::uint32_t hash = 2166136261U;
			for (const char c : text)
			{
				hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
				hash *= 16777619U;
			}
			return hash;
		}

		std::string hex6(std::uint32_t value)
		{
			std::ostringstream out;
			out << std::hex << std::setw(6) << std::setfill('0') << (value & 0xFFFFFFU);
			return out.str();
		}

		// "/Users/<名前>/" のような区間のユーザー名を伏せる。marker は "/Users/" のような
		// 区切りを含む接頭辞で、その直後の 1 区画（次の区切りまで）を "…" へ替える。
		std::string maskUserSegment(std::string text, const std::string& marker, char separator)
		{
			std::string::size_type pos = 0;
			while ((pos = text.find(marker, pos)) != std::string::npos)
			{
				const std::string::size_type nameAt = pos + marker.size();
				std::string::size_type end = text.find(separator, nameAt);
				if (end == std::string::npos)
					end = text.size();
				if (end == nameAt) // 区切りが続いただけ（伏せるものが無い）
				{
					pos = nameAt;
					continue;
				}
				text.replace(nameAt, end - nameAt, "…");
				pos = nameAt + std::string("…").size();
			}
			return text;
		}

		// 内訳の 1 行表現を要素ごとに切り分ける（"ラベル:描けた/命令" の並び）。
		// 壊れた要素は飛ばす——古い版が書いた記憶を読めなくして往復を止めない。
		struct TallyEntry
		{
			std::string label;
			std::size_t placed = 0;
			std::size_t commands = 0;
		};

		std::size_t parseCount(const std::string& text, bool& ok)
		{
			ok = !text.empty();
			std::size_t value = 0;
			for (const char c : text)
			{
				if (c < '0' || c > '9')
				{
					ok = false;
					return 0;
				}
				value = value * 10 + static_cast<std::size_t>(c - '0');
			}
			return value;
		}

		std::vector<TallyEntry> parseTally(const std::string& text)
		{
			std::vector<TallyEntry> entries;
			std::string::size_type pos = 0;
			while (pos <= text.size() && !text.empty())
			{
				const std::string::size_type comma = text.find(',', pos);
				const std::string item =
					text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
				pos = (comma == std::string::npos) ? text.size() + 1 : comma + 1;

				const std::string::size_type colon = item.rfind(':');
				const std::string::size_type slash = item.rfind('/');
				if (colon == std::string::npos || slash == std::string::npos || slash < colon)
					continue;
				bool okPlaced = false;
				bool okCommands = false;
				TallyEntry entry;
				entry.label = item.substr(0, colon);
				entry.placed = parseCount(item.substr(colon + 1, slash - colon - 1), okPlaced);
				entry.commands = parseCount(item.substr(slash + 1), okCommands);
				if (entry.label.empty() || !okPlaced || !okCommands)
					continue;
				entries.push_back(entry);
			}
			return entries;
		}

		// 秒を人が読める形へ（診断ログと同じ粒度で十分なので小数 1 桁）。
		std::string formatSeconds(double seconds)
		{
			std::ostringstream out;
			out << std::fixed << std::setprecision(1) << seconds << " 秒";
			return out.str();
		}

		// バイト数を KB / MB で（0 なら空）。**大きさを載せるのは「そもそも中身のある
		// ファイルだったか」の切り分けに要るから**（診断ログの見出しと同じ理由）。
		std::string formatBytes(unsigned long long bytes)
		{
			if (bytes == 0)
				return "";
			std::ostringstream out;
			out << std::fixed << std::setprecision(1);
			if (bytes >= 1024ULL * 1024ULL)
				out << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
			else
				out << (static_cast<double>(bytes) / 1024.0) << " KB";
			return out.str();
		}

		// 囲みコードブロック（``` で挟む）。中に ``` が現れても壊れないよう、4 連の
		// バッククォートで囲む——診断ログに Markdown が入ることは無いが、ユーザーの
		// 所見には何が書かれるか分からない。
		std::string codeBlock(const std::string& text)
		{
			std::string body = text;
			if (!body.empty() && body.back() != '\n')
				body += "\n";
			return "````\n" + body + "````\n";
		}
	} // namespace

	std::string formatTally(const std::vector<ElementRow>& rows)
	{
		std::ostringstream out;
		bool first = true;
		for (const ElementRow& row : rows)
		{
			// 命令が 0 の要素は載せない（無い物の 0 は差分の役に立たない）。
			if (row.commands == 0)
				continue;
			if (!first)
				out << ",";
			first = false;
			out << row.label << ":" << row.placed << "/" << row.commands;
		}
		return out.str();
	}

	std::string formatTallyDiff(const std::string& previous, const std::string& current)
	{
		const std::vector<TallyEntry> before = parseTally(previous);
		const std::vector<TallyEntry> after = parseTally(current);
		if (before.empty())
			return "";

		std::ostringstream out;
		for (const TallyEntry& now : after)
		{
			const TallyEntry* was = nullptr;
			for (const TallyEntry& entry : before)
			{
				if (entry.label == now.label)
				{
					was = &entry;
					break;
				}
			}
			if (was == nullptr)
			{
				out << "- " << now.label << ": （前回は無し）→ " << now.placed << "/"
					<< now.commands << "\n";
				continue;
			}
			if (was->placed == now.placed && was->commands == now.commands)
				continue;
			out << "- " << now.label << ": " << was->placed << "/" << was->commands << " → "
				<< now.placed << "/" << now.commands << "\n";
		}
		// 前回はあったのに今回は命令ごと消えた要素。**退行として真っ先に見たいので必ず出す。**
		for (const TallyEntry& was : before)
		{
			bool stillThere = false;
			for (const TallyEntry& now : after)
			{
				if (now.label == was.label)
				{
					stillThere = true;
					break;
				}
			}
			if (!stillThere)
				out << "- " << was.label << ": " << was.placed << "/" << was.commands
					<< " → （今回は命令なし）\n";
		}
		return out.str();
	}

	std::string anonymizedFileName(const std::string& path)
	{
		const std::string name = fileNameOf(path);
		if (name.empty())
			return "model-000000.ifc";
		const std::string::size_type dot = name.rfind('.');
		const std::string stem = (dot == std::string::npos) ? name : name.substr(0, dot);
		const std::string ext = (dot == std::string::npos) ? std::string(".ifc") : name.substr(dot);
		return "model-" + hex6(fnv1a(stem)) + ext;
	}

	std::string redactText(const std::string& text, const std::string& ifcPath)
	{
		std::string out = text;
		if (!ifcPath.empty())
		{
			const std::string name = fileNameOf(ifcPath);
			const std::string alias = anonymizedFileName(ifcPath);
			// **長いほうから順に**置き換える（先に短いほうを消すと、長いほうの一部が
			// 置き換わって「伏せたつもりのパス」が半端に残る）。
			out = replaceAll(out, ifcPath, alias);
			out = replaceAll(out, name, alias);
			const std::string::size_type dot = name.rfind('.');
			if (dot != std::string::npos && dot > 0)
				out = replaceAll(out, name.substr(0, dot), alias.substr(0, alias.rfind('.')));
		}
		// ホームディレクトリのユーザー名（ログのパスに必ず出る）。
		out = maskUserSegment(out, "/Users/", '/');
		out = maskUserSegment(out, "/home/", '/');
		out = maskUserSegment(out, "\\Users\\", '\\');
		return out;
	}

	std::string formatFeedbackComment(const FeedbackRound& round, const core::Document& document,
									  const core::DrawCounts& counts)
	{
		const ImportOutcome outcome = importOutcome(document, counts);
		const std::vector<ElementRow> rows = elementRows(document, counts);
		const std::string tally = formatTally(rows);

		// 伏せるかどうかで、載せる名前と本文の作り方が変わる。**判断はここ 1 か所**
		// （あちこちで if を書くと、必ずどこかで素の値が漏れる）。
		const std::string shownFile =
			round.anonymize ? anonymizedFileName(round.ifcPath) : fileNameOf(round.ifcPath);
		auto clean = [&round](const std::string& text)
		{ return round.anonymize ? redactText(text, round.ifcPath) : text; };

		std::ostringstream out;
		// 機械可読の目印。**本文の見た目を変えてもここは変えない**——読む側（Claude）が
		// 「これはプラグインの自動投稿で、何周目のどのビルドか」を確実に拾えるようにする。
		out << "<!-- homeskz-ifc-feedback v1 round=" << round.round
			<< " build=" << round.build.commit << " branch=" << round.build.branch << " -->\n";

		out << "## 実機フィードバック round " << round.round << " — `" << round.build.plugin << "` "
			<< round.build.commit;
		if (!round.build.platform.empty())
			out << "（" << round.build.platform << "）";
		out << "\n\n";

		out << "**結果: " << importStatusWord(outcome.status) << "** ／ 対象 `" << shownFile << "`";
		const std::string size = formatBytes(round.bytes);
		if (!size.empty())
			out << "（" << size << "）";
		if (round.seconds > 0.0)
			out << " ／ 所要 " << formatSeconds(round.seconds);
		if (!round.startedAt.empty())
			out << " ／ " << round.startedAt;
		out << "\n";

		// **所見をいちばん上に置く。** 数字は下に全部あるが、絵を見た人にしか書けない
		// ことはここにしかない。
		if (!round.note.empty())
		{
			out << "\n### 実機を見ての所見\n\n";
			std::istringstream noteLines(clean(round.note));
			std::string line;
			while (std::getline(noteLines, line))
				out << "> " << line << "\n";
		}

		// 前の周からの差分。1 周目（previousTally が空）では節ごと出さない。
		const std::string diff = formatTallyDiff(round.previousTally, tally);
		if (!round.previousTally.empty())
		{
			out << "\n### 前の周（round " << (round.round - 1);
			if (!round.previousCommit.empty())
				out << " / " << round.previousCommit;
			out << "）からの変化\n\n";
			out << (diff.empty() ? "内訳に変化はありません。\n" : diff);
		}

		// 要素ごとの内訳。命令が 0 の要素は出さない（診断ログと同じ方針）。
		out << "\n### 要素の内訳\n\n";
		out << "| 要素 | 描けた / 命令 |\n| --- | --- |\n";
		bool anyRow = false;
		for (const ElementRow& row : rows)
		{
			if (row.commands == 0)
				continue;
			anyRow = true;
			out << "| " << row.label << " | " << row.placed << " / " << row.commands << " "
				<< row.unit << " |\n";
		}
		if (!anyRow)
			out << "| （命令が 1 つも出ていません） | 0 / 0 |\n";

		// 描画側が持ち帰った異常と記録。異常は**折り畳まない**（読ませたいものを隠さない）。
		if (!counts.diagnostics.empty())
			out << "\n### 注意（描画側の異常）\n\n" << codeBlock(clean(counts.diagnostics));
		if (!counts.notes.empty())
			out << "\n<details><summary>記録（用紙の割り付けなど）</summary>\n\n"
				<< codeBlock(clean(counts.notes)) << "\n</details>\n";

		// **末尾の案内を先に組む。** 下のログはコメント 1 通の上限に収めるために削るので、
		// 「あとどれだけ入るか」を知るには末尾の長さが先に要る（案内を削って字数を稼ぐ
		// ことはしない——次に何が起きるかが読めなくなると、往復が人の手に戻る）。
		std::ostringstream tail;
		tail << "\n---\n";
		if (round.autoContinue)
			tail << "この投稿は VectorWorks 上の開発版プラグインが自動生成しました。**`"
				 << round.build.branch
				 << "` へ修正を push すると、この Vectorworks が新しい dev "
					"ビルドを自動で取り込み直し、round "
				 << (round.round + 1) << " を投稿します**（再起動も再選択も要りません）。\n";
		else
			tail << "この投稿は VectorWorks 上の開発版プラグインが自動生成しました（自動継続は"
					"切ってあるので、次の周は手動で走らせます）。\n";
		tail << "数字だけで判断が付かないときは、**実機で確かめてほしい点を返信で挙げて**"
				"ください（絵を見られるのは人だけです）。\n";
		if (round.anonymize)
			tail << "<sub>対象ファイル名とユーザー名は伏せてあります（同じ入力なら同じ仮名に"
					"なります）。</sub>\n";
		const std::string footer = tail.str();

		// 診断ログの全文。**折り畳む**——ふだんは読まないが、要るときは全部要る。
		if (!round.log.empty())
		{
			std::string log = clean(round.log);
			// コメント 1 通の上限に収める。削るのは**古いほう**（結果に近い末尾を残す）。
			// 差し引くのは「ここまでの本文＋末尾の案内＋折り畳みの飾り」で、飾りの分は
			// 多めに見ておく（1 通が上限を超えると投稿そのものが弾かれる）。
			const std::size_t used = out.str().size() + footer.size() + 256;
			const std::size_t budget =
				kMaxFeedbackCommentBytes > used ? kMaxFeedbackCommentBytes - used : 0;
			if (log.size() > budget)
			{
				const std::string omitted =
					"…（前半 " + std::to_string(log.size() - budget) + " バイトを省略）…\n";
				log = omitted + (budget > omitted.size()
									 ? log.substr(log.size() - budget + omitted.size())
									 : std::string());
			}
			out << "\n<details><summary>診断ログ（全文）</summary>\n\n"
				<< codeBlock(log) << "\n</details>\n";
		}

		out << footer;
		return out.str();
	}
} // namespace HomeskzIfcImport::parse
