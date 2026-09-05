//
//	draw/ImportCommand.cpp
//
//	取り込みコマンドの実装（意図は draw/ImportCommand.h 参照）。**Extensions/ExtMenu.cpp
//	から本体（ペイロード）側へ移したもの**で、中身は移設前と同じ——縦切りの通し処理:
//	ファイルを選ぶ → parse（Phase 1）で IFC を Document へ → draw（Phase 2）で
//	VectorWorks へ描く → 結果をダイアログに出す。要素が増えても入口はこの形のまま
//	（各要素の追加は Document と draw 側で行う。docs/DEV-NOTES.md）。ここが両フェーズの
//	オーケストレーションを担う唯一の場所になる。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "draw/ImportCommand.h"

// Phase 1（IFC 解析）と Phase 2（VW 描画）の入口。SDK 非依存の core/parse ライブラリ
// （HomeskzIfcCore）に解析があり、SDK 依存の draw/ が描画する。ヘッダはいずれも
// core::Document までしか参照せず、SDK / STEP を相互に引き込まない。
#include "parse/BuildDocument.h"
#include "parse/Summary.h"
#include "core/Document.h"
#include "core/FeedbackSession.h"
#include "core/ImportOptions.h"
#include "core/Trace.h"
#include "draw/ExecuteDocument.h"
#include "draw/Feedback.h"
#include "draw/ProgressDialog.h"
#include "draw/ResultDialog.h"
#include "draw/SettingsDialog.h"

// ファイル選択ダイアログ（VCOM）。ネイティブの「開く」ダイアログを出し、選ばれた
// ファイルの絶対パスを IFileIdentifier 経由で受け取る。
#include "Interfaces/VectorWorks/Filing/IFileChooserDialog.h"
#include "Interfaces/VectorWorks/Filing/IFileIdentifier.h"

#include <chrono>
#include <cstddef>
#include <exception>
#include <fstream>
#include <string>
#include <utility>

using namespace VectorWorks::Filing;
namespace HomeskzIfcImport::draw
{
	namespace
	{
		// ネイティブの「開く」ダイアログで IFC ファイルを 1 つ選ばせる。選ばれたら
		// その絶対パス（UTF-8）を outPath に入れて true を返す。キャンセルや取得失敗は
		// false（呼び出し側は何も描かず静かに終える）。
		//
		// VCOM の作法（Info「VCOM」）: VCOMPtr に IID を渡して生成し、ポインタが有効かを
		// if で確かめ、各呼び出しの VCOMError を kVCOMError_NoError と比較する。選択結果は
		// IFileIdentifier（0 番目）から GetFileFullPath で受け取り、TXString の
		// operator const char*()（UTF-8）で std::string へ写す。
		bool ChooseIfcFile(std::string& outPath)
		{
			// IFileChooserDialogPtr は VCOMPtr<IFileChooserDialog> の SDK 標準 typedef。
			// const で受ける: operator-> は const なので、const のまま各インターフェース
			// メソッド（SetTitle 等）を呼べる。VCOMPtr 自体は再代入しないため
			// clang-tidy の misc-const-correctness にも従う。
			const IFileChooserDialogPtr dialog(IID_FileChooserDialog);
			if (!dialog)
				return false;

			dialog->SetTitle("ホームズ君IFCファイルを選択");
			// 拡張子フィルタ（.ifc）と、念のため全ファイル。存在チェックも有効化する。
			dialog->AddFilter("ifc", "IFC ファイル (*.ifc)");
			dialog->AddFilterAllFiles();
			dialog->SetCheckFileExist(true);

			// RunOpenDialog は OK 選択で kVCOMError_NoError を返す（キャンセルはそれ以外）。
			if (dialog->RunOpenDialog() != kVCOMError_NoError)
				return false;

			Uint32 count = 0;
			if (dialog->GetSelectedFileNamesCount(count) != kVCOMError_NoError || count == 0)
				return false;

			IFileIdentifierPtr fileID;
			if (dialog->GetSelectedFileName(0, &fileID) != kVCOMError_NoError || !fileID)
				return false;

			TXString fullPath;
			if (fileID->GetFileFullPath(fullPath) != kVCOMError_NoError)
				return false;

			// TXString → UTF-8 std::string（operator const char*() は UTF-8 を返す）。
			outPath = static_cast<const char*>(fullPath);
			return !outPath.empty();
		}

