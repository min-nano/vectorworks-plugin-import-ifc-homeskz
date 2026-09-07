//
//	draw/Feedback.cpp
//
//	実機フィードバックの往復の実装（意図と 1 周の形は draw/Feedback.h 参照）。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）と VWFC のダイアログを include する。
//
//	使う SDK API はダイアログ 2 種（draw/ResultDialog と同じ作法。VWFC/VWUI/…）だけで、
//	**ネットワークには一切触れない**——
//	投稿もビルドの取得も同梱スクリプトが行い、こちらはその機械可読な出力を読む
//	（自動アップデートと同じ分担。src/Updater.h）。
//
//	【文字列の受け渡し】スクリプトの出力は `UpdaterParse` の純粋な関数で解く
//	（`ValueOf`）。**同じ解き方を 2 つ持たない**
//	ため、殻の自動アップデートが使っているものをそのまま使う
//	（CLAUDE.md「重複を作らない置き場所」）。
//
//	【待たないし、入れもしない】**新しいビルドをここで待ってはいけない。** 待つあいだ
//	モーダルのダイアログが Vectorworks を止め、その周の絵が見られなくなる——絵を見るために
//	回している往復で、それでは本末転倒である（実機 round 3 で判明。docs/DEV-NOTES.md M23）。
//	入れるのも殻の仕事で（src/UpdaterFlow.cpp）、本体のコードがスタックに載っている間は
//	本体を降ろせない以上どのみち殻へ返ってからにしかできない（src/PayloadSession.h）。
//	ここがするのは「投稿して、**次の取り込みでは尋ねずに入れてよい**と伝えて戻る」だけ。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "draw/Feedback.h"

