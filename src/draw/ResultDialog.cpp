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
//	  * OnCancelButtonEvent()                     … 「ログを表示」が押された（下記）
//	  * Get/SetDialogPosition(left, top)          … 位置（開き直しても動かさないため）
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
//	【ログは一度開いたら畳まない】レイアウトダイアログの大きさは**作るときに 1 度だけ**
//	決まる。`ShowControl` で後からログ欄を隠しても縮まず、初期状態で隠しておいてもその分の
//	高さは空いたままで、`SetDialogSize` で押し込んでも安定しなかった（いずれも実機で確認。
//	SDK リファレンス Findings「Layout Dialogs」）。畳めるように見せると、そのたびに作り直すことに
//	なって落ち着かない——**開くのは一方通行**とし、そのぶん確実に振る舞わせる。
//
//	【「ログを表示」はキャンセル枠】ボタン行（OK のある行）へコントロールを足す API は
//	SDK に無い（この行は `GS_CreateLayout` が作る）。そこで**キャンセルのボタンに
//	「ログを表示」の名前を付ける**——OK と同じ行に並び（macOS ではその左）、押されたことは
//	`OnCancelButtonEvent` で分かる。押されたら**ログ付きでもう 1 枚開く**。そちらは
//	キャンセルの名前を空にするので、ボタンは消えてログ欄だけが増える。位置は引き継ぐので、
//	その場で開いたように見える。
//
//	（Esc もキャンセル扱いなので、畳んでいる間の Esc は「ログを表示」になる。ログを開いた
//	後はキャンセルのボタンが無く、Esc でそのまま閉じられる。）
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
		// コントロール ID。1 = OK / 2 = キャンセル（＝「ログを表示」）は SDK の予約。
		// ログ欄を固定の番号にしておき、本文の行はその後ろから連番で振る
		// （行数は本文で変わる）。
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

		// 取り込みの結果ダイアログ**1 枚**。本文（数行の静的テキスト）と、ログを開いた枚なら
		// ログ欄。ボタン行は畳んでいる枚が「OK」＋「ログを表示」（＝キャンセル枠）、
		// 開いた枚は「OK」だけ（冒頭「ログは一度開いたら畳まない」）。
		class CImportResultDialog : public VWDialog
		{
		public:
			CImportResultDialog(const std::string& title, const std::vector<std::string>& body,
								const std::string& log, bool showLog,
								const DialogPlacement& placement)
				: fTitle(title.c_str()), fBody(body), fLog(kLogID), fLogText(log.c_str()),
				  fHasLog(!log.empty()), fShowLog(showLog && !log.empty()), fPlacement(placement)
			{
			}
			~CImportResultDialog() override = default;

			// **実際に出せたか。** レイアウトを組めなかったときは呼び出し側が素のアラートへ
			// 落とす（ResultDialog.h）。
			bool Shown() const
			{
				return fShown;
			}

			// 「ログを表示」（キャンセル枠）が押されたか（＝ログ付きで開き直してほしい）。
			bool RevealRequested() const
			{
				return fRevealRequested;
			}

			// 閉じたときの置き場所（作り直す側が引き継ぐ）。
			const DialogPlacement& Placement() const
			{
				return fPlacement;
			}

		protected:
			bool CreateDialogLayout() override
			{
				// **キャンセル枠を「ログを表示」に使う**（冒頭「『ログを表示』はキャンセル枠」）。
				// ログが無い枚・すでに開いた枚は空文字＝ボタンを出さない。
				// hasHelp = false でヘルプも出さない。
				const TXString revealButton = (fHasLog && !fShowLog) ? "ログを表示" : "";
				if (!this->CreateDialog(fTitle, "OK", revealButton, false))
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

				// **畳んだ枚ではログ欄を作らない。** 作って隠すのでは高さが空いたままになる
				// （冒頭「ログは一度開いたら畳まない」）。
				if (!fShowLog)
					return true;
				if (!fLog.CreateControl(this, "", kLogWidthChars, kLogHeightLines))
					return false;
				this->AddBelowControl(previous, &fLog, 0, 1);
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

			// キャンセル枠＝「ログを表示」。畳んだ枚でだけボタンが出ているので、そのときの
			// キャンセルは「ログを見たい」の意味になる（Esc も同じ扱い。冒頭の但し書き）。
			void OnCancelButtonEvent() override
			{
				VWDialog::OnCancelButtonEvent();
				if (!fHasLog || fShowLog)
					return;
				fRevealRequested = true;
				// 開き直す先の位置（いまの場所）を控える。
				const ViewPt position = this->GetDialogPosition();
				fPlacement.known = true;
				fPlacement.left = position.x;
				fPlacement.top = position.y;
			}

			// 個々のコントロールのイベントは受けないが、VWDialog がこの宣言を要求する
			// （Updater の CBuildPickerDialog と同じ）。
			DEFINE_EVENT_DISPATH_MAP;

		private:
			TXString fTitle;
			std::vector<std::string> fBody; // 本文（改行で切った 1 行ずつ）
			std::deque<VWStaticTextCtrl> fLines; // その行を出す静的テキスト（空行の分は作らない）
			VWEditTextCtrl fLog;
			TXString fLogText;
			bool fHasLog = false; // ログの本文があるか（無ければボタンもログ欄も出さない）
			bool fShowLog = false;		   // この 1 枚はログ欄を持つか
			bool fShown = false;		   // 実際に出せたか
			bool fRevealRequested = false; // 「ログを表示」が押されたか
			DialogPlacement fPlacement;	   // 開き直すときに引き継ぐ置き場所
		};

		// EVENT_DISPATCH_MAP_BEGIN は SDK のマクロ。展開に const 化できるローカルが出るが、
		// それはマクロ側のコードでこちらのものではない（Updater の同じ箇所と同じ理由）。
		// 受けるイベントは無い（ボタンはキャンセル枠なので OnCancelButtonEvent が拾う）。
		// NOLINTNEXTLINE(misc-const-correctness)
		EVENT_DISPATCH_MAP_BEGIN(CImportResultDialog);
		EVENT_DISPATCH_MAP_END;
	} // namespace

	bool showImportResult(const std::string& title, const std::string& body, const std::string& log)
	{
		const std::vector<std::string> lines = splitLines(body);
		if (lines.empty())
			return false;

		// 畳んだ枚を出し、「ログを表示」が押されたらログ付きで**1 度だけ**開き直す
		// （冒頭「ログは一度開いたら畳まない」）。2 周目はボタンが無いので、ここは
		// 高々 2 回しか回らない。
		bool showLog = false;
		DialogPlacement placement;
		for (;;)
		{
			CImportResultDialog dialog(title, lines, log, showLog, placement);
			dialog.RunDialogLayout("");
			// 押されたボタンそのものは見ない。見るのは「出せたか」と「ログを見たいか」。
			if (!dialog.Shown())
				return false;
			if (!dialog.RevealRequested())
				return true;
			showLog = true;
			placement = dialog.Placement();
		}
	}

} // namespace HomeskzIfcImport::draw