		// ファイルの大きさ（バイト）。読めなければ 0 で、そのときログは大きさを出さない。
		// **診断ログの見出しに要る**——「読み込めない」の報告で、そもそも中身のある
		// ファイルだったのかを最初に切り分けられる。
		unsigned long long FileSizeOf(const std::string& path)
		{
			std::ifstream in(path, std::ios::binary | std::ios::ate);
			if (!in)
				return 0;
			const std::streamoff size = in.tellg();
			if (size < 0)
				return 0;
			return static_cast<unsigned long long>(size);
		}

		// パスから末尾のファイル名だけを取り出す（進捗ダイアログの上段に出す 1 行）。
		// 区切りは POSIX とネイティブ Windows の両方を見る（SDK が返すパスは実行環境の
		// 流儀に従う）。区切りが無ければパスそのものがファイル名。
		std::string FileNameOf(const std::string& path)
		{
			const std::size_t pos = path.find_last_of("/\\");
			if (pos == std::string::npos)
				return path;
			return path.substr(pos + 1);
		}

		// undo イベントの状態を診断ログへ 1 行残す（docs/DEV-NOTES.md M15「Undo」）。
		//
		// **実機でしか分からない挙動なので残してある。** 実測では start=no / afterParse=no /
		// afterDraw=yes で、「VW は取り込みの開始時にイベントを開かない」「SDK 内部が描画の
		// 途中で勝手に開く」ことが分かった。いまは描画を draw::ImportUndoScope で包むので、
		// **afterDraw は no（自分で開いたイベントを閉じ切った状態）が正しい**。
		void LogUndoState(const char* when)
		{
			core::trace::log(std::string("undo: ") + when + " building=" +
							 (gSDK->IsCurrentlyBuildingAnUndoEvent() ? "yes" : "no"));
		}

		// 動かしているビルドの素性（診断ログの見出しに出す）。**ここで詰めるのは、
		// BuildConfig.h のマクロを見られるのが SDK 側だけ**だから——parse/Summary は
		// 受け取った文字列を並べるだけで、ビルド種別を知らない。
		parse::BuildInfo CurrentBuildInfo()
		{
			parse::BuildInfo build;
			build.plugin = PLUGIN_VWR_ID;
#ifdef VW_DEV_BUILD
			build.channel = "dev";
#else
			build.channel = "stable";
#endif
			build.commit = VW_BUILD_VERSION;
			build.branch = VW_BUILD_BRANCH;
			// プラットフォームは PluginPrefix.h と同じ判定（SDK の GS_MAC / GS_WIN は
			// 値で定義される流儀があるので、素の処理系マクロを見る）。
#if defined(_WINDOWS)
			build.platform = "Windows";
#elif defined(__APPLE__)
			build.platform = "macOS";
#endif
			return build;
		}

		// 診断ログを開き、見出しを書く（docs/DEV-NOTES.md M19「短い完了・厚いログ」）。
		// **取り込みのたびに必ず開く**——完了ダイアログがログをそのまま見せて「困ったら
		// これを貼る」経路にした以上、要るときに限って無いのでは意味がない（以前は
		// dev ビルドと HOMESKZ_IFC_TRACE 指定時だけだった）。開けなくても黙って続ける
		// （付随機能。本文はメモリに溜まるので、ダイアログのログ欄は変わらず読める）。
		//
		// HOMESKZ_IFC_TRACE に**パスを入れると出力先を差し替えられる**（一時ディレクトリ
		// 以外へ出したいとき用の逃げ道。値が無ければ既定の場所）。
		void OpenImportTrace(const std::string& ifcPath)
		{
			// 環境変数の読み取りは core/Trace が持つ（getenv の作法をあちこちに書かない）。
			const std::string custom = core::trace::envValue("HOMESKZ_IFC_TRACE");
			const std::string path =
				custom.empty() ? core::trace::defaultLogPath("HomeskzIfcImport.log") : custom;
			core::trace::open(path); // 開けなくても本文は溜まる（core/Trace.h）
			// **`core::trace::path()` を必ず渡す。** ここを省くと `formatLogHeader` の
			// 既定値（空）が効いて、**実際には書けているのに見出しが「ファイルへは
			// 書けませんでした」と言う**（実機のログで発覚。M19 でこの見出しを足して以来
			// ずっとそうなっていた）。`path()` は開けたときだけ値を持ち、開けなければ空を
			// 返すので、そのまま渡せば両方の場合が正しくなる（core/Trace.h）。
			core::trace::note(
				parse::formatLogHeader(CurrentBuildInfo(), ifcPath, FileSizeOf(ifcPath),
									   core::trace::localTimestamp(), core::trace::path()));
		}

