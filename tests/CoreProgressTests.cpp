//
//	CoreProgressTests.cpp
//
//	進捗報告（src/core/Progress）の単体テスト。VectorWorks SDK も STEP も使わない
//	純ロジック——表示文言の整形・進捗バーの配分・件数の勘定・中止の保持——なので、
//	無 SDK のテストハーネス（TestFramework.h）だけで完結する（CLAUDE.md「テスト方針」）。
//
//	実際の表示（進捗ダイアログの見た目・yield の効き）は SDK 側（draw/ProgressDialog）に
//	あり、ローカルの VectorWorks でしか確認できない。ここで担保するのは「呼び出し側が
//	報告した内容が、そのままフックへ正しい形で届くこと」まで。
//

#include "TestFramework.h"

#include "core/Progress.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using HomeskzIfcImport::core::Document;
using HomeskzIfcImport::core::DrawPhase;
using HomeskzIfcImport::core::drawPhaseShare;
using HomeskzIfcImport::core::drawWeight;
using HomeskzIfcImport::core::drawWeightedTotal;
using HomeskzIfcImport::core::formatProgressText;
using HomeskzIfcImport::core::NullProgressReporter;
using HomeskzIfcImport::core::phaseShare;
using HomeskzIfcImport::core::ProgressReporter;
using HomeskzIfcImport::core::ProgressStatus;

namespace
{
	// フックの呼ばれ方を記録するだけの報告先（draw/ProgressDialog の代役）。
	class RecordingReporter final : public ProgressReporter
	{
	public:
		struct Event
		{
			std::string text; // formatProgressText の結果（＝実際に表示される文字列）
			double share = 0.0;
			bool isBegin = false;
		};

		std::vector<Event> events;
		bool cancelNow = false; // これを true にすると以後 onCancelled が true を返す
		std::size_t cancelQueries = 0;

	protected:
		void onBeginPhase(const ProgressStatus& status, double share) override
		{
			events.push_back(Event{formatProgressText(status), share, true});
		}

		void onStep(const ProgressStatus& status) override
		{
			events.push_back(Event{formatProgressText(status), 0.0, false});
		}

		bool onCancelled() override
		{
			++cancelQueries;
			return cancelNow;
		}
	};

	bool nearly(double a, double b)
	{
		return std::abs(a - b) <= 1e-9;
	}
} // namespace

// --- 表示文言の整形 --------------------------------------------------------

TEST(format_shows_count_when_total_known)
{
	CHECK_EQ(formatProgressText(ProgressStatus{"横架材を描画しています…", 12, 196}),
			 "横架材を描画しています… (12/196)");
}

TEST(format_omits_count_when_total_unknown)
{
	// 総数 0（＝進み具合を刻めないフェーズ）は見出しだけ。「0/0」を出しても情報が無い。
	CHECK_EQ(formatProgressText(ProgressStatus{"IFC ファイルを読み込んでいます…", 0, 0}),
			 "IFC ファイルを読み込んでいます…");
	CHECK_EQ(formatProgressText(ProgressStatus{"読み込み中", 5, 0}), "読み込み中");
}

TEST(format_clamps_count_to_total)
{
	// 数え違えても「200/196」のような表示にはしない。
	CHECK_EQ(formatProgressText(ProgressStatus{"垂木", 200, 196}), "垂木 (196/196)");
}

// --- 進捗バーの配分 --------------------------------------------------------

TEST(phase_share_is_proportional_to_command_count)
{
	CHECK(nearly(phaseShare(50, 100, 90.0), 45.0));
	CHECK(nearly(phaseShare(1, 4, 100.0), 25.0));
}

TEST(phase_share_handles_empty_and_full_phases)
{
	// 命令が無いフェーズはバーを進めない。総数 0（＝命令が 1 つも無い Document）も同じ。
	CHECK(nearly(phaseShare(0, 100, 90.0), 0.0));
	CHECK(nearly(phaseShare(0, 0, 90.0), 0.0));
	// 全命令が 1 フェーズに属するなら、そのフェーズが配分を丸ごと取る。
	CHECK(nearly(phaseShare(7, 7, 90.0), 90.0));
	CHECK(nearly(phaseShare(9, 7, 90.0), 90.0));
}

TEST(phase_shares_sum_to_the_draw_share)
{
	// 要素ごとの配分を足すと描画フェーズ全体の配分になる（バーが 100% で終わる根拠）。
	const std::size_t total = 4 + 22 + 3 + 196 + 106 + 9;
	double sum = 0.0;
	for (std::size_t count : {std::size_t{4}, std::size_t{22}, std::size_t{3}, std::size_t{196},
							  std::size_t{106}, std::size_t{9}})
		sum += phaseShare(count, total, HomeskzIfcImport::core::kDrawShare);
	CHECK(std::abs(sum - HomeskzIfcImport::core::kDrawShare) < 1e-9);
}

TEST(phase_shares_of_all_phases_sum_to_one_hundred)
{
	CHECK(nearly(HomeskzIfcImport::core::kLoadShare + HomeskzIfcImport::core::kParseShare +
					 HomeskzIfcImport::core::kDrawShare,
				 100.0));
}

// --- 件数の勘定 ------------------------------------------------------------

