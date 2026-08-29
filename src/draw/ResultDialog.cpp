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
//	  * ShowControl(id, visible)                  … コントロールの表示／非表示（＝折り畳み）
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
//	実挙動（折り畳みでダイアログの高さが縮むか・ログ欄の見た目・コピーの可否）は
//	ローカルの VectorWorks で確認する（docs/DEV-NOTES.md「実機確認の作法」）。
//

#include "PluginPrefix.h"
#include "draw/ResultDialog.h"

#include <memory>
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

		// 取り込みの結果ダイアログ。本文（数行の静的テキスト）＋「ログを表示」ボタン＋
		// 折り畳んだログ欄。ボタンは OK 1 つ（キャンセルは出さない——結果を見るだけの
		// ダイアログで「取り消す」ものが無い）。
		class CImportResultDialog : public VWDialog
		{
		public:
			CImportResultDialog(const std::string& title, const std::vector<std::string>& body,
								const std::string& log)
				: fTitle(title.c_str()), fLog(kLogID), fToggle(kToggleID), fLogText(log.c_str()),
				  fHasLog(!log.empty())
			{
				TControlID id = kFirstBodyID;
				for (const std::string& line : body)
					fBody.push_back(BodyLine{std::make_unique<VWStaticTextCtrl>(id++), line});
			}
			~CImportResultDialog() override = default;

			// **実際に出せたか。** レイアウトを組めなかったときは呼び出し側が素のアラートへ
			// 落とす（ResultDialog.h）。
			bool Shown() const
			{
				return fShown;
			}

		protected:
			bool CreateDialogLayout() override
			{
				// キャンセルは空文字＝ボタンを出さない。hasHelp = false でヘルプも出さない。
				if (!this->CreateDialog(fTitle, "OK", "", false))
					return false;

				// 本文は上から 1 行ずつ。空行はコントロールを作らず、**次の行の行間**で表す
				// （空の静的テキストは高さを持たない環境がある）。
				VWControl* previous = nullptr;
				short pendingSpacing = 0;
				for (BodyLine& line : fBody)
				{
					if (line.text.empty())
					{
						pendingSpacing = 1; // 次の行の前に 1 行ぶん空ける
						continue;
					}
					if (!line.control->CreateControl(this, line.text.c_str()))
						return false;
					if (previous == nullptr)
						this->AddFirstGroupControl(line.control.get());
					else
						this->AddBelowControl(previous, line.control.get(), 0, pendingSpacing);
					previous = line.control.get();
					pendingSpacing = 0;
				}
				if (previous == nullptr)
					return false; // 本文が空（呼び出し側の誤り）。素のアラートへ落とす

				// ログが無い（開けなかった）なら、開閉ボタンもログ欄も作らない。
				if (!fHasLog)
					return true;
				if (!fToggle.CreateControl(this, "ログを表示"))
					return false;
				this->AddBelowControl(previous, &fToggle, 0, 1);
				if (!fLog.CreateControl(this, "", kLogWidthChars, kLogHeightLines))
					return false;
				this->AddBelowControl(&fToggle, &fLog);
				return true;
			}

			void OnInitializeContent() override
			{
				VWDialog::OnInitializeContent();
				if (fHasLog)
				{
					// 既定は畳んでおく。ふだん読むのは本文の数行だけで、ログは
					// 「困ったときに開くもの」（M19）。**中身は開いたときに流し込む**
					// （下の OnToggleLog）。
					this->ShowControl(kLogID, false);
				}
				fShown = true;
			}

			// 「ログを表示 / 隠す」。**ダイアログの高さが追随するか**は実機で確かめる
			// （ShowControl はレイアウトへ効くはずだが、SDK ヘッダには書かれていない）。
			void OnToggleLog(TControlID /*controlID*/, VWDialogEventArgs& /*eventArg*/)
			{
				if (!fHasLog)
					return;
				fLogVisible = !fLogVisible;
				if (fLogVisible && !fLogLoaded)
				{
					// **開いたときに読み込む。** 畳んだままなら流し込まない（ログは
					// 数十行とはいえ、要らない仕事はしない）。
					fLog.SetText(fLogText);
					fLogLoaded = true;
				}
				this->ShowControl(kLogID, fLogVisible);
				fToggle.SetControlText(fLogVisible ? "ログを隠す" : "ログを表示");
			}

			DEFINE_EVENT_DISPATH_MAP;

		private:
			// 本文の 1 行と、それを出す静的テキスト（空行は control を使わない）。
			struct BodyLine
			{
				std::unique_ptr<VWStaticTextCtrl> control;
				std::string text;
			};

			TXString fTitle;
			std::vector<BodyLine> fBody;
			VWEditTextCtrl fLog;
			VWPushButtonCtrl fToggle;
			TXString fLogText;
			bool fHasLog = false;
			bool fLogVisible = false;
			bool fLogLoaded = false;
			bool fShown = false;
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

		CImportResultDialog dialog(title, lines, log);
		dialog.RunDialogLayout("");
		// 押されたボタンは見ない（OK しか無い）。見るのは「出せたか」だけ。
		return dialog.Shown();
	}
} // namespace HomeskzIfcImport::draw