		// 取り込み 1 周ぶんの結果。**完了ダイアログの本文だけでは足りない**——実機
		// フィードバック（draw/Feedback）は命令セットと描画結果そのものを見て
		// 内訳と差分を組み立てるので、それらをここから持ち帰る。
		struct RoundResult
		{
			std::string body;			  // 完了ダイアログの短い本文
			core::Document document;	  // 命令セット
			core::DrawCounts counts;	  // 描画結果
			unsigned long long bytes = 0; // 対象ファイルの大きさ
			double seconds = 0.0;		  // 所要
			std::string startedAt;		  // 壁時計（ログの見出しと同じもの）
		};

		// インポート本体（ファイル選択の後）。解析 → 描画を通し、結果一式を返す。
		// 例外はここでは受けず、呼び出し元が SDK コールバックの境界で 1 か所だけ
		// 受け止める。進捗ダイアログは RAII なので、途中で例外が出てもデストラクタが閉じる。
		RoundResult RunImport(const std::string& ifcPath, const core::ImportOptions& options,
							  bool settingsShown, const std::string& settingsNote)
		{
			RoundResult result;
			result.startedAt = core::trace::localTimestamp();
			result.bytes = FileSizeOf(ifcPath);
			OpenImportTrace(ifcPath);
			// **見出しの次に設定を書く。** 「シンボルが 1 つも置かれない」の切り分けは
			// まず対応表を見るところから始まる（parse/Summary の formatImportOptions）。
			// 設定ダイアログを出せなかったときは、既定で続けたことも残す。**ダイアログ側の
			// 記録（どの形で出したか・何が駄目だったか）もここへ**——「設定ダイアログが
			// 出ない」の切り分けはこの 1 行から始まる（draw/SettingsDialog.h）。
			if (!settingsShown)
				core::trace::note("設定: ダイアログを出せなかったため既定の対応で取り込みます");
			if (!settingsNote.empty())
				core::trace::note("設定ダイアログ: " + settingsNote);
			core::trace::note(parse::formatImportOptions(options));
			// 所要時間は**トレースとは別に**測る（ログを開けなくても完了ダイアログに出す）。
			const auto started = std::chrono::steady_clock::now();
			LogUndoState("start");

			// 進捗ダイアログを開く。両フェーズへ**同じ 1 つ**を渡し、解析→描画を通して
			// 見出しとバーを進める。描画は横架材・垂木を 1 本ずつ SDK で作るため数百回の
			// 呼び出しになり、これが無いと VectorWorks が固まったように見える
			// （draw/ProgressDialog.h「なぜ要るか」）。
			draw::ProgressDialog progress("ホームズ君 IFC インポート", FileNameOf(ifcPath));

			// Phase 1（SDK 非依存）: IFC を解析して命令セット（Document）を組み立てる。
			// 読み込み失敗も例外を漏らさず空の Document として返る（1 要素の欠損で止めない）。
			// フェーズの区切りは**ここだけ**が書く——各フェーズの行は進捗報告（core/Progress の
			// beginPhase）が流し、要素側は `trace::log` を持たない（core/Trace.h「誰が書くか」）。
			core::trace::note("=== 解析 ===");
			core::Document document = parse::buildDocument(ifcPath, progress, options);
			LogUndoState("afterParse");

			// Phase 2（SDK 依存）: 命令セットを検証してから各要素を描く。検証を通らなければ
			// valid=false で何も描かない。途中でキャンセルされたら、その時点までを描いて
			// cancelled=true で戻る。
			// 図面変更は draw 側が自前の undo イベント（draw::ImportUndoScope）で包む。
			// executeDocument から戻った時点でイベントは閉じているので、building=no に
			// なっているはず——そこが崩れると「取り消し」で図面が壊れるので、ログで見る。
			core::trace::note("=== 描画 ===");
			const draw::DrawCounts drawn = draw::executeDocument(document, progress);
			LogUndoState("afterDraw");

			// 完了ダイアログの前に進捗ダイアログを閉じる（2 枚重ねない）。
			progress.close();

			const double seconds =
				std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

			// 本文の組み立ては**無 SDK 側**（parse/Summary）が持つ。要素が増えても
			// ここは変わらない（docs/DEV-NOTES.md M15「完了文言の集約」）。**要素ごとの
			// 内訳・注意・記録はログへ**、ダイアログには「どのファイルを・成功したか・
			// 問題はあったか」だけ（M19）。
			core::trace::note(parse::formatLogResult(document, drawn, seconds));
			core::trace::close();

			result.body = parse::formatImportResult(document, drawn, FileNameOf(ifcPath));
			result.document = std::move(document);
			result.counts = drawn;
			result.seconds = seconds;
			return result;
		}

