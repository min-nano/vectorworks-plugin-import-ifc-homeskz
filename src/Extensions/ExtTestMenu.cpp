//
//	Extensions/ExtTestMenu.cpp
//
//	「実機テストを実行…」コマンドの登録と取り次ぎ（意図は ExtTestMenu.h 参照）。中身は
//	本体側の draw::runTestRound（src/draw/Feedback.h）。**ここに実処理は 1 行も置かない**
//	（CLAUDE.md「殻と本体」）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtTestMenu.h"
#include "PayloadSession.h"
#include "Updater.h"
#include "UpdaterHost.h"

#include <string>

using namespace HomeskzIfcImport;

namespace HomeskzIfcImport
{
	namespace
	{
		// メニュー項目の宣言。カテゴリは他のコマンドと**同じ "category"**（＝プラグイン名）
		// を引く——このプラグインのコマンドはワークスペースの中で 1 か所にまとまっている
		// のが適切である（CLAUDE.md「殻と本体」）。
		//
		// 関数ローカル static で持つ理由は他のコマンドと同じ（EMenuEnableFlags は SDK の
		// 別 TU にある非ローカル static なので、名前空間スコープ変数の初期化子で参照すると
		// 静的初期化順序に依存する。Extensions/ExtMenu.cpp）。
		const SMenuDef& menuDef()
		{
			static const SMenuDef def = {/*Needs*/ EMenuEnableFlags::DocIsActive,
										 /*NeedsNot*/ EMenuEnableFlags::None,
										 /*Title*/ {PLUGIN_VWR_ID, "testTitle"},
										 /*Category*/ {PLUGIN_VWR_ID, "category"},
										 /*HelpText*/ {PLUGIN_VWR_ID, "testHelp"},
										 /*VersionCreated*/ 31,
										 /*VersionModified*/ 0,
										 /*VersionRetired*/ 0,
										 /*OverrideHelpID*/ ""};
			return def;
		}
	} // namespace
} // namespace HomeskzIfcImport

// 登録されるのは dev だけだが、UUID とユニバーサル名の綴りは他の拡張と揃える
// （安定版もこのクラスを持つ。どこにも登録しないだけ）。
//
// NOLINTBEGIN(misc-const-correctness)
#ifdef VW_DEV_BUILD
// UUID: 6f2c1d84-3b95-4a72-9e10-5c73a8d4b0e6  (dev build)
IMPLEMENT_VWMenuExtension(
	/*Extension class*/ CExtMenuTest,
	/*Event sink*/ CTestMenu_EventSink,
	/*Universal name*/ PLUGIN_TEST_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0x6f2c1d84, 0x3b95, 0x4a72, 0x9e, 0x10, 0x5c, 0x73, 0xa8, 0xd4, 0xb0, 0xe6);
#else
// UUID: 2a5e90c7-71d6-4f38-bb42-0d18e6c5af39  (stable build)
IMPLEMENT_VWMenuExtension(
	/*Extension class*/ CExtMenuTest,
	/*Event sink*/ CTestMenu_EventSink,
	/*Universal name*/ PLUGIN_TEST_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0x2a5e90c7, 0x71d6, 0x4f38, 0xbb, 0x42, 0x0d, 0x18, 0xe6, 0xc5, 0xaf, 0x39);
#endif
// NOLINTEND(misc-const-correctness)

// ---------------------------------------------------------------------------
CExtMenuTest::CExtMenuTest(CallBackPtr cbp) : VWExtensionMenu(cbp, menuDef()) {}

CExtMenuTest::~CExtMenuTest() = default;

// ---------------------------------------------------------------------------
CTestMenu_EventSink::CTestMenu_EventSink(IVWUnknown* parent) : VWMenu_EventSink(parent) {}

CTestMenu_EventSink::~CTestMenu_EventSink() = default;

// ---------------------------------------------------------------------------
void CTestMenu_EventSink::DoInterface()
{
	// **本体を確保する前に更新を確認する。** ここで新しい本体がインストールされれば、下の
	// PayloadUse がそれを再読み込みするので、**この回からもう新しいコードが動く**
	// （src/PayloadSession.h）。
	//
	// NOLINTBEGIN(bugprone-empty-catch): 何も表示せずに中断するのが**この場所では正しい**
	// 振る舞い（オフラインのときに何も表示しないのと同じ扱い）。
	bool proceed = true;
	try
	{
		proceed = CheckForUpdates(UpdateCheckKind::Silent);
	}
	catch (...)
	{
	}
	// NOLINTEND(bugprone-empty-catch)

	// インストールすると答えたのにインストールできなかった。更新の側が理由を表示し終えている。
	if (!proceed)
		return;

	const PayloadUse use;
	if (!use.ok())
	{
		gSDK->AlertInform("プラグインの本体を読み込めませんでした。", use.error().c_str(),
						  false /* not a minor alert: show a modal dialog */);
		return;
	}

	// 例外は本体側が境界の手前で受け止める（src/payload/PayloadMain.cpp）。ここへ返るのは
	// 「そもそも呼べなかった」ときだけ。
	std::string error;
	if (!use->runTest(/*allowDialogs*/ true, error))
		gSDK->AlertInform("実機テストを開始できませんでした。", error.c_str(), false);
}
