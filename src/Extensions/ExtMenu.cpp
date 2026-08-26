//
//	ExtMenu.cpp
//
//	Implementation of the plug-in's menu command.
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtMenu.h"

// Phase 1（IFC 解析）と Phase 2（VW 描画）の入口。SDK 非依存の core/parse ライブラリ
// （HomeskzIfcCore）に解析があり、SDK 依存の draw/ が描画する。このメニューコマンドが両
// フェーズをオーケストレーションする。ヘッダはいずれも core::Document までしか参照せず、SDK
// / STEP を相互に引き込まない。
#include "parse/BuildDocument.h"
#include "parse/Summary.h"
#include "core/Trace.h"
#include "draw/ExecuteDocument.h"
// 【一時計装 ── 役目を終えたら消す】dev ビルドでのみ働く凡例のダンプ。
#include "draw/AuxProbe.h"
#include "draw/ProgressDialog.h"

// ファイル選択ダイアログ（VCOM）。ネイティブの「開く」ダイアログを出し、選ばれた
// ファイルの絶対パスを IFileIdentifier 経由で受け取る。
#include "Interfaces/VectorWorks/Filing/IFileChooserDialog.h"
#include "Interfaces/VectorWorks/Filing/IFileIdentifier.h"

#include <cstddef>
#include <exception>
// 【一時計装】凡例のダンプの書き出しに使う（dev ビルドのみ。役目を終えたら消す）。
#include <fstream>
#include <ios>
#include <string>

using namespace HomeskzIfcImport;
using namespace VectorWorks::Filing;

namespace HomeskzIfcImport
{
	namespace
	{
		// Description of the menu command. The SResString entries ({resource,
		// identifier}) point at strings in the plug-in's .vwr resource file; a
		// resource file is optional for a build to succeed. PLUGIN_VWR_ID differs
		// between the stable and dev builds (see BuildConfig.h) so each loads its
		// own strings. File-local (anonymous namespace) rather than `static`.
		//
		// Needs = DocIsActive: 文書（デザインレイヤを持つ図面）がアクティブな
		// ときだけコマンドを有効にし、開いている文書が無ければ VW が自動で
		// グレーアウトする。これがメニュー有効化の宣言的かつ確実な仕組みで、
		// SDK 公式サンプル（WebPaletteExample / ProcessResources）も同じ
		// EMenuEnableFlags::DocIsActive を Needs に指定している。本プラグインは
		// アクティブなレイヤへ描画するため、文書が無い状態では実行させない。
		//
		// 以前は Needs を None（= EMenuEnableFlags{}）にしたうえで GetItemEnabled()
		// 動的フックで GetCurrentLayer() を判定していたが、VW のメニュー有効化は
		// まず Needs/NeedsNot フラグで決まり、None のままだと文書の有無に関わらず
		// 常に有効になってしまう（ローカル確認で「常に実行でき、文書未オープンでも
		// 描画メッセージが出る」と判明）。そのため宣言的な DocIsActive フラグへ
		// 移し、動的フックは廃止した。
		//
		// EMenuEnableFlags は SDK 内で
		//   None        = EMenuEnableFlags(0)
		//   DocIsActive = EMenuEnableFlags(1 << 0)
		// と定義されている（Kernel/API/MiniCadCallBacks）。
		//
		// menuDef() は名前空間スコープ変数ではなく関数ローカル static で保持する。
		// EMenuEnableFlags::DocIsActive / ::None は SDK（別 TU）の非ローカル static
		// なので、これを名前空間スコープ変数の初期化子で直接参照すると静的初期化
		// 順序に依存し、clang-tidy の cppcoreguidelines-interfaces-global-init が
		// エラーにする（"initializing non-local variable with non-const expression
		// depending on uninitialized non-local variable 'DocIsActive'"）。関数
		// ローカル static は初回呼び出し時に初期化されるため、その順序問題を避け
		// つつ名前付き定数のまま書ける（EMenuEnableFlags{} の頃はこの依存が無く
		// 出なかった）。
		const SMenuDef& menuDef()
		{
			static const SMenuDef def = {/*Needs*/ EMenuEnableFlags::DocIsActive,
										 /*NeedsNot*/ EMenuEnableFlags::None,
										 /*Title*/ {PLUGIN_VWR_ID, "title"},
										 /*Category*/ {PLUGIN_VWR_ID, "category"},
										 /*HelpText*/ {PLUGIN_VWR_ID, "help"},
										 /*VersionCreated*/ 31,
										 /*VersionModified*/ 0,
										 /*VersionRetired*/ 0,
										 /*OverrideHelpID*/ ""};
			return def;
		}

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
	} // namespace
} // namespace HomeskzIfcImport

