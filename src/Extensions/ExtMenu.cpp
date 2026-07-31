//
//	ExtMenu.cpp
//
//	Implementation of the plug-in's menu command.
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtMenu.h"

// Phase 1（IFC 解析）と Phase 2（VW 描画）の入口。SDK 非依存の core/parse
// ライブラリ（HomeskzIfcCore）に解析があり、SDK 依存の draw/ が描画する。この
// メニューコマンドが両フェーズをオーケストレーションする（Python 版 run() が
// ifc.build_document → vw.execute_document を呼ぶのと同じ立ち位置）。ヘッダは
// いずれも core::Document までしか参照せず、SDK / STEP を相互に引き込まない。
#include "parse/BuildDocument.h"
#include "draw/ExecuteDocument.h"

// ファイル選択ダイアログ（VCOM）。ネイティブの「開く」ダイアログを出し、選ばれた
// ファイルの絶対パスを IFileIdentifier 経由で受け取る。
#include "Interfaces/VectorWorks/Filing/IFileChooserDialog.h"
#include "Interfaces/VectorWorks/Filing/IFileIdentifier.h"

#include <cstddef>
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

// ---------------------------------------------------------------------------
// 文書アクティブ時のみ有効化（＝文書が無ければグレーアウト）は menuDef() の
// Needs = EMenuEnableFlags::DocIsActive で宣言的に行う（上のコメント参照）。
// このコマンドは追加の動的な有効／無効判定を持たないので GetItemEnabled() は
// override せず、基底の VWMenu_EventSink::GetItemEnabled()（常に true）に委ねる。
void CImportIfcMenu_EventSink::DoInterface()
{
	// Note: the dev-build picker is NOT run here. It runs once at Vectorworks
	// start-up (see plugin_module_main -> RunDevStartupCheck) because a compiled
	// plug-in can only be swapped in at load time, and because the command may be
	// re-invoked programmatically — a picker on the command path would then pop up
	// repeatedly. So the command just does its work below, every time it runs.

	// 縦切りの通し処理: ファイルを選ぶ → parse（Phase 1）で IFC を Document へ →
	// draw（Phase 2）でストーリ・通り芯・床を VectorWorks へ描く → 件数をダイアログに
	// 出す。要素が増えても入口はこの形のまま（各要素の追加は Document と draw 側で行う。
	// ROADMAP.md）。Python 版 run() が ifc.build_document → vw.execute_document
	// を呼ぶのと同じ入口で、ここが両フェーズのオーケストレーションを担う。

	// 1. ネイティブの「開く」ダイアログで IFC を 1 つ選ばせる。キャンセルなら静かに終える。
	std::string ifcPath;
	if (!ChooseIfcFile(ifcPath))
		return;

	// 2. Phase 1（SDK 非依存）: IFC を解析して命令セット（Document）を組み立てる。
	//    読み込み失敗も例外を漏らさず空の Document として返る（1 要素の欠損で止めない）。
	const core::Document document = parse::buildDocument(ifcPath);

	// 3. Phase 2（SDK 依存）: 命令セットを検証してからストーリ・通り芯・床・屋根組（垂木・
	//    野地板）を描く。検証を通らなければ valid=false で何も描かない。
	const draw::DrawCounts drawn = draw::executeDocument(document);

	// 4. 結果をダイアログ表示。本文には**実際に描けた数**を出し、命令はあるのに描けなかった
	//    要素は「N 件中 0 件」の形で分かるようにする（配置先レイヤが無い・オブジェクトを
	//    作れない等の描画側の問題を、ローカル確認で解析側と切り分けられるようにするため）。
	//    advice 行にファイルパス。false = 最小アラートでなくモーダルにして本文と advice を
	//    両方見せる（Updater と同じ作法）。TXString は UTF-8 の const char* から暗黙変換される。
	//
	// 「描けた数 / 命令数」を "3"（一致）または "0/12"（不一致）の形に整える小ヘルパー。
	const auto formatCount = [](std::size_t placed, std::size_t commands)
	{
		if (placed == commands)
			return std::to_string(placed);
		return std::to_string(placed) + "/" + std::to_string(commands);
	};

	const std::size_t commandCount = document.stories.size() + document.grids.size() +
									 document.floors.size() + document.rafters.size() +
									 document.roofs.size();
	std::string body;
	if (!drawn.valid)
		body = "命令セットの検証に通らなかったため、何も描きませんでした。";
	else if (commandCount == 0)
		body = "ストーリ・通り芯・床・屋根組が見つかりませんでした。";
	else
		body = "ストーリ " + formatCount(drawn.stories, document.stories.size()) + " 層・通り芯 " +
			   formatCount(drawn.grids, document.grids.size()) + " 本・床 " +
			   formatCount(drawn.floors, document.floors.size()) + " 枚・垂木 " +
			   formatCount(drawn.rafters, document.rafters.size()) + " 本・野地板 " +
			   formatCount(drawn.roofs, document.roofs.size()) + " 枚を描きました。";
	gSDK->AlertInform(body.c_str(), ifcPath.c_str(),
					  false /* not a minor alert: show a modal dialog */);
}
