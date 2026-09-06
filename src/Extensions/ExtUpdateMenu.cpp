//
//	Extensions/ExtUpdateMenu.cpp
//
//	「アップデータを確認」コマンドの登録と実行。中身は src/Updater.h（＝殻に残る唯一の
//	実処理。ExtUpdateMenu.h 冒頭「登録は殻に、処理は…も殻に」）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtUpdateMenu.h"
#include "Updater.h"
#include "UpdaterHost.h"

using namespace HomeskzIfcImport;

namespace HomeskzIfcImport
{
	namespace
	{
		// Description of the menu command. The SResString entries ({resource,
		// identifier}) point at strings in the plug-in's .vwr resource file;
		// PLUGIN_VWR_ID differs between the stable and dev builds (BuildConfig.h)
		// so each shows its own name（「…(みんなの構造設計支援)」/「…Dev)」）。
		//
		// カテゴリは取り込みコマンドと**同じ "category"**（＝プラグイン名）を引く。
		// このプラグインのコマンドはワークスペースの中で 1 か所にまとまっているのが
		// 筋で、同じ文字列を .vwr へ 2 度書く理由も無い（Extensions/ExtMenu.cpp）。
		//
		// Needs = None: **文書が開いていなくても有効**。取り込みコマンドは描画先が要る
		// ので DocIsActive を宣言しているが（Extensions/ExtMenu.cpp）、更新の確認に
		// 図面は要らない。むしろ「取り込む前に新しくしておく」ために、文書を開く前に
		// 押せなければ困る。
		//
		// 関数ローカル static で持つ理由は取り込みコマンドと同じ（EMenuEnableFlags は
		// SDK の別 TU にある非ローカル static なので、名前空間スコープ変数の初期化子で
		// 参照すると静的初期化順序に依存する）。
		const SMenuDef& menuDef()
		{
			static const SMenuDef def = {/*Needs*/ EMenuEnableFlags::None,
										 /*NeedsNot*/ EMenuEnableFlags::None,
										 /*Title*/ {PLUGIN_VWR_ID, "updateTitle"},
										 /*Category*/ {PLUGIN_VWR_ID, "category"},
										 /*HelpText*/ {PLUGIN_VWR_ID, "updateHelp"},
										 /*VersionCreated*/ 31,
										 /*VersionModified*/ 0,
										 /*VersionRetired*/ 0,
										 /*OverrideHelpID*/ ""};
			return def;
		}
	} // namespace
} // namespace HomeskzIfcImport

// 安定版と開発版は同時に読み込まれうるので、UUID とユニバーサル名は別にする。
//
// NOLINTBEGIN(misc-const-correctness)
#ifdef VW_DEV_BUILD
// UUID: e3be63e5-0d05-42c2-a11f-99fe1a458af0  (dev build)
IMPLEMENT_VWMenuExtension(
	/*Extension class*/ CExtMenuCheckUpdate,
	/*Event sink*/ CCheckUpdateMenu_EventSink,
	/*Universal name*/ PLUGIN_UPDATE_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0xe3be63e5, 0x0d05, 0x42c2, 0xa1, 0x1f, 0x99, 0xfe, 0x1a, 0x45, 0x8a, 0xf0);
#else
// UUID: 034375de-a03d-41f2-9055-c211aa091d6d  (stable build)
IMPLEMENT_VWMenuExtension(
	/*Extension class*/ CExtMenuCheckUpdate,
	/*Event sink*/ CCheckUpdateMenu_EventSink,
	/*Universal name*/ PLUGIN_UPDATE_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0x034375de, 0xa03d, 0x41f2, 0x90, 0x55, 0xc2, 0x11, 0xaa, 0x09, 0x1d, 0x6d);
#endif
// NOLINTEND(misc-const-correctness)

// ---------------------------------------------------------------------------
CExtMenuCheckUpdate::CExtMenuCheckUpdate(CallBackPtr cbp) : VWExtensionMenu(cbp, menuDef()) {}

CExtMenuCheckUpdate::~CExtMenuCheckUpdate() = default;

// ---------------------------------------------------------------------------
CCheckUpdateMenu_EventSink::CCheckUpdateMenu_EventSink(IVWUnknown* parent)
	: VWMenu_EventSink(parent)
{
}

CCheckUpdateMenu_EventSink::~CCheckUpdateMenu_EventSink() = default;

// ---------------------------------------------------------------------------
void CCheckUpdateMenu_EventSink::DoInterface()
{
	// **例外を SDK のコールバックへ漏らさない**（CLAUDE.md「エラーハンドリング」）。
	// ここは起動時と違って報告先があるので、握り潰さずに一言出す。
	try
	{
		CheckForUpdates(UpdateCheckKind::Manual);
	}
	catch (...)
	{
		gSDK->AlertInform("更新を確認できませんでした。",
						  "アップデータの実行中に問題が起きました。\n"
						  "しばらく待ってからもう一度お試しください。",
						  false /* not a minor alert: show a modal dialog */);
	}
}
