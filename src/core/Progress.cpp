//
//	core/Progress.cpp
//
//	進捗報告（core/Progress.h）の実装。件数の勘定・表示文言の整形・配分の計算という
//	**純ロジックだけ**を持ち、実際の表示は派生（draw/ProgressDialog）に委ねる。
//	【SDK 非依存】ここでは VectorWorks SDK を include しない。
//

#include "core/Progress.h"
#include "core/Trace.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

namespace HomeskzIfcImport::core
{
	std::string formatProgressText(const ProgressStatus& status)
	{
		// 総数が分からないフェーズ（読み込み等）は見出しだけ。件数を「0/0」と出しても
		// 情報が無いうえ、止まって見える。
		if (status.total == 0)
			return status.label;

		// 件数は総数で頭打ちにする（step() 側でも抑えているが、表示の一貫性をここでも守る）。
		const std::size_t done = std::min(status.done, status.total);
		return status.label + " (" + std::to_string(done) + "/" + std::to_string(status.total) +
			   ")";
	}

	double phaseShare(std::size_t count, std::size_t total, double totalShare)
	{
		if (total == 0 || count == 0)
			return 0.0;
		if (count >= total)
			return totalShare;
		return totalShare * (static_cast<double>(count) / static_cast<double>(total));
	}

	namespace
	{
		// 描画フェーズの表。**ここが唯一の一覧**で、要素を追加したら DrawPhase に 1 つ、
		// この表に 1 行を追加する（Count と行数が食い違えばテストが失敗する）。
		//
		// weight は**実測の 1 件あたりミリ秒**（dev ビルド・macOS。診断ログのフェーズ見出しの
		// 時刻差 ÷ 件数を、2 つのフィクスチャ——グレー本モデルプラン1【3階】（描画 27 秒）と
		// スキップフロア_サンプル（同 37 秒）——で平均して丸めたもの）。絶対値に意味は無く、
		// 要素どうしの比だけを使う。モデルが変われば多少ずれる——それでも件数比
		// （＝全要素が同じ重さという仮定）よりはるかに実時間に近い。
		//
		// **描画の処理を変えたら測り直す。** 実機テストの診断ログの見出しの時刻から求まる
		// （docs/dev-notes/design/diagnostics.md「進捗バーの重み」）。以前の表（安藤邸 IFC で
		// 測ったもの）は横架材・柱・底盤を 10 倍以上重く見積もっていて、伏図に入る前にバーが
		// 8 割まで進み、残りの 2 割に描画時間の 4 分の 3 を費やしていた。
		struct PhaseCost
		{
			double weight;							  // 1 件あたりの重さ（ms/件）
			std::size_t (*commands)(const Document&); // 命令数
		};

		constexpr std::array<PhaseCost, static_cast<std::size_t>(DrawPhase::Count)> kCosts = {{
			{5.0, [](const Document& d) { return d.stories.size(); }},
			{4.0, [](const Document& d) { return d.grids.size(); }},
			{40.0, [](const Document& d) { return d.walls.size(); }},
			{17.0, [](const Document& d) { return d.wallJoins.size(); }},
			// 底盤は形で重さが 1 桁変わる（31ms/枚 と 297ms/枚）。2 つの中間を採る。
			{80.0, [](const Document& d) { return d.slabs.size(); }},
			{45.0, [](const Document& d) { return d.floors.size(); }},
			{10.0, [](const Document& d) { return d.members.size(); }},
			{6.0, [](const Document& d) { return d.columns.size(); }},
			{10.0, [](const Document& d) { return d.rafters.size(); }},
			{5.0, [](const Document& d) { return d.roofs.size(); }},
			{0.15, [](const Document& d) { return d.anchorBolts.size(); }},
			{0.1, [](const Document& d) { return d.floorPosts.size(); }},
			{0.2, [](const Document& d) { return d.fireBraces.size(); }},
			{0.2, [](const Document& d) { return d.joints.size(); }},
			{0.2, [](const Document& d) { return d.splices.size(); }}, // 仕口と同じ描画方法
			{2.0, [](const Document& d) { return d.columnMarks.size(); }},
			{2.0, [](const Document& d) { return d.shearWalls.size(); }},
			{210.0, [](const Document& d) { return d.sheets.size(); }}, // 1 巡目
			{620.0, [](const Document& d) { return d.sheets.size(); }}, // 2 巡目
			{680.0, [](const Document& d) { return d.sections.size(); }},
		}};
	} // namespace

	double drawWeight(DrawPhase phase)
	{
		const auto index = static_cast<std::size_t>(phase);
		if (index >= kCosts.size())
			return 0.0; // DrawPhase::Count を渡された等（フェーズでないものは重さ 0）
		return kCosts[index].weight;
	}

	double drawWeightedTotal(const Document& document)
	{
		double total = 0.0;
		for (const PhaseCost& cost : kCosts)
			total += static_cast<double>(cost.commands(document)) * cost.weight;
		return total;
	}

	double drawPhaseShare(std::size_t count, DrawPhase phase, double weightedTotal,
						  double totalShare)
	{
		if (weightedTotal <= 0.0 || count == 0)
			return 0.0;
		const double weighted = static_cast<double>(count) * drawWeight(phase);
		if (weighted >= weightedTotal)
			return totalShare; // 1 フェーズしか無いときに端数で超えない
		return totalShare * (weighted / weightedTotal);
	}

	void ProgressReporter::beginPhase(const std::string& label, double share,
									  std::size_t totalSteps)
	{
		fStatus.label = label;
		fStatus.done = 0;
		fStatus.total = totalSteps;

		// クラッシュ診断ログへフェーズの区切りを出力する（有効でなければ何もしない）。
		// **トレースの呼び出しを各要素へ分散させないための 1 か所**——解析も描画も
		// フェーズの見出しはここを通るので、ログの最終行がそのまま「どこで異常終了したか」に
		// なる（core/Trace.h「誰が書くか」）。
		trace::log(formatProgressText(fStatus));

		onBeginPhase(fStatus, share);
	}

	void ProgressReporter::step(std::size_t count)
	{
		fStatus.done += count;
		// 総数が分かっているなら超えさせない（呼び出し側が数え違えてもバーの表示が乱れない）。
		if (fStatus.total != 0)
			fStatus.done = std::min(fStatus.done, fStatus.total);
		onStep(fStatus);
	}

	bool ProgressReporter::cancelled()
	{
		// 一度中止されたら以後は問い合わせずに true を返す（ヘッダの但し書き参照）。
		if (!fCancelled)
			fCancelled = onCancelled();
		return fCancelled;
	}
} // namespace HomeskzIfcImport::core
