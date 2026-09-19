//
//	parse/Regression.cpp
//
//	回帰テストの結果と基準（意図と制約は parse/Regression.h）。
//
//	**比較の実体は実機フィードバックの往復と同じもの**（parse/Feedback の `formatTally` /
//	`formatTallyDiff`）。周と周を突き合わせるのも、基準と今回を突き合わせるのも「内訳が
//	どう動いたか」であって、同じ比較を 2 通り書く理由が無い。
//

#include "parse/Regression.h"

#include "core/FixtureScan.h"
#include "parse/Feedback.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	namespace
	{
		// 秒を基準ファイルへ書く形（小数 1 桁）。**人向けの言い方（formatDuration）とは
		// 別**——こちらは読み戻す値なので、桁と区切りが環境で揺れない形にする。
		std::string formatSeconds(double seconds)
		{
			std::ostringstream out;
			out << std::fixed << std::setprecision(1) << seconds;
			return out.str();
		}

		// 基準ファイルの秒を読む（読めなければ 0）。
		double parseSeconds(const std::string& text)
		{
			try
			{
				return std::stod(text);
			}
			catch (...)
			{
				// **壊れた行で読むのをやめない**（parse/Regression.h「壊れた行は飛ばす」）。
				return 0.0;
			}
		}

		// 1 行ずつ渡す（CR は落とす）。基準ファイルは利用者の機械で作られるので、
		// Windows で書いて mac で読む（その逆も）が普通に起きる。
		// **転送参照で受けない**（`Body&&`）。呼ぶのは繰り返しの中なので転送しようがなく、
		// clang-tidy の cppcoreguidelines-missing-std-forward がそれを咎める。
		template <class Body> void forEachLine(const std::string& text, const Body& body)
		{
			std::istringstream in(text);
			std::string line;
			while (std::getline(in, line))
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				body(line);
			}
		}

		// "key=value" を割る（`=` が無ければ false）。**最初の `=` で割る**——値の側に
		// `=` を含む物件名がありうる。
		bool splitKeyValue(const std::string& line, std::string& key, std::string& value)
		{
			const std::string::size_type eq = line.find('=');
			if (eq == std::string::npos)
				return false;
			key = line.substr(0, eq);
			value = line.substr(eq + 1);
			return !key.empty();
		}

		// **複数行を 1 行に畳む**（描画側が持ち帰った異常 `core::DrawCounts::diagnostics` と、
		// 取り込みが例外で落ちたときのエラー本文）。
		// 基準ファイルは 1 行 1 鍵なので改行を入れられず、そもそも全文はログの領分である
		// ——ここに要るのは「何が起きたか」を思い出せるだけの手掛かりで、**先頭の数行**で
		// 足りる（続きは、その 1 件を本番の取り込みで走らせ直せば診断ログに出る）。
		//
		// **行の数で切る**（バイト数では切らない）——UTF-8 の途中で切ると壊れた文字が
		// 基準ファイルへ入る（実機フィードバックで一度それをやり、GitHub に弾かれて
		// 投稿がまるごと落ちた。docs/DEV-NOTES.md M23）。
		std::string flattenLines(const std::string& text)
		{
			constexpr int kMaxLines = 3;
			std::istringstream in(text);
			std::string line;
			std::string flat;
			int taken = 0;
			while (std::getline(in, line))
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line.empty())
					continue;
				if (taken >= kMaxLines)
				{
					flat += " …";
					break;
				}
				if (!flat.empty())
					flat += " / ";
				flat += line;
				++taken;
			}
			return flat;
		}

		// 突き合わせ 1 件ぶんの内訳の差分。`formatTallyDiff` は**前回が空なら空**を返す
		// （周回どうしの差分ではそれが正しい）ので、ここでは基準が空だった場合の言い方を
		// 自分で足す——**「基準が空だったから何も言わない」では、退行を黙って落とす**。
		std::string tallyDiffOf(const std::string& before, const std::string& after)
		{
			if (before == after)
				return {};
			// **const にしない**——返すときに move されなくなり、clang-tidy の
			// performance-no-automatic-move がエラーになる（draw/DocumentFile.cpp の
			// FreshTempPath にも同じ但し書きがある）。
			std::string diff = formatTallyDiff(before, after);
			if (!diff.empty())
				return diff;
			std::ostringstream out;
			out << "- 内訳: " << (before.empty() ? std::string("（基準は空）") : before) << " → "
				<< (after.empty() ? std::string("（今回は空）") : after) << "\n";
			return out.str();
		}

		// 突き合わせの分類を短い日本語に（ログの 1 行に出す）。
		const char* verdictWord(RegressionVerdict verdict)
		{
			switch (verdict)
			{
			case RegressionVerdict::Same:
				return "基準どおり";
			case RegressionVerdict::Changed:
				return "動いた";
			case RegressionVerdict::Added:
				return "基準に無し";
			case RegressionVerdict::Missing:
				break;
			}
			return "今回は走らず";
		}
	} // namespace

	// -----------------------------------------------------------------------
	RegressionEntry regressionEntry(const std::string& name, const core::Document& document,
									const core::DrawCounts& counts, double seconds)
	{
		RegressionEntry entry;
		entry.name = name;
		// **結末も内訳も、取り込みの報告が使っているものをそのまま使う**（parse/Summary）。
		// ここで独自の数え方を作ると、同じ取り込みが場所によって違う結末を名乗る。
		entry.status = importStatusWord(importOutcome(document, counts).status);
		entry.tally = formatTally(elementRows(document, counts));
		entry.seconds = seconds;
		// **異常の手掛かりも残す。** 「問題あり」がずっと続いているとき、その理由が
		// 基準にも記録から消えていると、毎回ゼロから切り分け直すことになる。
		entry.detail = flattenLines(counts.diagnostics);
		return entry;
	}

	RegressionEntry regressionErrorEntry(const std::string& name, const std::string& detail,
										 double seconds)
	{
		RegressionEntry entry;
		entry.name = name;
		entry.status = kRegressionErrorWord;
		entry.seconds = seconds;
		// **改行を持ち込ませない。** 呼び出し側が渡すのは取り込みのエラー本文（複数行）で、
		// 基準ファイルは 1 行 1 鍵なので、そのまま入れると次の行が鍵として読まれて記録が
		// 壊れる。畳み方は描画側の注意と同じ（上の flattenLines）。
		entry.detail = flattenLines(detail);
		return entry;
	}

	// -----------------------------------------------------------------------
	std::string regressionBaselinePath(const std::string& folder)
	{
		return core::folderFilePath(folder, kBaselineFileName);
	}

	bool readRegressionBaseline(const std::string& path, std::string& text)
	{
		text.clear();
		if (path.empty())
			return false;
		const std::ifstream in(path, std::ios::binary);
		if (!in)
			return false;
		std::ostringstream buffer;
		buffer << in.rdbuf();
		text = buffer.str();
		return true;
	}

	bool writeRegressionBaseline(const std::string& path, const std::string& text)
	{
		if (path.empty())
			return false;
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;
		out.write(text.data(), static_cast<std::streamsize>(text.size()));
		return out.good();
	}

	// -----------------------------------------------------------------------
	std::string formatRegressionBaseline(const std::vector<RegressionEntry>& entries,
										 const BuildInfo& build, const std::string& recordedAt)
	{
		std::ostringstream out;
		out << "# みんなの構造設計支援 — 回帰テストの基準（プラグインが書きます）\n";
		out << "# 消せば、次に実行したときの結果が新しい基準になります。\n";
		out << "version=1\n";
		out << "recorded=" << recordedAt << "\n";
		out << "plugin=" << build.plugin << "\n";
		out << "channel=" << build.channel << "\n";
		out << "build=" << build.commit << "\n";
		out << "branch=" << build.branch << "\n";
		for (const RegressionEntry& entry : entries)
		{
			out << "\n";
			// **`file=` が 1 件の始まり。** 読む側はこの鍵だけを頼りに区切る。
			out << "file=" << entry.name << "\n";
			out << "status=" << entry.status << "\n";
			out << "seconds=" << formatSeconds(entry.seconds) << "\n";
			out << "tally=" << entry.tally << "\n";
			if (!entry.detail.empty())
				out << "detail=" << entry.detail << "\n";
		}
		return out.str();
	}

	std::vector<RegressionEntry> parseRegressionBaseline(const std::string& text)
	{
		std::vector<RegressionEntry> entries;
		forEachLine(text,
					[&entries](const std::string& line)
					{
						if (line.empty() || line[0] == '#')
							return;
						std::string key;
						std::string value;
						if (!splitKeyValue(line, key, value))
							return; // 壊れた行は飛ばして読み続ける
						if (key == "file")
						{
							// 名前の無い件は突き合わせられない（鍵が無い）ので採らない。
							if (value.empty())
								return;
							RegressionEntry entry;
							entry.name = value;
							entries.push_back(std::move(entry));
							return;
						}
						if (entries.empty())
							return; // 見出しの鍵（version / build …）。ここでは使わない
						RegressionEntry& entry = entries.back();
						if (key == "status")
							entry.status = value;
						else if (key == "tally")
							entry.tally = value;
						else if (key == "seconds")
							entry.seconds = parseSeconds(value);
						else if (key == "detail")
							entry.detail = value;
						// 知らない鍵は飛ばす（古い版が書いたものも読める）
					});
		return entries;
	}

	std::string regressionBaselineOrigin(const std::string& text)
	{
		std::string recorded;
		std::string build;
		std::string branch;
		bool inEntries = false;
		forEachLine(text,
					[&](const std::string& line)
					{
						if (inEntries || line.empty() || line[0] == '#')
							return;
						std::string key;
						std::string value;
						if (!splitKeyValue(line, key, value))
							return;
						if (key == "file")
						{
							inEntries = true; // 見出しはここまで
							return;
						}
						if (key == "recorded")
							recorded = value;
						else if (key == "build")
							build = value;
						else if (key == "branch")
							branch = value;
					});
		if (recorded.empty() && build.empty())
			return {};
		std::string origin = recorded;
		if (!build.empty())
		{
			if (!origin.empty())
				origin += " / ";
			origin += build;
			if (!branch.empty())
				origin += " (" + branch + ")";
		}
		return origin;
	}

	// -----------------------------------------------------------------------
	std::vector<RegressionCompare> compareRegression(const std::vector<RegressionEntry>& baseline,
													 const std::vector<RegressionEntry>& current)
	{
		std::map<std::string, const RegressionEntry*> byName;
		for (const RegressionEntry& entry : baseline)
			byName.emplace(entry.name, &entry);

		std::vector<RegressionCompare> compares;
		compares.reserve(current.size() + baseline.size());
		std::map<std::string, bool> matched;

		for (const RegressionEntry& now : current)
		{
			RegressionCompare compare;
			compare.name = now.name;
			compare.status = now.status;
			compare.detail = now.detail;
			compare.seconds = now.seconds;
			// **基準に依らない異常。** 基準がその異常ごと記録されていても見逃さないよう、
			// 分類（下の verdict）とは別に立てる（parse/Regression.h「2 つある」）。
			compare.abnormal = now.status != importStatusWord(ImportStatus::Success);

			const auto it = byName.find(now.name);
			if (it == byName.end())
			{
				compare.verdict = RegressionVerdict::Added;
				compares.push_back(std::move(compare));
				continue;
			}
			matched[now.name] = true;
			const RegressionEntry& was = *it->second;
			compare.statusBefore = was.status;
			compare.diff = tallyDiffOf(was.tally, now.tally);
			compare.verdict = (was.tally == now.tally && was.status == now.status)
								  ? RegressionVerdict::Same
								  : RegressionVerdict::Changed;
			compares.push_back(std::move(compare));
		}

		// **基準にあるのに今回走らなかったものを必ず出す。** リンクが切れた・名前が
		// 変わったに気付ける唯一の場所で、黙って落とすと「通ったこと」と区別が付かない。
		for (const RegressionEntry& was : baseline)
		{
			if (matched.find(was.name) != matched.end())
				continue;
			RegressionCompare compare;
			compare.name = was.name;
			compare.verdict = RegressionVerdict::Missing;
			compare.status = was.status;
			compare.statusBefore = was.status;
			compares.push_back(std::move(compare));
		}
		return compares;
	}

	RegressionSummary summarizeRegression(const std::vector<RegressionCompare>& compares,
										  bool baselineKnown, bool cancelled)
	{
		RegressionSummary summary;
		summary.baselineKnown = baselineKnown;
		summary.cancelled = cancelled;
		for (const RegressionCompare& compare : compares)
		{
			if (compare.verdict != RegressionVerdict::Missing)
				++summary.total;
			if (compare.abnormal)
				++summary.abnormal;
			switch (compare.verdict)
			{
			case RegressionVerdict::Same:
				++summary.same;
				break;
			case RegressionVerdict::Changed:
				++summary.changed;
				break;
			case RegressionVerdict::Added:
				++summary.added;
				break;
			case RegressionVerdict::Missing:
				++summary.missing;
				break;
			}
		}
		return summary;
	}

	// -----------------------------------------------------------------------
	std::string formatRegressionPrompt(std::size_t found, std::size_t baselineCount,
									   const std::string& origin)
	{
		std::ostringstream out;
		out << "回帰テスト: " << found << " 件の IFC が見つかりました。\n";
		if (origin.empty())
			out << "基準がまだ無いので、今回の結果を基準として記録します。\n";
		else
			out << "基準（" << baselineCount << " 件 / " << origin << "）と引き比べます。\n";
		// **待つ時間を先に言う。** 1 件あたり 1 分前後かかるので、件数によっては 1 時間を
		// 超える——押してから気付くのでは遅い。
		out << "\n1 件ずつ取り込むので時間がかかります（1 件 1 分前後）。\n";
		// **図面がどうなるかも先に言う。** 走り終えたとき画面に出ているのは作業用の複製で、
		// もとの図面ではない——知らないと「上書きされた」と読む。
		out << "いま開いている図面を作業用に複製し、その複製へ 1 件ずつ取り込みます"
			   "（もとのファイルは変わりません）。\n";
		out << "終わるまで図面を触らないでください。";
		return out.str();
	}

	std::string formatRegressionResult(const RegressionSummary& summary, bool baselineWritten)
	{
		std::ostringstream out;
		out << "回帰テスト: " << summary.total << " 件を取り込みました。\n";
		if (summary.cancelled)
			out << "途中で中止したので、残りは走っていません。\n";
		if (!summary.baselineKnown)
		{
			out << "基準がありませんでした。";
			out << (baselineWritten ? "今回の結果を基準として記録しました。\n"
									: "基準を書けませんでした（ログに理由があります）。\n");
		}
		else
		{
			out << "基準どおり " << summary.same << " 件 / 動いた " << summary.changed
				<< " 件 / 基準に無し " << summary.added << " 件 / 今回は走らず " << summary.missing
				<< " 件\n";
			if (baselineWritten)
				out << "基準を今回の結果で更新しました。\n";
		}
		if (summary.abnormal > 0)
			out << "結末が「成功」でなかったものが " << summary.abnormal << " 件あります。\n";
		out << "\n件ごとの内訳は「ログを表示」で読めます。";
		return out.str();
	}

	std::string formatRegressionLog(const std::vector<RegressionCompare>& compares,
									const RegressionSummary& summary,
									const std::vector<std::string>& notes)
	{
		std::ostringstream out;
		out << "=== 回帰テストの結果 ===\n";
		out << "対象: " << summary.total << " 件";
		out << " / 基準: " << (summary.baselineKnown ? "あり" : "なし");
		if (summary.cancelled)
			out << " / 途中で中止";
		out << "\n";
		if (summary.baselineKnown)
			out << "基準どおり " << summary.same << " / 動いた " << summary.changed
				<< " / 基準に無し " << summary.added << " / 今回は走らず " << summary.missing
				<< "\n";
		out << "結末が「成功」でなかった: " << summary.abnormal << " 件\n";

		out << "\n=== 件ごと ===\n";
		for (const RegressionCompare& compare : compares)
		{
			out << compare.name << ": " << compare.status;
			// **基準の結末が違ったなら必ず並べる**（「問題あり → 成功」も動きである）。
			if (!compare.statusBefore.empty() && compare.statusBefore != compare.status)
				out << "（基準は " << compare.statusBefore << "）";
			out << " / " << verdictWord(compare.verdict);
			if (compare.verdict != RegressionVerdict::Missing && compare.seconds > 0.0)
				out << " / " << formatDuration(compare.seconds);
			out << "\n";
			if (!compare.detail.empty())
				out << "  詳細: " << compare.detail << "\n";
			out << compare.diff;
		}

		if (!notes.empty())
		{
			out << "\n=== フォルダの走査 ===\n";
			for (const std::string& note : notes)
				out << note << "\n";
		}
		return out.str();
	}
} // namespace HomeskzIfcImport::parse