#include "UpdaterParse.h"
#include "core/Document.h"
#include "core/FeedbackSession.h"
#include "core/ImportOptions.h"
#include "core/Trace.h"
#include "draw/DrawUtil.h"
#include "draw/HostServices.h"
#include "parse/Feedback.h"
#include "parse/Summary.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using namespace HomeskzIfcImport::UpdaterParse;

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 同梱スクリプトの名前（拡張子は殻が付ける。src/PayloadAbi.h）。
		constexpr const char* kFeedbackScript = "vw-feedback";

		// ダイアログのコントロール ID（1 = OK / 2 = キャンセルは SDK の予約）。
		// kNoteLabelID / kNoteID はトークンの貼り付けダイアログが使う（**所見の欄は
		// フィードバックのダイアログには無い**——下記「所見は別プロセスで訊く」）。
		constexpr TControlID kNoteLabelID = 4;
		constexpr TControlID kNoteID = 5;
		constexpr TControlID kPrLabelID = 6;
		constexpr TControlID kPrID = 7;
		constexpr TControlID kAnonID = 9;
		constexpr TControlID kHintNoteID = 10;
		constexpr TControlID kHintLoopID = 11;
		constexpr TControlID kFirstBodyID = 20;

		// トークンの貼り付け欄の幅（標準文字数）と、PR 番号の欄の幅。
		constexpr short kNoteWidthChars = 72;
		constexpr short kPrWidthChars = 10;

		// -------------------------------------------------------------------
		// 殻から借りた道具（draw/HostServices）。
		// -------------------------------------------------------------------

		bool RunScript(const char* baseName, const std::vector<std::string>& args, std::string& out)
		{
			out.clear();
			const HostServices& host = hostServices();
			if (!host.canRunScripts())
				return false;
			return host.runScript(baseName, args, out);
		}

		// -------------------------------------------------------------------
		// 一時ファイル（本文とトークンの受け渡し）。
		// -------------------------------------------------------------------

		// 一時ディレクトリに書き出して、そのパスを返す（書けなければ空）。
		// **本文を引数に乗せない**ため（コマンドラインは長さに限りがあり、プロセス一覧
		// からも見える）。tag は名前を分けるためのもの。
		std::string WriteTempFile(const std::string& tag, const std::string& contents,
								  bool ownerOnly)
		{
			std::error_code ec;
			const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
			if (ec)
				return "";
			const std::filesystem::path path =
				dir / ("homeskz-feedback-" + tag + "-" +
					   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

			{
				std::ofstream out(path, std::ios::binary | std::ios::trunc);
				if (!out)
					return "";
				out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
				if (!out.good())
					return "";
			}
			if (ownerOnly)
			{
				// **トークンを渡すファイルは本人しか読めなくする。** 一時ディレクトリは
				// 共有なので、既定の許可のまま置くと他のユーザーに読まれうる。
				std::filesystem::permissions(
					path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
					std::filesystem::perm_options::replace, ec);
			}
			return path.string();
		}

		void RemoveTempFile(const std::string& path)
		{
			if (path.empty())
				return;
			std::error_code ec;
			std::filesystem::remove(std::filesystem::path(path), ec);
		}

		// -------------------------------------------------------------------
		// トークンを 1 度だけ受け取るダイアログ。
		// -------------------------------------------------------------------

		class CTokenDialog : public VWDialog
		{
		public:
			CTokenDialog() : fLabel(kNoteLabelID), fToken(kNoteID) {}
			~CTokenDialog() override = default;

			const TXString& Token() const
			{
				return fTokenText;
			}

		protected:
			bool CreateDialogLayout() override
			{
				if (!this->CreateDialog("GitHub のトークン", "保存", "やめる", false))
					return false;
				if (!fLabel.CreateControl(
						this, "PR へ投稿するためのトークンを 1 度だけ登録します。\n"
							  "GitHub の Fine-grained token（対象リポジトリの Pull "
							  "requests: Read and write）を貼り付けてください。\n"
							  "保存先は macOS のキーチェーン / Windows の暗号化ファイルで、"
							  "図面にもログにも残りません。"))
					return false;
				if (!fToken.CreateControl(this, "", kNoteWidthChars, 1))
					return false;
				this->AddFirstGroupControl(&fLabel);
				this->AddBelowControl(&fLabel, &fToken);
				return true;
			}

			void OnDDXInitialize() override
			{
				this->AddDDX_EditText(kNoteID, &fTokenText);
			}

			DEFINE_EVENT_DISPATH_MAP;

		private:
			VWStaticTextCtrl fLabel;
			VWEditTextCtrl fToken;
			TXString fTokenText;
		};

		// NOLINTNEXTLINE(misc-const-correctness)
		EVENT_DISPATCH_MAP_BEGIN(CTokenDialog);
		EVENT_DISPATCH_MAP_END;

		// **もう投稿できる状態か**（尋ねない・何も出さない）。「送らない」を選んだ周にも
		// 所見だけは訊くが、そのために**トークンの貼り付けダイアログを出すのは筋が違う**
		// ——送らないと決めた人の前に登録を求めるモーダルが出るのでは、押しづらいだけの
		// ボタンになる。登録済みのときだけ静かに訊く。
		bool HaveToken()
		{
			std::string out;
			return RunScript(kFeedbackScript, {"token-status"}, out) && ValueOf(out, "ok") == "yes";
		}

		// **投稿できる状態にする。** トークンが無ければ 1 度だけ尋ねて保存する。
		// 使えるようになったら true。理由は note へ（呼び出し側が見せる）。
		bool EnsureToken(std::string& note)
		{
			std::string out;
			if (!RunScript(kFeedbackScript, {"token-status"}, out))
			{
				note = "フィードバック用のスクリプトを起動できませんでした。";
				return false;
			}
			if (ValueOf(out, "ok") == "yes")
				return true;

			CTokenDialog dialog;
			if (dialog.RunDialogLayout("") != VWFC::VWUI::kDialogButton_Ok)
			{
				note = "トークンの登録をやめました。";
				return false;
			}
			const std::string token = static_cast<const char*>(dialog.Token());
			if (token.empty())
			{
				note = "トークンが空でした。";
				return false;
			}

			// **引数に乗せずファイル経由で渡す**（プロセス一覧から秘密が見えないように）。
			// 読んだスクリプトがその場で消す約束で、こちらも念のため消す。
			const std::string file = WriteTempFile("token", token, /*ownerOnly*/ true);
			if (file.empty())
			{
				note = "トークンを一時ファイルへ書けませんでした。";
				return false;
			}
			const bool ran = RunScript(kFeedbackScript, {"login", file}, out);
			RemoveTempFile(file);
			if (!ran || Trim(out) != "ok")
			{
				const std::string reason = ValueOf(out, "error");
				note = reason.empty() ? "トークンを保存できませんでした。" : reason;
				return false;
			}
			return true;
		}

		// -------------------------------------------------------------------
		// フィードバックのダイアログ（結果の本文＋宛先＋伏せるかどうか）。
		// -------------------------------------------------------------------

		class CFeedbackDialog : public VWDialog
		{
		public:
			CFeedbackDialog(const std::string& title, const std::vector<std::string>& body,
							const core::FeedbackSession& session)
				: fTitle(title.c_str()), fBody(body), fPrLabel(kPrLabelID), fPr(kPrID),
				  fAnon(kAnonID), fHintNote(kHintNoteID), fHintLoop(kHintLoopID),
				  fPrText(std::to_string(session.pullRequest).c_str()),
				  fAnonymize(session.anonymize)
			{
			}
			~CFeedbackDialog() override = default;

			bool Shown() const
			{
				return fShown;
			}
			std::string PullRequest() const
			{
				return static_cast<const char*>(fPrText);
			}
			bool Anonymize() const
			{
				return fAnonymize;
			}

		protected:
			bool CreateDialogLayout() override
			{
				// **ボタンは「何を」送るのかを名指しする。** 所見のダイアログにも「送る」が
				// あるので、ただの「送る／送らない」では**どちらの送信の話か分からない**
				// ——実際にそう読めなかった、という指摘を実機から受けている
				// （docs/DEV-NOTES.md M23「ボタンが何を指しているのか分からなかった」）。
				// **どちらを押しても往復は終わらない**ことも、下の案内で言い切る。
				if (!this->CreateDialog(fTitle, "結果を送る", "結果を送らない", false))
					return false;

				// 結果の本文は 1 行 1 コントロール（draw/ResultDialog と同じ理由）。
				TControlID id = kFirstBodyID;
				VWControl* previous = nullptr;
				short pendingSpacing = 0;
				for (const std::string& line : fBody)
				{
					if (line.empty())
					{
						pendingSpacing = 1;
						continue;
					}
					// deque に直接作る（vector だと既存要素が動く。draw/ResultDialog 参照）。
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
					return false;

				// **所見の欄はここに無い。** 絵を見てから書くには Vectorworks を操作でき
				// なければならず、このダイアログはモーダルでそれを塞ぐ。投稿のあと、別
				// プロセスのダイアログで訊く（下記「所見は別プロセスで訊く」）。
				if (!fPrLabel.CreateControl(this, "送信先の PR 番号:"))
					return false;
				this->AddBelowControl(previous, &fPrLabel, 0, 1);
				if (!fPr.CreateControl(this, "", kPrWidthChars, 1))
					return false;
				this->AddRightControl(&fPrLabel, &fPr);

				if (!fAnon.CreateControl(this, "ファイル名とユーザー名を伏せて投稿する"))
					return false;
				this->AddBelowControl(&fPrLabel, &fAnon, 0, 1);

				// **押したあと何が起きるかを、押す前に書いておく。** ここを書かないと
				// 「結果を送らない＝往復が終わる」と読まれる（実機の指摘）。
				if (!fHintNote.CreateControl(
						this, "※ どちらを押しても、このあと所見のダイアログが出ます。"))
					return false;
				this->AddBelowControl(&fAnon, &fHintNote, 0, 1);
				if (!fHintLoop.CreateControl(this, "※ 往復が終わるのは、次の取り込みの確認で"
												   "「往復を終える」を押したときだけです。"))
					return false;
				this->AddBelowControl(&fHintNote, &fHintLoop);
				return true;
			}

			void OnInitializeContent() override
			{
				VWDialog::OnInitializeContent();
				// **初期値は自分でも入れる。** DDX が流し込む前提には寄りかからない
				// （draw/SettingsDialog も同じく SetState を明示している）——ここが空だと
				// 「前回どおりでよい」ときに毎回打ち直すことになる。
				fPr.SetText(fPrText);
				fAnon.SetState(fAnonymize);
				fShown = true;
			}

			void OnDDXInitialize() override
			{
				this->AddDDX_EditText(kPrID, &fPrText);
				this->AddDDX_CheckButton(kAnonID, &fAnonymize);
			}

			DEFINE_EVENT_DISPATH_MAP;

		private:
			TXString fTitle;
			std::vector<std::string> fBody;
			std::deque<VWStaticTextCtrl> fLines;
			VWStaticTextCtrl fPrLabel;
			VWEditTextCtrl fPr;
			VWCheckButtonCtrl fAnon;
			VWStaticTextCtrl fHintNote;
			VWStaticTextCtrl fHintLoop;
			TXString fPrText;
			bool fAnonymize = true;
			bool fShown = false;
		};

		// NOLINTNEXTLINE(misc-const-correctness)
		EVENT_DISPATCH_MAP_BEGIN(CFeedbackDialog);
		EVENT_DISPATCH_MAP_END;

		// -------------------------------------------------------------------
		// 宛先・投稿・待機。
		// -------------------------------------------------------------------

		// 10 進の PR 番号（数字以外・空は 0）。
		int ParsePullRequest(const std::string& text)
		{
			const std::string trimmed = Trim(text);
			if (trimmed.empty())
				return 0;
			int value = 0;
			for (const char c : trimmed)
			{
				if (c < '0' || c > '9')
					return 0;
				if (value > 214748363)
					return 0;
				value = value * 10 + (c - '0');
			}
			return value;
		}

		// ブランチから open な PR 番号を引く（引けなければ 0）。**人に番号を打たせない**
		// ための当て推量で、外れてもダイアログで直せる。
		int ResolvePullRequest(const std::string& repo, const std::string& branch)
		{
			if (branch.empty() || branch == "local")
				return 0;
			std::string out;
			if (!RunScript(kFeedbackScript, {"find-pr", repo, branch}, out))
				return 0;
			return ParsePullRequest(ValueOf(out, "pr"));
		}

		// 本文を投稿する。投稿できたら true で、url にコメントの在り処が入る。
		bool PostComment(const std::string& repo, int pullRequest, const std::string& body,
						 std::string& url, std::string& error)
		{
			url.clear();
			error.clear();
			const std::string file = WriteTempFile("body", body, /*ownerOnly*/ false);
			if (file.empty())
			{
				error = "投稿する本文を一時ファイルへ書けませんでした。";
				return false;
			}
			std::string out;
			const bool ran =
				RunScript(kFeedbackScript, {"post", repo, std::to_string(pullRequest), file}, out);
			RemoveTempFile(file);
			if (!ran)
			{
				error = "フィードバック用のスクリプトを起動できませんでした。";
				return false;
			}
			const std::string reason = ValueOf(out, "error");
			if (!reason.empty())
			{
				error = reason;
				return false;
			}
			url = ValueOf(out, "url");
			return true;
		}

		// **所見を訊く仕事を別プロセスへ渡す。** こちらは待たない——`ask-note` は自分を
		// 起こし直して即座に返るので、この呼び出しで Vectorworks が止まるのは一瞬だけで、
		// 利用者は図面を拡大・レイヤ切り替えしながら所見を書ける（scripts/vw-feedback.sh）。
		//
		// **戻ってしまう以上こちらへは返せない**ので、尋ねてから投稿するところまであちらの
		// 仕事になり、所見は独立した 1 通として PR へ載る。
		//
		// 起動できなくても取り込みそのものは済んでいるので、**黙って諦める**（所見が付か
		// ないだけ。PR へ直接返信もできる）。宛先が分からないときも同じ。
		void SpawnNoteDialog(const core::FeedbackSession& session, int round,
							 const std::string& commit, const std::string& url, bool posted)
		{
			if (session.pullRequest == 0 || !HaveToken())
				return;
			std::string spawned;
			RunScript(kFeedbackScript,
					  {"ask-note", session.repo, std::to_string(session.pullRequest),
					   std::to_string(round), commit, url, posted ? "yes" : "no"},
					  spawned);
		}
	} // namespace

	// -----------------------------------------------------------------------
	bool feedbackAvailable()
	{
#ifdef VW_DEV_BUILD
		// 殻がスクリプトを貸してくれていること（古い殻・単体テストでは貸されない）。
		return hostServices().canRunScripts();
#else
		// **安定版では動かさない。** 往復するのは PR のビルドであって、main の配布物では
		// ない——安定版から PR へコメントが飛ぶのは筋が通らないし、利用者の図面の情報が
		// 外へ出る経路を、開発用でないビルドに持たせない。
		return false;
#endif
	}

	core::FeedbackSession loadFeedbackSession(const std::string& branch)
	{
		core::FeedbackSession session;
		if (!feedbackAvailable())
			return session;
		if (!core::readFeedbackSession(core::defaultFeedbackSessionPath(), session))
			return core::FeedbackSession{};

		// **別のブランチの記憶なら使わない。** 別ブランチのビルドに入れ替わったのなら、
		// それは前の往復の続きではなく、新しい往復の 1 周目である。
		if (!session.branch.empty() && !branch.empty() && session.branch != branch)
			return core::FeedbackSession{};
		return session;
	}

	bool runFeedbackRound(const FeedbackInput& input, bool& shownResult)
	{
		shownResult = false;
		if (!feedbackAvailable() || input.document == nullptr || input.counts == nullptr)
			return false;

		core::FeedbackSession session = loadFeedbackSession(input.build.branch);
		session.branch = input.build.branch;

		// 宛先の PR。記憶が無ければブランチから引く（人に番号を打たせないため）。
		if (session.pullRequest == 0)
			session.pullRequest = ResolvePullRequest(session.repo, session.branch);

		// 結果の本文＋所見＋宛先を 1 枚で尋ねる。**ここが結果ダイアログを兼ねる**
		// （送らないと決めたときだけ、呼び出し側が従来の結果ダイアログを出す）。
		const std::vector<std::string> body = SplitLines(input.resultBody);
		if (body.empty())
			return false;

		CFeedbackDialog dialog(
			"実機フィードバック（round " + std::to_string(session.round + 1) + "）", body, session);
		const bool accepted = dialog.RunDialogLayout("") == VWFC::VWUI::kDialogButton_Ok;
		if (!dialog.Shown())
			return false; // ダイアログを組めなかった → 呼び出し側が結果ダイアログへ落とす
		if (!accepted)
		{
			// 「送らない」。**記憶は消さない。** この周を報告しないことと往復をやめることは
			// 別で、やめるのは次の周の確認で「やめる」を押したときである
			// （draw/ImportCommand.cpp）。ここで消すと、報告したくない周が 1 つあっただけで
			// ファイルと設定を選び直す羽目になる。
			//
			// **それでも所見だけは訊く。** 送らないと決めた周にこそ、その理由や絵の様子と
			// いった「こちらからは決して見えないもの」があることがある——ここで黙ると、
			// その周は読む側から完全に消える（実機 round 4 の所見）。空のまま閉じれば
			// 何も投稿されないので、本当に何も残したくないときの逃げ道も残る。
			SpawnNoteDialog(session, session.round + 1, input.build.commit, /*url*/ "",
							/*posted*/ false);
			return false;
		}

		session.send = true;
		session.anonymize = dialog.Anonymize();
		session.pullRequest = ParsePullRequest(dialog.PullRequest());
		session.ifcPath = input.ifcPath;
		session.options = input.options;

		if (session.pullRequest == 0)
		{
			gSDK->AlertInform("投稿先の PR が分かりません。",
							  "PR 番号を入れて、もう一度お試しください。", false);
			return false;
		}

		std::string note;
		if (!EnsureToken(note))
		{
			gSDK->AlertInform("フィードバックを投稿できませんでした。", note.c_str(), false);
			return false;
		}

		// 本文を組む（無 SDK 側。parse/Feedback）。
		parse::FeedbackRound round;
		round.build = input.build;
		round.ifcPath = input.ifcPath;
		round.bytes = input.bytes;
		round.seconds = input.seconds;
		round.startedAt = input.startedAt;
		round.log = input.log;
		round.round = session.round + 1;
		round.previousCommit = session.lastCommit;
		round.previousTally = session.lastTally;
		round.anonymize = session.anonymize;

		const std::string commentBody =
			parse::formatFeedbackComment(round, *input.document, *input.counts);

		std::string url;
		std::string error;
		if (!PostComment(session.repo, session.pullRequest, commentBody, url, error))
		{
			gSDK->AlertInform("フィードバックを投稿できませんでした。", error.c_str(), false);
			return false;
		}

		// **ここで初めて「結果を見せ切った」ことにする。** 投稿できていれば内訳もログも
		// PR にあるので、上のダイアログで足りている。逆に**送れなかった／送らなかった
		// ときは、呼び出し側にいつもの結果ダイアログを出させる**——そちらにしか
		// 「ログを表示」が無く、困ったときに貼るものへ手が届かなくなるため
		// （draw/ResultDialog.h）。
		shownResult = true;

		// **投稿できたところで記憶を進める。** 投稿できていない周を数えると、次の
		// コメントが「前の周からの変化」を持たないまま round だけ進む。
		session.round = round.round;
		session.lastCommit = input.build.commit;
		session.lastTally = parse::formatTally(parse::elementRows(*input.document, *input.counts));
		if (!core::writeFeedbackSession(core::defaultFeedbackSessionPath(), session))
		{
			gSDK->AlertInform("投稿しました。",
							  ("ただし、次の周のための記憶を保存できませんでした（次の取り込みは"
							   "ファイル選択からになります）。\n" +
							   url)
								  .c_str(),
							  false);
			return false;
		}

		// **所見は別プロセスで訊く**（SpawnNoteDialog）。絵を見てから書くには Vectorworks を
		// 操作できなければならず、こちらのダイアログはモーダルでそれを塞ぐ（VW のレイアウト
		// ダイアログはモーダル前提で、モードレスにするには別の拡張種別が要る——[SDK
		// リファレンス「モードレス（非モーダル）なパレット」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Layout%20Dialogs.md)。
		// 実機未確認）。
		SpawnNoteDialog(session, session.round, input.build.commit, url, /*posted*/ true);

		// **ここで「投稿しました」を出さない。** モーダルのアラートは Vectorworks を
		// 止めるので、いま出したばかりの所見のダイアログの前で図面を見られなくなる。
		// 投稿できたことは、そのダイアログの文言（「round N を投稿しました」）と、
		// ブラウザで開くコメントが伝える。

		// **次の取り込みでは尋ねずに入れてよい。** この人はいま往復の最中にいるので、
		// 次に取り込みを実行するときには「新しいビルドがあります。インストールします
		// か？」を挟まない（src/Extensions/ExtMenu.cpp が UpdateCheckKind::Auto を選ぶ）。
		//
		// **待つのはこちらの仕事ではない。** 以前はここで新しいビルドが出るまで待って
		// いたが、待つあいだ進捗ダイアログが Vectorworks を止めてしまい、**その周の絵を
		// 見られない**（実機 round 3 で判明。docs/DEV-NOTES.md M23）。絵を見られないなら
		// 往復の意味が無いので、待つのをやめて即座に戻る——次の周は、新しいビルドが出た
		// あとに取り込みをもう一度実行すれば始まる（ファイル選択も設定も出ない）。
		return true;
	}
} // namespace HomeskzIfcImport::draw