		// 例外で中断したときの後始末と本文づくり。診断ログに例外を書き残してから閉じ、
		// ダイアログ本文（無 SDK 側が組み立てる）を返す。ログの場所は見出しにあるので
		// ここでは添えない。
		std::string ReportImportError(const std::string& ifcPath, const std::string& detail)
		{
			core::trace::note("=== 結果 ===\n結果: エラーで中断\n詳細: " +
							  (detail.empty() ? std::string("原因不明") : detail));
			core::trace::close();
			return parse::formatImportError(detail, FileNameOf(ifcPath));
		}
	} // namespace

	// -------------------------------------------------------------------
	// メニューコマンドの本体（draw/ImportCommand.h）。
	bool runImportCommand()
	{
		// Note: the dev-build picker is NOT run here. It runs once at Vectorworks
		// start-up (see plugin_module_main -> RunDevStartupCheck) because a compiled
		// plug-in can only be swapped in at load time, and because the command may be
		// re-invoked programmatically — a picker on the command path would then pop up
		// repeatedly. So the command just does its work below, every time it runs.

		// 縦切りの通し処理: ファイルを選ぶ → parse（Phase 1）で IFC を Document へ → draw（Phase 2）
		// で VectorWorks へ描く → 件数をダイアログに出す。要素が増えても入口はこの形のまま（各要
		// 素の追加は Document と draw 側で行う。docs/DEV-NOTES.md）。ここが両フェーズの
		// オーケストレーションを担う唯一の場所になる。

		const parse::BuildInfo build = CurrentBuildInfo();

		// 0. **実機フィードバックの往復の続きか。** 続きなら 1 周目の選択（ファイル・設定）を
		//    そのまま使い、ファイル選択も設定ダイアログも出さない——ここで人の操作を挟むと、
		//    往復を自動にした意味が無くなる（draw/Feedback.h）。
		const core::FeedbackSession session = draw::loadFeedbackSession(build.branch);
		const bool continuing =
			session.send && session.autoContinue && session.round > 0 && !session.ifcPath.empty();

		std::string ifcPath = session.ifcPath;
		core::ImportOptions options = session.options;
		bool settingsShown = true;
		std::string settingsNote;

		if (continuing)
		{
			// **図面を戻してもらう。** 同じ文書へ 2 回描くと前の周の図形が二重に残る。
			// プログラムから「取り消し」を掛ける手立ては確かめていないので（SDK の
			// 調査はリファレンス側で行う。CLAUDE.md）、ここは 1 クリックで頼む。
			const std::string advice = "取り込み前の状態に戻してから（「取り消し」）"
									   "「続ける」を押してください。\n\nファイル: " +
									   FileNameOf(ifcPath);
			if (gSDK->AlertQuestion("新しいビルドで同じ IFC を取り込み直します。", advice.c_str(),
									/*defaultButton*/ 1, "続ける", "やめる", "", "") != 1)
				return false;
			settingsNote = "前の周の設定をそのまま使いました（実機フィードバックの往復）";
		}
		else
		{
			// 1. ネイティブの「開く」ダイアログで IFC を 1 つ選ばせる。キャンセルなら静かに終える。
			if (!ChooseIfcFile(ifcPath))
				return false;

			// 2. 取り込みの設定（配置するシンボルの対応）を決める。キャンセルなら静かに終える
			//    ——ファイルは選んだが取り込みたくない、という意思表示なので何も描かない。
			//    ダイアログを組めなかったときは**既定の対応でそのまま進む**（設定を出せない
			//    ことを理由に取り込み自体を落とさない。draw/SettingsDialog.h）。
			options = core::ImportOptions{};
			const draw::SettingsOutcome settings = draw::showImportSettings(options, &settingsNote);
			if (settings == draw::SettingsOutcome::Cancelled)
				return false;
			settingsShown = settings == draw::SettingsOutcome::Accepted;
		}

		// 3. インポート本体。**例外を SDK コールバックの外へ漏らさない**（CLAUDE.md
		// 「エラーハンドリング・所有権」）。ネイティブプラグインの未捕捉例外は **VectorWorks
		// 本体を巻き込んで落とす**ので、フェーズ境界であるここで必ず受け止め、ユーザーへは
		// 1 通のダイアログとして見せる。1 要素の欠損で全体を止めない寛容さ（parse / draw の中で
		// continue する）は従来どおりで、ここへ来るのは「そこでも吸収できなかった異常」だけ。
		RoundResult round;
		bool failed = false;
		try
		{
			round = RunImport(ifcPath, options, settingsShown, settingsNote);
		}
		catch (const std::exception& error)
		{
			round.body = ReportImportError(ifcPath, error.what());
			failed = true;
		}
		catch (...)
		{
			// std::exception ですらないもの（サードパーティや処理系が投げるもの）。
			// 何が起きたかは分からないが、**それでも VW を落とさない**ことが最優先。
			round.body = ReportImportError(ifcPath, "");
			failed = true;
		}

		// 4. 実機フィードバック（dev ビルドのみ）。結果の本文と所見を 1 枚で見せて PR へ
		//    投稿し、「自動で続ける」なら修正版のビルドを待って入れる。**そのダイアログが
		//    結果ダイアログを兼ねる**ので、出せたなら下の結果ダイアログは出さない。
		//    エラーで中断した周は送らない（送るべき内訳がそもそも無い）。
		bool shownResult = false;
		if (!failed && draw::feedbackAvailable())
		{
			draw::FeedbackInput input;
			input.document = &round.document;
			input.counts = &round.counts;
			input.build = build;
			input.options = options;
			input.ifcPath = ifcPath;
			input.bytes = round.bytes;
			input.seconds = round.seconds;
			input.startedAt = round.startedAt;
			input.resultBody = round.body;
			input.log = core::trace::text();
			if (draw::runFeedbackRound(input, shownResult))
				return true; // 新しい本体が入った → 殻が持ち直して、もう 1 周
		}

		// 5. 結果をダイアログ表示。本文は短く、**診断ログは折り畳んだテキスト欄**として同じ
		//    ダイアログに載せる（draw/ResultDialog.h。ふだんは開かず、不具合の報告のときに
		//    開いて丸ごとコピーする）。
		if (!shownResult &&
			!draw::showImportResult("ホームズ君 IFC 取り込み", round.body, core::trace::text()))
		{
			// ダイアログを組めなかったときの逃げ道。結果を伝えられないまま黙って終わるのが
			// 最悪なので、素のアラートへ落とす（advice 行にファイルパス。false = 最小アラート
			// でなくモーダル）。TXString は UTF-8 の const char* から暗黙変換される。
			gSDK->AlertInform(round.body.c_str(), ifcPath.c_str(),
							  false /* not a minor alert: show a modal dialog */);
		}
		return false;
	}
} // namespace HomeskzIfcImport::draw
