//
//	Extensions/ExtRegressionMenu.cpp
//
//	「回帰テストを実行…」コマンドの登録と取り次ぎ（意図は ExtRegressionMenu.h 参照）。
//	中身は本体側の draw::runRegressionCommand（src/draw/Regression.h）。**ここに実処理は
//	1 行も置かない**（CLAUDE.md「殻と本体」）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtRegressionMenu.h"
#include "FeedbackLoopHost.h"
#include "PayloadSession.h"

#include <string>

using namespace HomeskzIfcImport;

namespace HomeskzIfcImport
{
	namespace
	{
		// メニュー項目の宣言。カテゴリは他のコマンドと**同じ "category"**（＝プラグイン名）
		// を引く——このプラグインのコマンドはワークスペースの中で 1 か所にまとまっている
		// のが筋である（CLAUDE.md「殻と本体」）。
		//
		// 関数ローカル static で持つ理由は他のコマンドと同じ（EMenuEnableFlags は SDK の
		// 別 TU にある非ローカル static なので、名前空間スコープ変数の初期化子で参照すると
		// 静的初期化順序に依存する。Extensions/ExtMenu.cpp）。
		const SMenuDef& menuDef()
		{
			static const SMenuDef def = {/*Needs*/ EMenuEnableFlags::DocIsActive,
										 /*NeedsNot*/ EMenuEnableFlags::None,
										 /*Title*/ {PLUGIN_VWR_ID, "regressionTitle"},
										 /*Category*/ {PLUGIN_VWR_ID, "category"},
										 /*HelpText*/ {PLUGIN_VWR_ID, "regressionHelp"},
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
// UUID: ce54563a-004c-45e7-9093-417ed6f95188  (dev build)
IMPLEMENT_VWMenuExtension(
	/*Extension class*/ CExtMenuRegression,
	/*Event sink*/ CRegressionMenu_EventSink,
	/*Universal name*/ PLUGIN_REGRESSION_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0xce54563a, 0x004c, 0x45e7, 0x90, 0x93, 0x41, 0x7e, 0xd6, 0xf9, 0x51, 0x88);
#else
// UUID: a1a349ac-5720-4204-8630-b07fd8081c9a  (stable build)
IMPLEMENT_VWMenuExtension(
	/*Extension class*/ CExtMenuRegression,
	/*Event sink*/ CRegressionMenu_EventSink,
	/*Universal name*/ PLUGIN_REGRESSION_UNIVERSAL_NAME,
	/*Version*/ 1,
	/*UUID*/ 0xa1a349ac, 0x5720, 0x4204, 0x86, 0x30, 0xb0, 0x7f, 0xd8, 0x08, 0x1c, 0x9a);
#endif
// NOLINTEND(misc-const-correctness)

// ---------------------------------------------------------------------------
CExtMenuRegression::CExtMenuRegression(CallBackPtr cbp) : VWExtensionMenu(cbp, menuDef()) {}

CExtMenuRegression::~CExtMenuRegression() = default;

// ---------------------------------------------------------------------------
CRegressionMenu_EventSink::CRegressionMenu_EventSink(IVWUnknown* parent) : VWMenu_EventSink(parent)
{
}

CRegressionMenu_EventSink::~CRegressionMenu_EventSink() = default;

// ---------------------------------------------------------------------------
void CRegressionMenu_EventSink::DoInterface()
{
	// **走っている間は往復の駆動を止める**（ExtRegressionMenu.h「走っている間は」）。
	// 何十分も取り込み続けるあいだ、進捗ダイアログの DoYield でパレットの JS タイマーが
	// 動きうる——素通しすると駆動が周を始めようとして本体を降ろしにいく。
	const FeedbackLoopBusyScope busy;

	// **ここでは更新を確認しない**（ExtRegressionMenu.h「ここでは更新を確認しない」）。
	// 測りたいのは、いま入っているビルドである。
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
	if (!use->runRegression(error))
		gSDK->AlertInform("回帰テストを開始できませんでした。", error.c_str(), false);
}
