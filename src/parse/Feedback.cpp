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
			out << "\n\n図面には何も描いていません。「準備:」の行の理由を取り除いてから、"
				   "もう一度実行してください。";
			return out.str();
		case TestRoundOutcome::ImportFailed:
			out << "取り込みがエラーで中断しました。";
			if (!detail.empty())
				out << "\n\n" << detail;
			out << "\n\nくわしい原因は診断ログにあります。";
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

	namespace
	{
		// 取り込み前の図面の状態（報告の「図面の状態:」1 行）。判断材料は描画側の実測
		// （DrawCounts::existingLayers ＝取り込み前から在ったレイヤ）。
		//
		// **判定はしない。** 毎周テンプレートから開いた新しい図面へ描画するので（M39）、同じ
		// テンプレートの周どうしでこの枚数が違えば図面の用意を疑う——その比較は前の周の報告を
		// 持っている側（Claude）が行う（M43。M42 までは 1 周目の顔ぶれを記憶して照合していた）。
		// テンプレートに「共通」等が最初から在ると 0 にはならないので、枚数だけで良し悪しを
		// 書かない。
		//
		// 【名前を載せない】枚数だけを出す（顔ぶれは診断ログにある）。
		std::string drawingStateLine(const core::DrawCounts& counts)
		{
			const std::size_t now = counts.existingLayers.size();
			if (now == 0)
				return "まっさらな図面から取り込みました。";
			return "取り込み前から在ったレイヤ " + std::to_string(now) +
				   " 枚に描きました（テンプレートにもとから在るもの。同じテンプレートの周どうしで"
				   "枚数が違えば、図面の用意を疑ってください）。";
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

		std::ostringstream out;
		out << "## 実機テスト — `" << round.build.plugin << "` " << round.build.commit;
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
		out << "\n図面の状態: " << drawingStateLine(counts) << "\n";
		// **図面をどう用意したか（準備:）を、図面の状態のすぐ隣に置く。** 診断ログにも同じ行が
		// あるが、ログは上限で切り詰められるので、そこだけを頼りにすると読めない周が出る
		// （実機 round 2 で実際に省略部分へ入り読めなかった）。
		if (!round.preparation.empty())
			out << round.preparation << "\n";

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
