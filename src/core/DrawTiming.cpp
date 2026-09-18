//
//	core/DrawTiming.cpp
//
//	描画の区間計測（core/DrawTiming.h）の実装。時計と文字列の整形だけで、
//	VectorWorks SDK も STEP も知らない。
//

#include "core/DrawTiming.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace HomeskzIfcImport::core
{
	namespace
	{
		// 数を「12345ms」「61.4ms/回」のように整える。TXString も iostream も使わない
		// （core/ は標準ライブラリだけで完結させる。core/Trace.h と同じ流儀）。
		std::string formatNumber(const char* form, double value)
		{
			std::array<char, 64> buffer{};
			std::snprintf(buffer.data(), buffer.size(), form, value);
			return {buffer.data()};
		}
	} // namespace

	void TimingTable::add(std::string_view name, double milliseconds)
	{
		// 負の経過時間は 0 として積む（合計が減ると読む側が必ず混乱する）。
		const double elapsed = milliseconds > 0.0 ? milliseconds : 0.0;

		const auto found = std::ranges::find_if(fEntries, [name](const Entry& entry)
												{ return entry.name == name; });
		if (found != fEntries.end())
		{
			found->milliseconds += elapsed;
			++found->count;
			return;
		}
		fEntries.push_back(Entry{std::string(name), elapsed, 1});
	}

	void TimingTable::clear()
	{
		fEntries.clear();
	}

	std::vector<TimingTable::Entry> TimingTable::sorted() const
	{
		std::vector<Entry> entries = fEntries;
		// **安定ソート**にする。同じ時間の区間は積まれた順のまま残るので、走らせるたびに
		// 並びが入れ替わらない（CLAUDE.md「決定性を守る」）。
		std::ranges::stable_sort(entries, [](const Entry& a, const Entry& b)
								 { return a.milliseconds > b.milliseconds; });
		return entries;
	}

	double TimingTable::total() const
	{
		double sum = 0.0;
		for (const Entry& entry : fEntries)
			sum += entry.milliseconds;
		return sum;
	}

	std::string TimingTable::format(std::string_view heading) const
	{
		if (fEntries.empty())
			return {};

		std::string text(heading);
		text += "（合計 ";
		text += formatNumber("%.0f", total());
		text += "ms）:";

		for (const Entry& entry : sorted())
		{
			text += "\n  ";
			text += entry.name;
			text += " ";
			text += formatNumber("%.0f", entry.milliseconds);
			text += "ms（";
			text += std::to_string(entry.count);
			text += " 回・";
			// 1 回あたり。回数が 0 になることは無い（add が必ず 1 以上にする）。
			const auto calls = static_cast<double>(entry.count);
			text += formatNumber("%.2f", entry.milliseconds / calls);
			text += "ms/回）";
		}
		return text;
	}

	TimingTable& drawTiming()
	{
		// 関数内 static。翻訳単位をまたぐ初期化順の問題が起きない
		// （tests/TestFramework.h の Registry と同じ形）。
		static TimingTable table;
		return table;
	}
} // namespace HomeskzIfcImport::core
