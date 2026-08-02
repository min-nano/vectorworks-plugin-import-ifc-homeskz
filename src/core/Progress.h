//
//	core/Progress.h
//
//	インポート中の進捗を伝えるための最小の仕組み。Phase 1（IFC 解析）と Phase 2（VW 描画）の
//	どちらもここで定義する ProgressReporter を通して「いま何をしているか」「何件中の何件か」を
//	報告し、実際の表示は SDK 側の実装（draw/ProgressDialog）が受け持つ。
//
//	【なぜ要るか】インポートは実測でほぼ全時間が**描画**に費やされる（解析は大きな
//	ホームズ君 IFC でも 0.1 秒程度で終わる）。描画は横架材・垂木を 1 本ずつ PIO として
//	作るため数百回の SDK 呼び出しになり、その間 VectorWorks は再描画もイベント処理もせず
//	**フリーズしたように見える**。進捗ダイアログを出して 1 件ごとに yield すれば、進み具合が
//	見えるうえに UI も生き返る。
//
//	【SDK 非依存】core/ は VectorWorks SDK を一切 include しない（CLAUDE.md「依存の向きは
//	厳守する」）。したがって parse/ からも draw/ からも同じ型で報告でき、件数の数え方・表示
//	文言の整形・配分の計算は無 SDK で単体テストできる（tests/CoreProgressTests.cpp）。
//

#pragma once

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::core
{
	// 進捗バー全体（100%）をフェーズへどう配分するか。合計は 100 になる。
	//
	// 描画に 90% を割いているのは実測に基づく: 解析（読み込み＋各要素）は大きなフィクスチャ
	// （エンティティ 4 万・横架材 270 本）でも 100ms 弱で終わり、体感時間はすべて描画側にある。
	// 解析に大きな配分を与えるとバーが一瞬で 3 割まで飛んで以後動かない、という嘘の進捗に
	// なるため、実時間の比に近い配分にしてある。
	constexpr double kLoadShare = 3.0; // IFC ファイルの読み込み（テキスト→STEP グラフ）
	constexpr double kParseShare = 7.0; // 要素ごとの解析（Document の組み立て）
	constexpr double kDrawShare = 90.0; // VectorWorks への描画

	// 解析フェーズの step() 呼び出し回数（parse/BuildDocument が要素ごとに 1 回進める:
	// 横架材・ストーリ・通り芯・床・垂木・野地板）。要素を足したらここも増やす。
	constexpr std::size_t kParseSteps = 6;

	// いま報告されている進捗。label は「横架材を描画しています…」のような見出し、
	// done / total は件数（total = 0 は「総数が分からない／件数を出さない」の意味）。
	struct ProgressStatus
	{
		std::string label;
		std::size_t done = 0;
		std::size_t total = 0;
	};

	// 進捗の表示文字列へ整形する。総数があれば「見出し (12/196)」、無ければ見出しのみ。
	std::string formatProgressText(const ProgressStatus& status);

	// 進捗バー全体に占めるフェーズの配分（%）を、命令数の比で求める。total が 0 なら 0
	// （＝バーを進めないフェーズ）。count が total を超えていても totalShare は超えない。
	double phaseShare(std::size_t count, std::size_t total, double totalShare);

	// 進捗の報告先。**呼び出し側（parse / draw）はこの基底だけを知る。** 件数の勘定と
	// 表示文言の整形はここで済ませ、派生（draw/ProgressDialog）は「見出しが変わった」
	// 「1 件進んだ」「中止されたか」の 3 つのフックだけを実装すればよい。
	//
	// 既定の実装は何もしない（＝進捗を表示しない）ので、進捗の要らない呼び出し
	// （単体テスト・オーバーロードの既定）は NullProgressReporter を渡せばよい。
	class ProgressReporter
	{
	public:
		ProgressReporter() = default;
		virtual ~ProgressReporter() = default;
		ProgressReporter(const ProgressReporter&) = delete;
		ProgressReporter& operator=(const ProgressReporter&) = delete;
		ProgressReporter(ProgressReporter&&) = delete;
		ProgressReporter& operator=(ProgressReporter&&) = delete;

		// フェーズを始める。share はこのフェーズが進捗バー全体に占める割合（%）、
		// totalSteps はこのフェーズで step() を呼ぶ回数（0 なら見出しだけ更新する）。
		// 件数はここで 0 に戻る。
		void beginPhase(const std::string& label, double share, std::size_t totalSteps);

		// 進捗を count 件進める。総数が分かっているときは総数を超えない。
		void step(std::size_t count = 1);

		// ユーザーが中止を押したか。**一度 true になったら以後も true を返す**
		// （SDK 側の問い合わせが押下を一度しか報告しなくても取りこぼさないため）。
		bool cancelled();

		const ProgressStatus& status() const
		{
			return fStatus;
		}

	protected:
		// 派生が実装するフック。既定は「何もしない／中止されていない」。
		virtual void onBeginPhase(const ProgressStatus& /*status*/, double /*share*/) {}
		virtual void onStep(const ProgressStatus& /*status*/) {}
		virtual bool onCancelled()
		{
			return false;
		}

	private:
		ProgressStatus fStatus;
		bool fCancelled = false;
	};

	// 何も表示しない報告先。進捗を出さない呼び出し（テスト・既定のオーバーロード）用。
	class NullProgressReporter final : public ProgressReporter
	{
	};
} // namespace HomeskzIfcImport::core
