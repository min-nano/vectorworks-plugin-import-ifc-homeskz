//
//	ExtMenu.cpp
//
//	Implementation of the plug-in's menu command.
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtMenu.h"

// Phase 1（IFC 解析）の入口。SDK 非依存の core/parse ライブラリ（HomeskzIfcCore）に
// 実装があり、このメニューコマンドがオーケストレーションする（Python 版 run() が
// ifc.build_document を呼ぶのと同じ立ち位置）。ヘッダは SDK を引き込まない。
#include "parse/Summary.h"

// ファイル選択ダイアログ（VCOM）。ネイティブの「開く」ダイアログを出し、選ばれた
// ファイルの絶対パスを IFileIdentifier 経由で受け取る。
#include "Interfaces/VectorWorks/Filing/IFileChooserDialog.h"
#include "Interfaces/VectorWorks/Filing/IFileIdentifier.h"

#include <string>

using namespace HomeskzIfcImport;
using namespace VectorWorks::Filing;

namespace HomeskzIfcImport
{
	namespace
	{
		// Description of the menu command. The SResString entries ({resource,
		// identifier}) point at strings in the plug-in's .vwr resource file; a
		// resource file is optional for a build to succeed. EMenuEnableFlags{}
		// means "no special selection requirements to enable the command".
		// PLUGIN_VWR_ID differs between the stable and dev builds (see
		// BuildConfig.h) so each loads its own strings. File-local (anonymous
		// namespace) rather than `static`.
		SMenuDef gMenuDef = {
			/*Needs*/ EMenuEnableFlags{},
			/*NeedsNot*/ EMenuEnableFlags{},
			/*Title*/ {PLUGIN_VWR_ID, "title"},
			/*Category*/ {PLUGIN_VWR_ID, "category"},
			/*HelpText*/ {PLUGIN_VWR_ID, "help"},
			/*VersionCreated*/ 31,
			/*VersionModified*/ 0,
			/*VersionRetired*/ 0,
			/*OverrideHelpID*/ ""};

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
			IFileChooserDialogPtr dialog(IID_FileChooserDialog);
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
CExtMenuImportIfc::CExtMenuImportIfc(CallBackPtr cbp) : VWExtensionMenu(cbp, gMenuDef) {}

CExtMenuImportIfc::~CExtMenuImportIfc() = default;

// ---------------------------------------------------------------------------
CImportIfcMenu_EventSink::CImportIfcMenu_EventSink(IVWUnknown* parent) : VWMenu_EventSink(parent) {}

CImportIfcMenu_EventSink::~CImportIfcMenu_EventSink() = default;

void CImportIfcMenu_EventSink::DoInterface()
{
	// Note: the dev-build picker is NOT run here. It runs once at Vectorworks
	// start-up (see plugin_module_main -> RunDevStartupCheck) because a compiled
	// plug-in can only be swapped in at load time, and because the command may be
	// re-invoked programmatically — a picker on the command path would then pop up
	// repeatedly. So the command just does its work below, every time it runs.

	// M0 の縦切り: ファイルを選ぶ → parse（Phase 1）で IFC を読む → 主要要素の件数を
	// ダイアログに出す。まだ描画（Phase 2）はしない。パースが実際に動いている確証を
	// 件数で示すのが狙い（ROADMAP.md M0「ローカル確認」）。Python 版 run() が
	// ifc.build_document を呼ぶのと同じ入口で、ここがオーケストレーションを担う。

	// 1. ネイティブの「開く」ダイアログで IFC を 1 つ選ばせる。キャンセルなら静かに終える。
	std::string ifcPath;
	if (!ChooseIfcFile(ifcPath))
		return;

	// 2. Phase 1（SDK 非依存）で読み込み、主要エンティティ型の件数を数える。読み込み
	//    失敗も例外を漏らさず loaded=false のサマリとして返る（1 要素の欠損で止めない）。
	const parse::IfcSummary summary = parse::summarizeIfc(ifcPath);

	// 3. 件数を人が読めるテキストへ整形してダイアログ表示（整形は無 SDK でテスト済み）。
	//    本文に件数一覧、advice 行に選んだファイルのパスを出す。false = 最小アラートで
	//    なくモーダルダイアログにして、本文と advice を両方見せる（Updater と同じ作法）。
	//    TXString は UTF-8 の const char* から暗黙変換される（日本語を含めそのまま渡せる）。
	const std::string body = parse::formatSummary(summary);
	gSDK->AlertInform(body.c_str(), ifcPath.c_str(),
					  false /* not a minor alert: show a modal dialog */);
}