// Every extension needs a globally unique ID and universal name. The stable and
// dev builds MUST use different ones so both plug-ins can be loaded at once.
//
// NOLINT: IMPLEMENT_VWMenuExtension is an SDK macro whose expansion trips
// misc-const-correctness (a `static VWIID iid` it could mark const); that is the
// macro's code, not ours, so silence the check across the two invocations.
// NOLINTBEGIN(misc-const-correctness)
#ifdef VW_DEV_BUILD
// UUID: 2368a4b2-0497-4bcc-89f6-fc436736de2b  (dev build)
IMPLEMENT_VWMenuExtension(
	/*Extension class*/ CExtMenuImportIfc,
	/*Event sink*/ CImportIfcMenu_EventSink,
	/*Universal name*/ PLUGIN_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0x2368a4b2, 0x0497, 0x4bcc, 0x89, 0xf6, 0xfc, 0x43, 0x67, 0x36, 0xde, 0x2b);
#else
// UUID: 137bde33-b2f1-4382-b3dc-1eef297f1b12  (stable build)
IMPLEMENT_VWMenuExtension(
	/*Extension class*/ CExtMenuImportIfc,
	/*Event sink*/ CImportIfcMenu_EventSink,
	/*Universal name*/ PLUGIN_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0x137bde33, 0xb2f1, 0x4382, 0xb3, 0xdc, 0x1e, 0xef, 0x29, 0x7f, 0x1b, 0x12);
#endif
// NOLINTEND(misc-const-correctness)

// ---------------------------------------------------------------------------
CExtMenuImportIfc::CExtMenuImportIfc(CallBackPtr cbp) : VWExtensionMenu(cbp, menuDef()) {}

CExtMenuImportIfc::~CExtMenuImportIfc() = default;

// ---------------------------------------------------------------------------
CImportIfcMenu_EventSink::CImportIfcMenu_EventSink(IVWUnknown* parent) : VWMenu_EventSink(parent) {}

CImportIfcMenu_EventSink::~CImportIfcMenu_EventSink() = default;

namespace HomeskzIfcImport
{
	namespace
	{
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

		// クラッシュ診断ログを開く（docs/DEV-NOTES.md M15「core/Trace」）。**dev ビルドでは常に、
		// stable では環境変数 HOMESKZ_IFC_TRACE があるときだけ**開く——常時ログを吐くのは
		// 実運用では余計で、しかし不具合を追うときには「落ちた直前のフェーズ」が唯一の
		// 手掛かりになるので、dev には既定で残す。開けなくても黙って続ける（付随機能）。
		//
		// 有効／無効の判断がここにあるのは、**BuildConfig.h（VW_DEV_BUILD）を見られるのが
		// SDK 側だけ**だから。core/Trace 自身はビルド種別を知らない。
		void OpenImportTrace(const std::string& ifcPath)
		{
#ifndef VW_DEV_BUILD
			// 環境変数の読み取りは core/Trace が持つ（getenv の作法をあちこちに書かない）。
			if (!core::trace::envFlag("HOMESKZ_IFC_TRACE"))
				return;
#endif
			if (!core::trace::open(core::trace::defaultLogPath("HomeskzIfcImport.log")))
				return;
			core::trace::log("import: " + ifcPath);
		}

		// インポート本体（ファイル選択の後）。解析 → 描画を通し、完了ダイアログの本文を返す。
		// 例外はここでは受けず、呼び出し元（DoInterface）が SDK コールバックの境界で 1 か所だけ
		// 受け止める。進捗ダイアログは RAII なので、途中で例外が出てもデストラクタが閉じる。
		std::string RunImport(const std::string& ifcPath)
		{
			OpenImportTrace(ifcPath);
			LogUndoState("start");

			// 進捗ダイアログを開く。両フェーズへ**同じ 1 つ**を渡し、解析→描画を通して
			// 見出しとバーを進める。描画は横架材・垂木を 1 本ずつ SDK で作るため数百回の
			// 呼び出しになり、これが無いと VectorWorks が固まったように見える
			// （draw/ProgressDialog.h「なぜ要るか」）。
			draw::ProgressDialog progress("ホームズ君 IFC インポート", FileNameOf(ifcPath));

			// Phase 1（SDK 非依存）: IFC を解析して命令セット（Document）を組み立てる。
			// 読み込み失敗も例外を漏らさず空の Document として返る（1 要素の欠損で止めない）。
			const core::Document document = parse::buildDocument(ifcPath, progress);
			LogUndoState("afterParse");

			// Phase 2（SDK 依存）: 命令セットを検証してから各要素を描く。検証を通らなければ
			// valid=false で何も描かない。途中でキャンセルされたら、その時点までを描いて
			// cancelled=true で戻る。
			// 図面変更は draw 側が自前の undo イベント（draw::ImportUndoScope）で包む。
			// executeDocument から戻った時点でイベントは閉じているので、building=no に
			// なっているはず——そこが崩れると「取り消し」で図面が壊れるので、ログで見る。
			const draw::DrawCounts drawn = draw::executeDocument(document, progress);
			LogUndoState("afterDraw");

			// 完了ダイアログの前に進捗ダイアログを閉じる（2 枚重ねない）。
			progress.close();

			// 本文の組み立ては**無 SDK 側**（parse/Summary）が持つ。要素が増えても
			// ここは変わらない（docs/DEV-NOTES.md M15「完了文言の集約」）。
			// 診断ログが有効ならその場所も本文へ載せる（一時ディレクトリは macOS では
			// /var/folders/… という当てられない場所なので、毎回ここで案内する）。
			const std::string body =
				parse::formatImportResult(document, drawn, core::trace::path());
			core::trace::log("done");
			core::trace::close();
			return body;
		}

#ifdef VW_DEV_BUILD
		// 【一時計装 ── 役目を終えたら消す】グラフィック凡例を選んだ状態でこのコマンドを
		// 実行したときだけ働く。**文書内のグラフィック凡例すべて**（パラメトリック
		// レコードの全欄・補助オブジェクトのタグ付きデータ）をファイルへ書き出し、
		// **インポートはせずに終わる**。凡例を選んでいなければ空振りして通常のインポートへ
		// 進む（draw/AuxProbe.h）。
		//
		// 書き出し先の決め方は診断ログと同じ（core/Trace の defaultLogPath。図面や IFC の
		// 隣には置かない）。中身は複数行の素の文章なので、行ごとに経過時間が付く
		// core::trace::log ではなく素直に ofstream で書く。**ダイアログには場所だけ出す**
		// ——ダンプは数千行になるので本文には収まらない。
		bool RunLegendAuxProbe()
		{
			const std::string dump = draw::probeSelectedLegendAuxData();
			if (dump.empty())
				return false;

			const std::string path = core::trace::defaultLogPath("HomeskzIfcAuxProbe.txt");
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if (stream)
				stream << dump;
			stream.close();

			gSDK->AlertInform("グラフィック凡例のダンプを書き出しました。", path.c_str(),
							  false /* not a minor alert: show a modal dialog */);
			return true;
		}
#endif

