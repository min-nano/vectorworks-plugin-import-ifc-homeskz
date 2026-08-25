//
//	draw/ProgressDialog.cpp
//
//	進捗ダイアログの実装。【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、
//	この翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse
//	ライブラリには入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	使う SDK API は VWFC の進捗ダイアログ（VWFC/Tools/ProgressDlg.h の VWFC::Tools::
//	CProgressDlg）。VectorScript の ProgressDlg* 一式に 1 対 1 で対応する薄いラッパーで、
//	進捗バーとキャンセルをそのまま使える:
//
//	  * Open(title, canCancel)        … ダイアログを開く（ProgressDlgOpen）
//	  * SetTopText / SetMeterText     … 上段の 1 行／メーター横の 1 行（ProgressDlgSetTopMsg /
//	                                    ProgressDlgSetMeter）
//	  * Start(percent, loopCount)     … 「これから loopCount 回まわり、その間にバーを
//	                                    percent% 進める」区間の宣言（ProgressDlgStart）
//	  * DoYield(count)                … 区間を count 回ぶん進め、**VW にイベント処理と
//	                                    再描画の機会を与える**（ProgressDlgYield）
//	  * End()                         … 区間の終了（ProgressDlgEnd）
//	  * HasCancel()                   … キャンセルが押されたか（ProgressDlgHasCancel）
//	  * Close()                       … ダイアログを閉じる（ProgressDlgClose）
//
//	【テキストは即時 / 遅延を使い分ける】SetMeterText などは既定（inbImmediate=false）では
//	文字列を溜めるだけで、実際の描き換えは次の DoYield で起きる。1 件ごとの更新はこの既定で
//	よい（DoYield が直後に来るうえ、毎回描き換えるより速い）。逆に**フェーズの開始時は
//	即時**にする——読み込みフェーズのように DoYield を 1 回も呼ばない区間があり、遅延に
//	しておくと見出しが最後まで出てこない。
//
//	実挙動（ダイアログの見た目・キャンセルの効き・yield の頻度が重すぎないか）は
//	ローカルの VectorWorks で確認する（docs/DEV-NOTES.md M15「ローカル確認」）。
//

#include "PluginPrefix.h"
#include "draw/ProgressDialog.h"
#include "core/Progress.h"

#include "VWFC/Tools/ProgressDlg.h"

#include <memory>
#include <string>

namespace HomeskzIfcImport::draw
{
	// SDK の進捗ダイアログと、その状態（開いているか・Start した区間が開いたままか）。
	struct ProgressDialog::Impl
	{
		VWFC::Tools::CProgressDlg dialog;
		bool open = false;	  // Open 済みで Close していない
		bool segment = false; // Start 済みで End していない

		// 開いている区間を閉じる。Start と End は対で使う（対にしないと、次の Start の
		// 配分が前の区間の残りに乗ってバーの進みが狂う）。
		void endSegment()
		{
			if (!segment)
				return;
			dialog.End();
			segment = false;
		}
	};

	ProgressDialog::ProgressDialog(const std::string& title, const std::string& topText,
								   bool canCancel)
		: fImpl(std::make_unique<Impl>())
	{
		fImpl->dialog.Open(TXString(title.c_str()), canCancel);
		fImpl->open = true;
		// 上段はインポート中ずっと変わらない 1 行（対象ファイル）。ここはまだ DoYield が
		// 来ないので即時で出す。
		if (!topText.empty())
			fImpl->dialog.SetTopText(TXString(topText.c_str()), true /* immediate */);
	}

	ProgressDialog::~ProgressDialog()
	{
		close();
	}

	void ProgressDialog::close()
	{
		if (!fImpl->open)
			return;
		fImpl->endSegment();
		fImpl->dialog.Close();
		fImpl->open = false;
	}

	void ProgressDialog::onBeginPhase(const core::ProgressStatus& status, double share)
	{
		if (!fImpl->open)
			return;

		// 前のフェーズの区間を閉じてから、このフェーズの区間を宣言する。
		fImpl->endSegment();
		// 見出しは**即時**（このフェーズが DoYield を 1 回も呼ばない可能性があるため。
		// 冒頭「テキストは即時 / 遅延を使い分ける」）。
		fImpl->dialog.SetMeterText(TXString(core::formatProgressText(status).c_str()),
								   true /* immediate */);

		// 件数が無いフェーズ（＝進み具合を刻めない読み込み等）はバーを進めない。区間を
		// 開かないので、そのフェーズの step() は yield もしない。
		if (status.total == 0)
			return;
		fImpl->dialog.Start(share, static_cast<Sint32>(status.total));
		fImpl->segment = true;
	}

	void ProgressDialog::onStep(const core::ProgressStatus& status)
	{
		if (!fImpl->open || !fImpl->segment)
			return;

		// 件数の表示は遅延で足りる（直後の DoYield が描き換える）。
		fImpl->dialog.SetMeterText(TXString(core::formatProgressText(status).c_str()));
		// **ここが「フリーズして見える」への効き所**: 1 件ごとに制御を VW へ返し、
		// ダイアログの再描画とキャンセル操作を受け付けさせる。
		fImpl->dialog.DoYield(1);
	}

	bool ProgressDialog::onCancelled()
	{
		if (!fImpl->open)
			return false;
		return fImpl->dialog.HasCancel();
	}
} // namespace HomeskzIfcImport::draw
