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

	void ProgressReporter::beginPhase(const std::string& label, double share,
									  std::size_t totalSteps)
	{
		fStatus.label = label;
		fStatus.done = 0;
		fStatus.total = totalSteps;

		// クラッシュ診断ログへフェーズの区切りを流す（有効でなければ何もしない）。
		// **トレースの呼び出しを各要素へ撒かないための 1 か所**——解析も描画も
		// フェーズの見出しはここを通るので、ログの最終行がそのまま「どこで落ちたか」に
		// なる（core/Trace.h「誰が書くか」）。
		trace::log(formatProgressText(fStatus));

		onBeginPhase(fStatus, share);
	}

	void ProgressReporter::step(std::size_t count)
	{
		fStatus.done += count;
		// 総数が分かっているなら超えさせない（呼び出し側が数え違えてもバーが暴れない）。
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
