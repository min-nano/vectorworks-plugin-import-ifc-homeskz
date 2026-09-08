//
//	draw/ImportRun.cpp
//
//	取り込み 1 周ぶんの部品の実装（意図は draw/ImportRun.h 参照）。**中身は M24 まで
//	draw/ImportCommand.cpp の無名名前空間に在ったものをそのまま出しただけ**で、絵を作る
//	手順は 1 行も変えていない（M25 で分けたのは「誰が呼ぶか」だけ）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "draw/ImportRun.h"

// Phase 1（IFC 解析）と Phase 2（VW 描画）の入口。SDK 非依存の core/parse ライブラリ
// （HomeskzIfcCore）に解析があり、SDK 依存の draw/ が描画する。ヘッダはいずれも
// core::Document までしか参照せず、SDK / STEP を相互に引き込まない。
#include "parse/BuildDocument.h"
#include "parse/Summary.h"
#include "core/Document.h"
#include "core/ImportOptions.h"
#include "core/Trace.h"
#include "draw/ExecuteDocument.h"
#include "draw/ProgressDialog.h"

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
				custom.empty() ? core::trace::defaultLogPath("min-nano_structure.log") : custom;
			core::trace::open(path); // 開けなくても本文は溜まる（core/Trace.h）
			// **`core::trace::path()` を必ず渡す。** ここを省くと `formatLogHeader` の
			// 既定値（空）が効いて、**実際には書けているのに見出しが「ファイルへは
			// 書けませんでした」と言う**（実機のログで発覚。M19 でこの見出しを足して以来
			// ずっとそうなっていた）。`path()` は開けたときだけ値を持ち、開けなければ空を
			// 返すので、そのまま渡せば両方の場合が正しくなる（core/Trace.h）。
			core::trace::note(
				parse::formatLogHeader(currentBuildInfo(), ifcPath, FileSizeOf(ifcPath),
									   core::trace::localTimestamp(), core::trace::path()));
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

		// インポート本体。解析 → 描画を通し、結果一式を返す。**例外はここでは受けず**、
		// 下の runImportRound が 1 か所だけで受け止める。進捗ダイアログは RAII なので、
		// 途中で例外が出てもデストラクタが閉じる。
		ImportRound runImportRoundUnguarded(const std::string& ifcPath,
											const core::ImportOptions& options, bool settingsShown,
											const std::string& settingsNote,
											const std::string& prologue)
		{
			ImportRound result;
			result.startedAt = core::trace::localTimestamp();
			result.bytes = FileSizeOf(ifcPath);
			OpenImportTrace(ifcPath);
			// **準備は設定より先に書く。** 図面をどう用意したかは、数字を読むより前に
			// 知りたい 1 行である（実機テストの周だけが渡す。draw/ImportRun.h）。
			if (!prologue.empty())
				core::trace::note(prologue);
			// **その次に設定を書く。** 「シンボルが 1 つも置かれない」の切り分けは
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

	} // namespace

	// -------------------------------------------------------------------
	// ネイティブの「開く」ダイアログで IFC ファイルを 1 つ選ばせる。選ばれたら
	// その絶対パス（UTF-8）を outPath に入れて true を返す。キャンセルや取得失敗は
	// false（呼び出し側は何も描かず静かに終える）。
	//
	// VCOM の作法（Info「VCOM」）: VCOMPtr に IID を渡して生成し、ポインタが有効かを
	// if で確かめ、各呼び出しの VCOMError を kVCOMError_NoError と比較する。選択結果は
	// IFileIdentifier（0 番目）から GetFileFullPath で受け取り、TXString の
	// operator const char*()（UTF-8）で std::string へ写す。
	bool chooseIfcFile(std::string& outPath)
	{
		return chooseFile("ホームズ君IFCファイルを選択", {"ifc"}, "IFC ファイル (*.ifc)", outPath);
	}

	bool chooseFile(const std::string& title, const std::vector<std::string>& extensions,
					const std::string& extensionLabel, std::string& outPath)
	{
		// IFileChooserDialogPtr は VCOMPtr<IFileChooserDialog> の SDK 標準 typedef。
		// const で受ける: operator-> は const なので、const のまま各インターフェース
		// メソッド（SetTitle 等）を呼べる。VCOMPtr 自体は再代入しないため
		// clang-tidy の misc-const-correctness にも従う。
		const IFileChooserDialogPtr dialog(IID_FileChooserDialog);
		if (!dialog)
			return false;

		dialog->SetTitle(TXString(title.c_str()));
		// 拡張子フィルタと、念のため全ファイル。存在チェックも有効化する。
		// **1 拡張子につき 1 回呼ぶ**（まとめて渡すと効かない。draw/ImportRun.h）。
		for (const std::string& extension : extensions)
			dialog->AddFilter(TXString(extension.c_str()), TXString(extensionLabel.c_str()));
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

	// 動かしているビルドの素性（診断ログの見出しに出す）。**ここで詰めるのは、
	// BuildConfig.h のマクロを見られるのが SDK 側だけ**だから——parse/Summary は
	// 受け取った文字列を並べるだけで、ビルド種別を知らない。
	parse::BuildInfo currentBuildInfo()
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

	// -------------------------------------------------------------------
	// **例外をここで受け止める。** ネイティブプラグインの未捕捉例外は VectorWorks 本体を
	// 巻き込んで落とすので、フェーズ境界であるここで必ず捕まえる（CLAUDE.md
	// 「エラーハンドリング・所有権」）。1 要素の欠損で全体を止めない寛容さ（parse / draw の
	// 中で continue する）は従来どおりで、ここへ来るのは「そこでも吸収できなかった異常」だけ。
	//
	// **呼び出し側は failed を見るだけでよい。** 本番のコマンドは結果ダイアログへ、テストの
	// 周は「この周は送らない」の判断へ使う——どちらも try/catch を書かずに済む。
	ImportRound runImportRound(const std::string& ifcPath, const core::ImportOptions& options,
							   bool settingsShown, const std::string& settingsNote,
							   const std::string& prologue)
	{
		try
		{
			return runImportRoundUnguarded(ifcPath, options, settingsShown, settingsNote, prologue);
		}
		catch (const std::exception& error)
		{
			ImportRound failed;
			failed.body = ReportImportError(ifcPath, error.what());
			failed.failed = true;
			return failed;
		}
		catch (...)
		{
			// std::exception ですらないもの（サードパーティや処理系が投げるもの）。
			// 何が起きたかは分からないが、**それでも VW を落とさない**ことが最優先。
			ImportRound failed;
			failed.body = ReportImportError(ifcPath, "");
			failed.failed = true;
			return failed;
		}
	}
} // namespace HomeskzIfcImport::draw