		// 例外で中断したときの後始末と本文づくり。診断ログに例外を書き残してから閉じ、
		// ダイアログ本文（無 SDK 側が組み立てる）にログの場所を添えて返す。
		std::string ReportImportError(const std::string& detail)
		{
			core::trace::log("error: " + (detail.empty() ? std::string("(unknown)") : detail));
			core::trace::close();
			// パスは close() の後も残る（core/Trace の path()）ので、そのまま案内に使える。
			return parse::formatImportError(detail, core::trace::path());
		}
	} // namespace
} // namespace HomeskzIfcImport

// ---------------------------------------------------------------------------
// 文書アクティブ時のみ有効化（＝文書が無ければグレーアウト）は menuDef() の Needs =
// EMenuEnableFlags::DocIsActive で宣言的に行う（上のコメント参照）。このコマンドは追加の動的
// な有効／無効判定を持たないので GetItemEnabled() は override せず、基底の VWMenu_EventSink::
// GetItemEnabled()（常に true）に委ねる。
void CImportIfcMenu_EventSink::DoInterface()
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

#ifdef VW_DEV_BUILD
	// 0.【一時計装 ── 役目を終えたら消す】グラフィック凡例を選んだ状態で実行したときは、
	//    インポートの代わりに凡例のダンプを書き出して終わる（上の RunLegendAuxProbe。
	//    凡例を選んでいなければ素通りする）。
	if (RunLegendAuxProbe())
		return;
#endif

	// 1. ネイティブの「開く」ダイアログで IFC を 1 つ選ばせる。キャンセルなら静かに終える。
	std::string ifcPath;
	if (!ChooseIfcFile(ifcPath))
		return;

	// 2. インポート本体。**例外を SDK コールバックの外へ漏らさない**（CLAUDE.md
	// 「エラーハンドリング・所有権」）。ネイティブプラグインの未捕捉例外は **VectorWorks
	// 本体を巻き込んで落とす**ので、フェーズ境界であるここで必ず受け止め、ユーザーへは
	// 1 通のダイアログとして見せる。1 要素の欠損で全体を止めない寛容さ（parse / draw の中で
	// continue する）は従来どおりで、ここへ来るのは「そこでも吸収できなかった異常」だけ。
	std::string body;
	try
	{
		body = RunImport(ifcPath);
	}
	catch (const std::exception& error)
	{
		body = ReportImportError(error.what());
	}
	catch (...)
	{
		// std::exception ですらないもの（サードパーティや処理系が投げるもの）。
		// 何が起きたかは分からないが、**それでも VW を落とさない**ことが最優先。
		body = ReportImportError("");
	}

	// 3. 結果をダイアログ表示。advice 行にファイルパス。false = 最小アラートでなくモーダルに
	//    して本文と advice を両方見せる（Updater と同じ作法）。TXString は UTF-8 の
	//    const char* から暗黙変換される。
	gSDK->AlertInform(body.c_str(), ifcPath.c_str(),
					  false /* not a minor alert: show a modal dialog */);
}
