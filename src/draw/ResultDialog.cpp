//
//	draw/ResultDialog.cpp
//
//	結果ダイアログの実装。【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、
//	この翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse
//	ライブラリには入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	使う SDK API は VWFC のレイアウトダイアログ（VWFC/VWUI/…）。Updater の
//	CBuildPickerDialog と同じ作法で組む:
//
//	  * CreateDialog(title, ok, cancel, hasHelp)  … 枠を作る（ID 1=OK / 2=キャンセル は予約）
//	  * AddFirstGroupControl / AddBelowControl    … 上から順にコントロールを積む
//	  * OnInitializeContent()                     … コントロールができた後の中身の流し込み
//	  * SetDialogClose(bCloseWithOK)              … ハンドラの中からダイアログを閉じる
//	  * Get/SetDialogPosition(left, top)          … 位置（開き直しても動かさないため）
//	  * EVENT_DISPATCH_MAP + ADD_DISPATCH_EVENT   … ボタンのクリックを受ける
//
//	【本文は 1 行 1 コントロール】VWStaticTextCtrl は 1 行を出すためのもので、埋め込んだ
//	改行がそのまま行になる保証が無い。本文（parse/Summary が組み立てた数行）を確実に
//	そのままの形で見せるため、**改行で切って 1 行ずつ静的テキストにする**。空行は
//	コントロールを作らず、次の行の行間（AddBelowControl の lineSpacing）で表す。
//
//	【ログ欄は VWEditTextCtrl】複数行の編集欄なので**スクロールし、選択してコピーできる**
//	（静的テキストではコピーできず、報告に貼れない）。編集はできてしまうが、閉じるときに
//	捨てるだけなので害は無い。
//
//	【折り畳みはダイアログを作り直して行う】レイアウトダイアログの大きさは**作るときに
//	1 度だけ決まる**。`ShowControl` で後からログ欄を隠しても縮まず、初期状態で隠しておいても
//	その分の高さは空いたままで、`SetDialogSize` で押し込んでも安定しなかった（いずれも実機で
//	確認。docs/DEV-NOTES.md「結果ダイアログ」）。
//
//	そこで**状態ごとにダイアログを作り直す**: 畳んだダイアログは**ログ欄そのものを作らない**
//	ので、VW のレイアウトが計算する大きさが最初から正しい。開閉ボタンは自分の状態を
//	記録して `SetDialogClose` でダイアログを閉じ、呼び出し側（showImportResult）が反対の
//	状態でもう 1 枚開く。位置は引き継ぐので、その場で開き直したように見える。
//

#include "PluginPrefix.h"
#include "draw/ResultDialog.h"

