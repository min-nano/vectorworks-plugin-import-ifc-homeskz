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
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
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
		fNested.clear();
		fDepth = 0;
	}

	bool TimingTable::enterScope()
	{
		const bool nested = fDepth > 0;
		++fDepth;
		return nested;
	}

	void TimingTable::leaveScope()
	{
		// 入っていないのに出ようとするのは呼び出し側の取り違えだが、ここで 0 を下回らせると
		// 以後の入れ子を見逃す。0 で止める。
		if (fDepth > 0)
			--fDepth;
	}

	void TimingTable::noteNested(std::string_view name)
	{
		if (std::ranges::find(fNested, name) != fNested.end())
			return;
		fNested.emplace_back(name);
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

	namespace
	{
		// 区間 1 つを「名前 12ms（3 回・4.00ms/回）」の形にする。
		std::string formatEntry(const TimingTable::Entry& entry)
		{
			std::string text = entry.name;
			text += " ";
			text += formatNumber("%.0f", entry.milliseconds);
			text += "ms（";
			text += std::to_string(entry.count);
			text += " 回・";
			// 1 回あたり。回数が 0 になることは無い（add が必ず 1 以上にする）。
			const auto calls = static_cast<double>(entry.count);
			text += formatNumber("%.2f", entry.milliseconds / calls);
			text += "ms/回）";
			return text;
		}

		// 区間名の「分類:項目」を分けた 1 グループ。分類の無い名前は項目を持たない 1 行になる。
		struct TimingGroup
		{
			std::string category;
			double milliseconds = 0.0;
			std::vector<TimingTable::Entry> entries; // 名前は「項目」だけに切り詰めてある
			bool flat = false;						 // 分類の無い名前（1 行で出す）
		};

		// 区間を分類ごとにまとめ、**分類の合計の大きい順**に並べる。分類の中は sorted() の
		// 順（時間の大きい順）を保つ。どちらも安定な並べ替えで、決定性を保つ。
		std::vector<TimingGroup> groupByCategory(const std::vector<TimingTable::Entry>& sorted)
		{
			std::vector<TimingGroup> groups;
			for (const TimingTable::Entry& entry : sorted)
			{
				const std::size_t colon = entry.name.find(':');
				if (colon == std::string::npos)
				{
					groups.push_back(TimingGroup{entry.name, entry.milliseconds, {entry}, true});
					continue;
				}
				const std::string category = entry.name.substr(0, colon);
				auto found =
					std::ranges::find_if(groups, [&category](const TimingGroup& group)
										 { return !group.flat && group.category == category; });
				if (found == groups.end())
				{
					groups.push_back(TimingGroup{category, 0.0, {}, false});
					found = std::prev(groups.end());
				}
				TimingTable::Entry item = entry;
				item.name = entry.name.substr(colon + 1);
				found->milliseconds += entry.milliseconds;
				found->entries.push_back(std::move(item));
			}
			std::ranges::stable_sort(groups, [](const TimingGroup& a, const TimingGroup& b)
									 { return a.milliseconds > b.milliseconds; });
			return groups;
		}
	} // namespace

	std::string TimingTable::format(std::string_view heading) const
	{
		if (fEntries.empty())
			return {};

		std::string text(heading);
		text += "（合計 ";
		text += formatNumber("%.0f", total());
		text += "ms）:";

		// **「分類:項目」の名前は分類ごとに 1 段下げて並べる。** 区間は 50 を超えるので、
		// 平らに並べると「タグ全体で何 ms か」を読む側が足し算しなければならない。
		// 四捨五入で 0ms になる項目は分類ごとに 1 行へまとめる——回数は多くても時間には
		// 効いていないので、並べても探す邪魔になるだけ。
		for (const TimingGroup& group : groupByCategory(sorted()))
		{
			if (group.flat)
			{
				text += "\n  ";
				text += formatEntry(group.entries.front());
				continue;
			}
			text += "\n  ";
			text += group.category;
			text += " ";
			text += formatNumber("%.0f", group.milliseconds);
			text += "ms";
			std::size_t negligible = 0;
			for (const Entry& entry : group.entries)
			{
				if (entry.milliseconds < 0.5)
				{
					++negligible;
					continue;
				}
				text += "\n    ";
				text += formatEntry(entry);
			}
			if (negligible > 0)
			{
				text += "\n    ほか ";
				text += std::to_string(negligible);
				text += " 項目（いずれも 0.5ms 未満）";
			}
		}

		// **入れ子になった区間は必ず報告する。** 二重計上はもう直せないので、せめて
		// 「この数字は信じてよいか」を読む側へ渡す（core/DrawTiming.h「入れ子の見張り」）。
		if (!fNested.empty())
		{
			text += "\n  ⚠ 入れ子になった区間（時間が外側にも積まれています）: ";
			for (std::size_t i = 0; i < fNested.size(); ++i)
			{
				if (i > 0)
					text += ", ";
				text += fNested[i];
			}
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