TEST(reporter_counts_steps_and_reports_them)
{
	RecordingReporter reporter;
	reporter.beginPhase("通り芯を描画しています…", 12.5, 3);
	reporter.step();
	reporter.step();
	reporter.step();

	CHECK_EQ(reporter.events.size(), std::size_t{4});
	CHECK(reporter.events[0].isBegin);
	CHECK(nearly(reporter.events[0].share, 12.5));
	CHECK_EQ(reporter.events[0].text, "通り芯を描画しています… (0/3)");
	CHECK_EQ(reporter.events[1].text, "通り芯を描画しています… (1/3)");
	CHECK_EQ(reporter.events[3].text, "通り芯を描画しています… (3/3)");
	CHECK_EQ(reporter.status().done, std::size_t{3});
}

TEST(reporter_resets_the_count_on_a_new_phase)
{
	RecordingReporter reporter;
	reporter.beginPhase("床", 10.0, 2);
	reporter.step();
	reporter.beginPhase("屋根", 10.0, 5);

	CHECK_EQ(reporter.status().done, std::size_t{0});
	CHECK_EQ(reporter.status().total, std::size_t{5});
	CHECK_EQ(reporter.status().label, "屋根");
}

TEST(reporter_never_counts_past_the_total)
{
	RecordingReporter reporter;
	reporter.beginPhase("垂木", 10.0, 2);
	reporter.step(5);
	CHECK_EQ(reporter.status().done, std::size_t{2});
}

TEST(reporter_counts_freely_when_the_total_is_unknown)
{
	// 総数 0 のフェーズでも件数は数える（表示には出ないが、進んだこと自体は伝わる）。
	RecordingReporter reporter;
	reporter.beginPhase("読み込み", 3.0, 0);
	reporter.step();
	reporter.step();
	CHECK_EQ(reporter.status().done, std::size_t{2});
}

// --- 中止 ------------------------------------------------------------------

TEST(reporter_latches_cancellation)
{
	RecordingReporter reporter;
	CHECK(!reporter.cancelled());

	reporter.cancelNow = true;
	CHECK(reporter.cancelled());

	// 一度中止されたら、SDK 側が「押されていない」に戻しても true のまま
	// （押下を一度しか報告しない実装で取りこぼさないため）。問い合わせも増えない。
	reporter.cancelNow = false;
	const std::size_t queries = reporter.cancelQueries;
	CHECK(reporter.cancelled());
	CHECK_EQ(reporter.cancelQueries, queries);
}

// --- 既定（進捗を表示しない呼び出し） --------------------------------------

TEST(null_reporter_is_inert)
{
	// 既定のオーバーロード（buildDocument / executeDocument）が渡すもの。何も起きず、
	// 中止も返さない＝進捗の有無で描画の振る舞いが変わらないことの担保。
	NullProgressReporter reporter;
	reporter.beginPhase("何か", 50.0, 10);
	reporter.step();
	CHECK(!reporter.cancelled());
	CHECK_EQ(reporter.status().done, std::size_t{1});
}

// ---------------------------------------------------------------------------
// 描画フェーズの重み付き配分（実測に基づく按分）
// ---------------------------------------------------------------------------

TEST(draw_weights_are_defined_for_every_phase)
{
	// **表の網羅性の番人**。DrawPhase を足したのに重みの表へ書き忘れると、その要素の重さが
	// 0 になってバーが進まなくなる（気付きにくい）。全フェーズが正の重さを持つことを固定する。
	for (std::size_t i = 0; i < static_cast<std::size_t>(DrawPhase::Count); ++i)
		CHECK(drawWeight(static_cast<DrawPhase>(i)) > 0.0);
	// 番兵そのものは重さを持たない（範囲外は 0）。
	CHECK_EQ(drawWeight(DrawPhase::Count), 0.0);
}

TEST(draw_weighted_total_sums_count_times_weight)
{
	Document document;
	document.members.resize(2);
	document.sections.resize(3);

	const double expected =
		2.0 * drawWeight(DrawPhase::Members) + 3.0 * drawWeight(DrawPhase::Sections);
	CHECK(std::fabs(drawWeightedTotal(document) - expected) < 1e-9);
	// 空の命令セットは 0（配分の分母が 0 ＝ バーを進めないフェーズだけになる）。
	CHECK_EQ(drawWeightedTotal(Document{}), 0.0);
}

TEST(draw_phase_share_follows_time_not_command_count)
{
	// **これが直したかったこと。** 実測モデルに近い内訳（仕口 284 件は 0.03 秒、
	// 軸組図 33 枚は 17 秒）で、件数比なら仕口が軸組図の 8 倍以上バーを進めてしまう。
	// 重み付きなら逆転し、時間を食う軸組図のほうが大きく進む。
	Document document;
	document.joints.resize(284);
	document.sections.resize(33);
	const double total = drawWeightedTotal(document);

	const double joints = drawPhaseShare(284, DrawPhase::Joints, total, 100.0);
	const double sections = drawPhaseShare(33, DrawPhase::Sections, total, 100.0);

	CHECK(sections > joints * 100.0); // 桁で違う（実測では 0.03 秒 対 17 秒）
	CHECK(std::fabs(joints + sections - 100.0) < 1e-9); // 取りこぼしなく 100% を配る
}

TEST(draw_phase_share_handles_empty_and_single_phase)
{
	// 描く物が無ければ 0（0 除算しない）。
	CHECK_EQ(drawPhaseShare(0, DrawPhase::Members, 0.0, 97.0), 0.0);
	CHECK_EQ(drawPhaseShare(5, DrawPhase::Members, 0.0, 97.0), 0.0);

	// 1 フェーズしか無ければ端数で超えず、ぴったり全部を受け取る。
	Document document;
	document.members.resize(7);
	const double total = drawWeightedTotal(document);
	CHECK_EQ(drawPhaseShare(7, DrawPhase::Members, total, 97.0), 97.0);
}

TEST_MAIN();
