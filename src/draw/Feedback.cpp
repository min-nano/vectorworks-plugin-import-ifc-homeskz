//
//	draw/Feedback.cpp
//
//	実機フィードバックの往復の実装（意図と 1 周の形は draw/Feedback.h 参照）。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）と VWFC のダイアログを include する。
//
//	使う SDK API はダイアログ 2 種（draw/ResultDialog と同じ作法。VWFC/VWUI/…）と
//	進捗ダイアログ（draw/ProgressDialog）だけで、**ネットワークには一切触れない**——
//	投稿もビルドの取得も同梱スクリプトが行い、こちらはその機械可読な出力を読む
//	（自動アップデートと同じ分担。src/Updater.h）。
//
//	【文字列の受け渡し】スクリプトの出力は `UpdaterParse` の純粋な関数で解く
//	（ValueOf / ParseDevBuilds / InstalledShellId …）。**同じ解き方を 2 つ持たない**ため、
//	殻の自動アップデートが使っているものをそのまま使う（CLAUDE.md「重複を作らない置き場所」）。
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
#include "draw/ProgressDialog.h"
#include "parse/Feedback.h"
#include "parse/Summary.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace HomeskzIfcImport::UpdaterParse;

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 同梱スクリプトの名前（拡張子は殻が付ける。src/PayloadAbi.h）。
		constexpr const char* kFeedbackScript = "vw-feedback";
		constexpr const char* kUpdateScript = "vw-update";

		// 新しいビルドを待つときの刻み。**30 秒ごとに 1 回問い合わせ**、待つのは
		// 最長 90 分。CI（ビルド + clang-tidy）は 20〜40 分かかるので、1 往復ぶんに
		// 十分な余裕を取りつつ、間違って始めた待機が半日居座らない長さにしてある。
		constexpr int kPollSeconds = 30;
		constexpr int kMaxWaitMinutes = 90;
		// 眠りを刻む幅（ms）。**この刻みごとに進捗ダイアログへ yield する**ので、
		// 待っている間も Vectorworks が固まって見えず、中止も効く。
		constexpr int kSleepSliceMs = 250;

		// ダイアログのコントロール ID（1 = OK / 2 = キャンセルは SDK の予約）。
		constexpr TControlID kNoteLabelID = 4;
		constexpr TControlID kNoteID = 5;
		constexpr TControlID kPrLabelID = 6;
		constexpr TControlID kPrID = 7;
		constexpr TControlID kAutoID = 8;
		constexpr TControlID kAnonID = 9;
		constexpr TControlID kFirstBodyID = 20;

		// 所見欄の大きさ（標準文字幅・行数）。**結果の本文より広く取らない**——
		// 書くのは一言か二言で、広い欄は「たくさん書け」という圧になる。
		constexpr short kNoteWidthChars = 72;
		constexpr short kNoteHeightLines = 5;
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
		// フィードバックのダイアログ（結果の本文＋所見＋宛先＋続け方）。
		// -------------------------------------------------------------------

		class CFeedbackDialog : public VWDialog
		{
		public:
			CFeedbackDialog(const std::string& title, const std::vector<std::string>& body,
							const core::FeedbackSession& session)
				: fTitle(title.c_str()), fBody(body), fNote(kNoteID), fNoteLabel(kNoteLabelID),
				  fPrLabel(kPrLabelID), fPr(kPrID), fAuto(kAutoID), fAnon(kAnonID),
				  fPrText(std::to_string(session.pullRequest).c_str()),
				  fAutoContinue(session.autoContinue), fAnonymize(session.anonymize)
			{
			}
			~CFeedbackDialog() override = default;

			bool Shown() const
			{
				return fShown;
			}
			std::string Note() const
			{
				return static_cast<const char*>(fNoteText);
			}
			std::string PullRequest() const
			{
				return static_cast<const char*>(fPrText);
			}
			bool AutoContinue() const
			{
				return fAutoContinue;
			}
			bool Anonymize() const
			{
				return fAnonymize;
			}

		protected:
			bool CreateDialogLayout() override
			{
				if (!this->CreateDialog(fTitle, "PR へ送る", "送らない", false))
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

				if (!fNoteLabel.CreateControl(this,
											  "所見（実機を見て気付いたこと。空でも送れます）:"))
					return false;
				this->AddBelowControl(previous, &fNoteLabel, 0, 1);
				if (!fNote.CreateControl(this, "", kNoteWidthChars, kNoteHeightLines))
					return false;
				this->AddBelowControl(&fNoteLabel, &fNote);

				if (!fPrLabel.CreateControl(this, "送信先の PR 番号:"))
					return false;
				this->AddBelowControl(&fNote, &fPrLabel, 0, 1);
				if (!fPr.CreateControl(this, "", kPrWidthChars, 1))
					return false;
				this->AddRightControl(&fPrLabel, &fPr);

				if (!fAuto.CreateControl(this, "修正版のビルドが出たら、待って自動で取り込み直す"))
					return false;
				this->AddBelowControl(&fPrLabel, &fAuto, 0, 1);
				if (!fAnon.CreateControl(this, "ファイル名とユーザー名を伏せて投稿する"))
					return false;
				this->AddBelowControl(&fAuto, &fAnon);
				return true;
			}

			void OnInitializeContent() override
			{
				VWDialog::OnInitializeContent();
				// **初期値は自分でも入れる。** DDX が流し込む前提には寄りかからない
				// （draw/SettingsDialog も同じく SetState を明示している）——ここが空だと
				// 「前回どおりでよい」ときに毎回打ち直すことになる。
				fPr.SetText(fPrText);
				fAuto.SetState(fAutoContinue);
				fAnon.SetState(fAnonymize);
				fShown = true;
			}

			void OnDDXInitialize() override
			{
				this->AddDDX_EditText(kNoteID, &fNoteText);
				this->AddDDX_EditText(kPrID, &fPrText);
				this->AddDDX_CheckButton(kAutoID, &fAutoContinue);
				this->AddDDX_CheckButton(kAnonID, &fAnonymize);
			}

			DEFINE_EVENT_DISPATH_MAP;

		private:
			TXString fTitle;
			std::vector<std::string> fBody;
			std::deque<VWStaticTextCtrl> fLines;
			VWEditTextCtrl fNote;
			VWStaticTextCtrl fNoteLabel;
			VWStaticTextCtrl fPrLabel;
			VWEditTextCtrl fPr;
			VWCheckButtonCtrl fAuto;
			VWCheckButtonCtrl fAnon;
			TXString fNoteText;
			TXString fPrText;
			bool fAutoContinue = true;
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

		// 待機の結末。
		enum class WaitOutcome
		{
			Installed, // 新しいビルドを入れた（本体だけ＝そのまま次の周へ）
			NeedsRestart, // 殻まで変わった（次の起動でしか効かない）
			Cancelled,	  // 中止された
			TimedOut,	  // 待ちきれなかった
			Failed,		  // 取得・インストールに失敗した
		};

		// **同じブランチの新しい dev ビルド**が出るまで待って、出たら入れる。
		// 待っている間は進捗ダイアログを出し、刻みごとに yield して中止を受け付ける。
		WaitOutcome WaitForNextBuild(const std::string& branch, const std::string& runningCommit,
									 std::string& detail)
		{
			detail.clear();
			ProgressDialog progress("実機フィードバック", "ブランチ " + branch, true);

			const int slicesPerPoll = (kPollSeconds * 1000) / kSleepSliceMs;
			const int polls = (kMaxWaitMinutes * 60) / kPollSeconds;
			progress.beginPhase("修正版のビルドを待っています（中止で終われます）", 100,
								static_cast<std::size_t>(polls) *
									static_cast<std::size_t>(slicesPerPoll));

			for (int poll = 0; poll < polls; ++poll)
			{
				std::string out;
				if (RunScript(kUpdateScript, {"q-dev"}, out) && ValueOf(out, "error").empty())
				{
					// 自分と同じコミットは候補から外れる（DevSwitchCandidates）。そのうえで
					// **同じブランチのもの**だけを採る（UpdaterParse.h の FindDevBuildForBranch）。
					const std::vector<DevBuild> candidates =
						DevSwitchCandidates(out, runningCommit);
					const int index = FindDevBuildForBranch(candidates, branch);
					if (index >= 0)
					{
						const DevBuild& build = candidates[static_cast<std::size_t>(index)];
						progress.beginPhase("新しいビルド（" + build.commit + "）を入れています",
											100, 0);
						std::string install;
						if (!RunScript(kUpdateScript, {"do-install", build.url, PLUGIN_VWR_ID},
									   install))
						{
							detail = "アップデータを起動できませんでした。";
							return WaitOutcome::Failed;
						}
						if (!InstallReportedOk(install))
						{
							detail = InstallErrorText(install, "インストールに失敗しました。");
							return WaitOutcome::Failed;
						}
						detail = build.commit;
						// **殻まで変わったなら、この実行では効かない**（コンパイル済みの
						// 殻は起動時にしか読み込まれない。src/PayloadAbi.h）。
						if (NeedsRestartAfterInstall(hostServices().shellId,
													 InstalledShellId(install)))
							return WaitOutcome::NeedsRestart;
						return WaitOutcome::Installed;
					}
				}

				// 次の問い合わせまで眠る。**刻んで眠り、刻みごとに yield する**ので、
				// 待っている間も画面が更新され、中止が効く。
				for (int slice = 0; slice < slicesPerPoll; ++slice)
				{
					if (progress.cancelled())
						return WaitOutcome::Cancelled;
					std::this_thread::sleep_for(std::chrono::milliseconds(kSleepSliceMs));
					progress.step();
				}
			}
			return WaitOutcome::TimedOut;
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
			// 「送らない」。**記憶は消す**——次の取り込みがひとりでに自動周回を始めると
			// 驚くので、往復をやめる意思表示として扱う。
			core::clearFeedbackSession(core::defaultFeedbackSessionPath());
			return false;
		}

		session.send = true;
		session.autoContinue = dialog.AutoContinue();
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
		round.note = dialog.Note();
		round.log = input.log;
		round.round = session.round + 1;
		round.previousCommit = session.lastCommit;
		round.previousTally = session.lastTally;
		round.anonymize = session.anonymize;
		round.autoContinue = session.autoContinue;

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
							  ("ただし、次の周のための記憶を保存できませんでした（自動継続は"
							   "できません）。\n" +
							   url)
								  .c_str(),
							  false);
			return false;
		}

		if (!session.autoContinue)
		{
			gSDK->AlertInform("投稿しました。", url.c_str(), false);
			return false;
		}

		// 新しいビルドを待つ。
		std::string detail;
		const WaitOutcome outcome = WaitForNextBuild(session.branch, input.build.commit, detail);
		switch (outcome)
		{
		case WaitOutcome::Installed:
			// **ここでホットリロードに入る。** 呼び出し側は何もせずに戻り、殻が本体を
			// 持ち直して取り込みを呼び直す（draw/Feedback.h「なぜ殻を経由して周回するか」）。
			return true;
		case WaitOutcome::NeedsRestart:
		{
			const std::string headline =
				"新しいビルド（" + detail + "）を入れましたが、反映には再起動が必要です。";
			gSDK->AlertInform(headline.c_str(),
							  "殻（プラグインのモジュール）まで変わったため、この実行では"
							  "入れ替えられません。\nVectorworks を再起動してから、もう一度"
							  "取り込みを実行してください（同じ条件で続きから走ります）。",
							  false);
			return false;
		}
		case WaitOutcome::Cancelled:
			gSDK->AlertInform("自動の続きをやめました。",
							  "投稿は済んでいます。次の取り込みを手で実行すれば、同じ条件で"
							  "続きから走ります。",
							  false);
			return false;
		case WaitOutcome::TimedOut:
			gSDK->AlertInform("修正版のビルドが出ませんでした。",
							  "投稿は済んでいます。ビルドが出てから取り込みを手で実行すれば、"
							  "同じ条件で続きから走ります。",
							  false);
			return false;
		case WaitOutcome::Failed:
			break;
		}
		gSDK->AlertInform("新しいビルドを入れられませんでした。", detail.c_str(), false);
		return false;
	}
} // namespace HomeskzIfcImport::draw