#include <deque>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// コントロール ID。1 = OK / 2 = キャンセルは SDK の予約。ログ欄と開閉ボタンを
		// 固定の番号にしておき、本文の行はその後ろから連番で振る（行数は本文で変わる）。
		constexpr TControlID kToggleID = 3;
		constexpr TControlID kLogID = 4;
		constexpr TControlID kFirstBodyID = 10;

		// ログ欄の大きさ（標準文字幅・行数）。**用紙のように広げない**——ふだんは畳んで
		// あるものなので、開いたときに画面へ収まる範囲で、割り付けの 1 行（長い）が
		// 折り返さずに読める幅を採る。
		constexpr short kLogWidthChars = 92;
		constexpr short kLogHeightLines = 18;

		// 本文を改行で切る（末尾の空行は落とす）。
		std::vector<std::string> splitLines(const std::string& text)
		{
			std::vector<std::string> lines;
			std::string::size_type start = 0;
			while (start <= text.size())
			{
				const std::string::size_type end = text.find('\n', start);
				if (end == std::string::npos)
				{
					lines.push_back(text.substr(start));
					break;
				}
				lines.push_back(text.substr(start, end - start));
				start = end + 1;
			}
			while (!lines.empty() && lines.back().empty())
				lines.pop_back();
			return lines;
		}

		// ダイアログの置き場所（作り直すときに引き継ぐ）。known=false なら VW に任せる。
		struct DialogPlacement
		{
			bool known = false;
			ViewCoord left = 0;
			ViewCoord top = 0;
		};

		// 取り込みの結果ダイアログ**1 枚**。本文（数行の静的テキスト）＋「ログを表示 /
		// 隠す」ボタン＋（開いた状態なら）ログ欄。ボタンは OK 1 つ（キャンセルは出さない
		// ——結果を見るだけのダイアログで「取り消す」ものが無い）。開閉はこの 1 枚の中では
		// 行わず、閉じて反対の状態でもう 1 枚開く（冒頭「折り畳みは…」）。
		class CImportResultDialog : public VWDialog
		{
		public:
			CImportResultDialog(const std::string& title, const std::vector<std::string>& body,
								const std::string& log, bool showLog,
								const DialogPlacement& placement)
				: fTitle(title.c_str()), fBody(body), fLog(kLogID), fToggle(kToggleID),
				  fLogText(log.c_str()), fHasLog(!log.empty()), fShowLog(showLog && !log.empty()),
				  fPlacement(placement)
			{
			}
			~CImportResultDialog() override = default;

			// **実際に出せたか。** レイアウトを組めなかったときは呼び出し側が素のアラートへ
			// 落とす（ResultDialog.h）。
			bool Shown() const
			{
				return fShown;
			}

			// 開閉ボタンが押されたか（＝反対の状態で開き直してほしい）。
			bool ToggleRequested() const
			{
				return fToggleRequested;
			}

			// 閉じたときの置き場所（作り直す側が引き継ぐ）。
			const DialogPlacement& Placement() const
			{
				return fPlacement;
			}

		protected:
			bool CreateDialogLayout() override
			{
				// キャンセルは空文字＝ボタンを出さない。hasHelp = false でヘルプも出さない。
				if (!this->CreateDialog(fTitle, "OK", "", false))
					return false;

				// 本文は上から 1 行ずつ。空行はコントロールを作らず、**次の行の行間**で表す
				// （空の静的テキストは高さを持たない環境がある）。
				TControlID id = kFirstBodyID;
				VWControl* previous = nullptr;
				short pendingSpacing = 0;
				for (const std::string& line : fBody)
				{
					if (line.empty())
					{
						pendingSpacing = 1; // 次の行の前に 1 行ぶん空ける
						continue;
					}
					// **deque に直接作る。** 行数は本文で変わるので器が要るが、vector だと
					// 追加のたびに既存の要素が動いてしまい（ダイアログは生存中ずっと
					// コントロールのアドレスを持つ）、unique_ptr で逃がすと今度は静的解析が
					// 「漏れるかもしれない」と誤検出する。deque は追加しても既存の要素を
					// 動かさないので、どちらの問題も出ない。
					VWStaticTextCtrl& control = fLines.emplace_back(id++);
					if (!control.CreateControl(this, line.c_str()))
						return false;
					if (previous == nullptr)
						this->AddFirstGroupControl(&control);
					else
						this->AddBelowControl(previous, &control, 0, pendingSpacing);
					previous = &control;
					pendingSpacing = 0;
				}
				if (previous == nullptr)
					return false; // 本文が空（呼び出し側の誤り）。素のアラートへ落とす

				// ログが無い（開けなかった）なら、開閉ボタンもログ欄も作らない。
				if (!fHasLog)
					return true;
				if (!fToggle.CreateControl(this, fShowLog ? "ログを隠す" : "ログを表示"))
					return false;
				this->AddBelowControl(previous, &fToggle, 0, 1);
				// **畳んだ状態ではログ欄を作らない。** 作って隠すのでは高さが空いたままに
				// なる（冒頭「折り畳みはダイアログを作り直して行う」）。
				if (!fShowLog)
					return true;
				if (!fLog.CreateControl(this, "", kLogWidthChars, kLogHeightLines))
					return false;
				this->AddBelowControl(&fToggle, &fLog);
				return true;
			}

			void OnInitializeContent() override
			{
				VWDialog::OnInitializeContent();
				// ログ欄があるのは開いた状態のときだけ。中身はここで流し込む
				// （畳んだ状態では欄そのものが無いので、何もしない）。
				if (fShowLog)
					fLog.SetText(fLogText);
				// 開き直しのときは元の場所へ。**その場で開き直したように見せる**ため
				// （引き継がないと画面中央へ飛ぶ）。
				if (fPlacement.known)
					this->SetDialogPosition(fPlacement.left, fPlacement.top);
				fShown = true;
			}

			// DDX（コントロールと変数の結び付け）は使わない——このダイアログは値を集めず、
			// 結果を見せるだけ。**それでも空実装が要る**（VWDialog の純粋仮想。SDK 自身の
			// CStandardInfoDlg も同じく空で潰している）。
			void OnDDXInitialize() override {}

			// 「ログを表示 / 隠す」。**ここでは開閉しない**——状態を控えてダイアログを閉じ、
			// 呼び出し側が反対の状態で開き直す（冒頭「折り畳みはダイアログを作り直して行う」）。
			void OnToggleLog(TControlID /*controlID*/, VWDialogEventArgs& /*eventArg*/)
			{
				if (!fHasLog)
					return;
				fToggleRequested = true;
				// 開き直す先の位置（いまの場所）を控える。
				const ViewPt position = this->GetDialogPosition();
				fPlacement.known = true;
				fPlacement.left = position.x;
				fPlacement.top = position.y;
				// OK として閉じる（押されたボタンは呼び出し側が見ないので、どちらでもよい）。
				this->SetDialogClose(true);
			}

			DEFINE_EVENT_DISPATH_MAP;

		private:
			TXString fTitle;
			std::vector<std::string> fBody; // 本文（改行で切った 1 行ずつ）
			std::deque<VWStaticTextCtrl> fLines; // その行を出す静的テキスト（空行の分は作らない）
			VWEditTextCtrl fLog;
			VWPushButtonCtrl fToggle;
			TXString fLogText;
			bool fHasLog = false; // ログの本文があるか（無ければ開閉ボタンも出さない）
			bool fShowLog = false;		   // この 1 枚はログ欄を持つか
			bool fShown = false;		   // 実際に出せたか
			bool fToggleRequested = false; // 開閉ボタンが押されたか
			DialogPlacement fPlacement;	   // 開き直すときに引き継ぐ置き場所
		};

		// EVENT_DISPATCH_MAP_BEGIN は SDK のマクロ。展開に const 化できるローカルが出るが、
		// それはマクロ側のコードでこちらのものではない（Updater の同じ箇所と同じ理由）。
		// NOLINTNEXTLINE(misc-const-correctness)
		EVENT_DISPATCH_MAP_BEGIN(CImportResultDialog);
		ADD_DISPATCH_EVENT(kToggleID, OnToggleLog);
		EVENT_DISPATCH_MAP_END;
	} // namespace

	bool showImportResult(const std::string& title, const std::string& body, const std::string& log)
	{
		const std::vector<std::string> lines = splitLines(body);
		if (lines.empty())
			return false;

		// **開閉のたびに 1 枚ずつ開き直す。** 畳んだ枚はログ欄を持たないので、VW が
		// 計算する大きさが最初から正しい（冒頭「折り畳みはダイアログを作り直して行う」）。
		// 回るのは開閉ボタンが押されたときだけ——ユーザーの操作 1 回につき 1 周なので、
		// 勝手に回り続けることはない。
		bool showLog = false;
		DialogPlacement placement;
		for (;;)
		{
			CImportResultDialog dialog(title, lines, log, showLog, placement);
			dialog.RunDialogLayout("");
			// 押されたボタンは見ない（OK しか無い）。見るのは「出せたか」と「開閉か」。
			if (!dialog.Shown())
				return false;
			if (!dialog.ToggleRequested())
				return true;
			showLog = !showLog;
			placement = dialog.Placement();
		}
	}
} // namespace HomeskzIfcImport::draw
