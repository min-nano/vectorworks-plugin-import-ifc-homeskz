//
//	parse/Feedback.cpp
//
//	実機フィードバック本文の実装（意図は parse/Feedback.h 参照）。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない。
//

#include "parse/Feedback.h"
#include "core/Document.h"
#include "parse/Summary.h"

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	namespace
	{
		// 末尾のファイル名（区切りは POSIX と Windows の両方を考慮する）。
		std::string fileNameOf(const std::string& path)
		{
			const std::string::size_type pos = path.find_last_of("/\\");
			if (pos == std::string::npos)
				return path;
			return path.substr(pos + 1);
		}

		// 内訳の 1 行表現を要素ごとに切り分ける（"ラベル:描けた/命令" の並び）。
		// 不正な要素はスキップする——古い版が書いた記憶を読めないことで往復を止めない。
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

	std::string formatTestRoundResult(TestRoundOutcome outcome, const std::string& detail)
	{
		std::ostringstream out;
		switch (outcome)
		{
		case TestRoundOutcome::Completed:
			out << "実機テストを終えました。";
			if (!detail.empty())
				out << "\n\n" << detail;
			return out.str();
		case TestRoundOutcome::DocumentFailed:
			out << "描く図面を用意できなかったので、この周は走らせませんでした。";
			if (!detail.empty())
				out << "\n\n" << detail;
			// **描画しなかったことを明言する。** 「0 件」と紛らわしくしない
			// （実機 round 9 でそれが起きた）。
			out << "\n\n図面には何も描いていません。試したい図面を開いてから、メニュー"
				   "「実機テストを実行…」をもう一度実行してください（その図面をテンプレート"
				   "として採り直します）。";
			return out.str();
		case TestRoundOutcome::ImportFailed:
			out << "取り込みがエラーで中断しました。";
			if (!detail.empty())
				out << "\n\n" << detail;
			out << "\n\nくわしい原因は診断ログにあります。";
			return out.str();
		case TestRoundOutcome::NotRemembered:
			// **名指しの無い MCP の周から 1 周目は起こせない**（何を取り込むかが分からない）。
			// 何をすれば続けられるかを、頼んだ側（Claude）がそのまま使える形で示す（M40）。
			out << "実機テストの 1 周目がまだ済んでいません。vw_run_test に ifc（取り込む IFC の"
				   "絶対パス）と template（テンプレートの .sta の絶対パス。リポジトリの "
				   "tests/fixtures/Default.sta）を渡せば、尋ねずに 1 周目から始めます。"
				   "人が選ぶなら、Vectorworks のメニュー「実機テストを実行…」を 1 度実行して"
				   "ください（2 周目からは同じ条件でダイアログ無しに走ります）。";
			if (!detail.empty())
				out << "\n\n" << detail;
			return out.str();
		case TestRoundOutcome::InvalidRequest:
			out << "頼まれた条件では実機テストを始められませんでした。"
				   "図面には何も描いていません。";
			if (!detail.empty())
				out << "\n\n" << detail;
			return out.str();
		}
		return out.str();
	}

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

	namespace
	{
		// 図面が「取り込み前」へ戻してあるか（報告の「図面の状態:」1 行）。
		//
		// 【なぜ真偽 1 つでは足りないのか】判断材料は描画側の実測
		// （DrawCounts::existingLayers ＝取り込み前から在ったレイヤ）だが、**図面のテンプレートに
		// 「共通」等が最初から在ると、1 周目からこれは空にならない**。つまり
		// 「前の周を戻し忘れた」と「もともと在った」を真偽では区別できない（実機の指摘。
		// docs/DEV-NOTES.md M23「基準は 1 周目に採る」）。そこで**1 周目の顔ぶれを基準として
		// 記憶し**（core::FeedbackSession::baselineLayers）、次の周からはそこへ戻っているかを
		// 照合する。
		//
		// 【なぜ人に訊かないのか】押したかどうかは戻したかどうかではない。確認ダイアログは
		// 「押したが戻していない」を防げないので、**実測だけで判定する**。
		//
		// 【名前を載せない】枚数と判定だけを出す（読む側が知りたいのは「戻っていたか」で、
		// 顔ぶれは診断ログにある）。
		std::string restoredStateLine(const FeedbackRound& round, const core::DrawCounts& counts)
		{
			const std::size_t now = counts.existingLayers.size();
			if (!round.baselineKnown)
			{
				// 1 周目（または古い版が書いた記憶）。**基準を記録したことだけ書く。**
				// ここで「戻っています」と書くと、次の周と照合したときに事実と食い違う。
				if (now == 0)
					return "まっさらな図面から取り込みました（次の周はここへ戻っているかを見ます）"
						   "。";
				return "取り込み前から在ったレイヤ " + std::to_string(now) +
					   " 枚に描きました（図面のテンプレートにもとから在るもの）。"
					   "**この顔ぶれを基準にします**——次の周はここへ戻っているかを見ます。";
			}

			// 基準にあるものが今回も「取り込み前から在った」なら、そこは戻っている。
			// 基準に無いものが増えていたら、それは**前の周がこの図面へ残した**レイヤである。
			std::size_t extra = 0;
			for (const std::string& layer : counts.existingLayers)
			{
				if (std::ranges::find(round.baselineLayers, layer) == round.baselineLayers.end())
					++extra;
			}
			std::size_t missing = 0;
			for (const std::string& layer : round.baselineLayers)
			{
				if (std::ranges::find(counts.existingLayers, layer) == counts.existingLayers.end())
					++missing;
			}

			if (extra > 0)
				return "**前の周の図が残ったまま重ねて描きました**（1 周目の基準に無いレイヤ " +
					   std::to_string(extra) +
					   " 枚へも描いています）。**絵の破綻をそのまま実装の"
					   "せいにしないでください。**";
			if (missing > 0)
				return "1 周目の基準にあったレイヤ " + std::to_string(missing) +
					   " 枚が見当たりません（別の図面か、テンプレートが変わった可能性があります）"
					   "。";
			return "取り込み前の状態へ戻してから実行されています（1 周目と同じ " +
				   std::to_string(now) + " 枚）。";
		}
	} // namespace

	namespace
	{
		// 切り詰めの開始位置 offset を、UTF-8 の文字境界の次の行頭まで進めて返す。
		// 継続バイト（0b10xxxxxx）の間は前へ進め、そのうえで**次の行頭まで**進める。
		// **進めるだけで戻らない**ので、切り詰めた結果が予算を超えることはない。
		//
		// **切り詰めは UTF-8 の文字境界で行う。** 単純に「末尾から N バイト」を切り出すと、
		// 3 バイトの日本語の途中で切れて**壊れた UTF-8** ができる。PR へ投稿していた頃は
		// GitHub がそれを 400 で拒否し、その周の投稿がすべて失敗した（実機で発生。
		// docs/DEV-NOTES.md M25）。いまの読み手（MCP の JSON）も壊れた UTF-8 は受け付けない。
		// 行頭まで進めるのは、行の途中から始まるログは読む側にも読みにくく、どのみち削除する
		// なら行単位で削除するほうがよいため。
		std::size_t utf8LineStart(const std::string& text, std::size_t offset)
		{
			if (offset >= text.size())
				return text.size();
			while (offset < text.size() &&
				   (static_cast<unsigned char>(text[offset]) & 0xC0U) == 0x80U)
				++offset;
			const std::string::size_type nl = text.find('\n', offset);
			return nl == std::string::npos ? text.size() : nl + 1;
		}
	} // namespace

	std::string keepTail(const std::string& text, std::size_t maxBytes)
	{
		if (text.size() <= maxBytes)
			return text;
		// 案内の 1 行ぶんも予算を消費するので、その分だけ多めに削除する。行数字を含めて高々
		// 64 バイト。
		constexpr std::size_t kOmittedNoticeBytes = 64;
		std::size_t start = text.size() - maxBytes + kOmittedNoticeBytes;
		if (start > text.size())
			start = text.size();
		start = utf8LineStart(text, start); // **文字と行の境界まで進める**
		return "…（前半 " + std::to_string(start) + " バイトを省略）…\n" + text.substr(start);
	}

	std::string formatTestRoundReport(const FeedbackRound& round, const core::Document& document,
									  const core::DrawCounts& counts)
	{
		const ImportOutcome outcome = importOutcome(document, counts);
		const std::vector<ElementRow> rows = elementRows(document, counts);
		const std::string tally = formatTally(rows);

		std::ostringstream out;
		out << "## 実機テスト round " << round.round << " — `" << round.build.plugin << "` "
			<< round.build.commit;
		if (!round.build.branch.empty())
			out << "（" << round.build.branch << "）";
		if (!round.build.platform.empty())
			out << " " << round.build.platform;
		out << "\n\n";

		out << "**結果: " << importStatusWord(outcome.status) << "** ／ 対象 `"
			<< fileNameOf(round.ifcPath) << "`";
		const std::string size = formatBytes(round.bytes);
		if (!size.empty())
			out << "（" << size << "）";
		if (round.seconds > 0.0)
			out << " ／ 所要 " << formatSeconds(round.seconds);
		if (!round.startedAt.empty())
			out << " ／ " << round.startedAt;
		out << "\n";

		// **取り込み前へ戻っていたか。** 描画先の図面に前の周の図形が残っていると図形が二重
		// になる（M39 からは毎周テンプレートから開いた新しい図面へ描画するので、崩れるのは
		// テンプレートそのものが汚れていたときか図面の用意に失敗したとき）——ところが**内訳の
		// 数字は命令の数なので、戻したかどうかで 1 つも変わらない**。読む側（Claude）が
		// 「描画結果が壊れているのは実装のせいか、戻し損ねか」を切り分けられるよう、1 行を
		// 必ず載せる。
		out << "\n図面の状態: " << restoredStateLine(round, counts) << "\n";
		// **図面をどう用意したか（準備:）を、図面の状態のすぐ隣に置く。** 診断ログにも同じ行が
		// あるが、ログは上限で切り詰められるので、そこだけを頼りにすると読めない周が出る
		// （実機 round 2 で実際に省略部分へ入り読めなかった）。
		if (!round.preparation.empty())
			out << round.preparation << "\n";

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

		// 描画側が記録した異常と記録。
		if (!counts.diagnostics.empty())
			out << "\n### 注意（描画側の異常）\n\n"
				<< codeBlock(foldRecordLines(counts.diagnostics));
		if (!counts.notes.empty())
			out << "\n### 記録（用紙の割り付けなど）\n\n"
				<< codeBlock(foldRecordLines(counts.notes));

		// 診断ログの全文。報告 1 つの上限に収める。削るのは**古いほう**（結果に近い末尾を
		// 残す）。装飾の分は多めに見積もる。
		if (!round.log.empty())
		{
			const std::size_t used = out.str().size() + 256;
			const std::size_t budget = kMaxTestReportBytes > used ? kMaxTestReportBytes - used : 0;
			const std::string log = keepTail(round.log, budget);
			out << "\n### 診断ログ\n\n" << codeBlock(log);
		}
		return out.str();
	}
} // namespace HomeskzIfcImport::parse
