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
#include "FeedbackLoopHost.h"
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
		// のが筋である（CLAUDE.md「殻と本体」）。
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
	// **往復の最中だけは尋ねない。** 回り出した次の回は `Auto`（尋ねず・報せず入れる）
	// ——その人は「直したから、もう一度走らせてほしい」と言われて押しているので、そこへ
	// 確認を挟むのは、この往復が無くそうとしている手間そのものである。**一度きり**で、
	// 次の回は本体がまたそう言ってきたときだけ（ExtTestMenu.h「更新の確認」）。
	//
	// **殻に置くのは、本体が入れ替わっても残ってほしいから**——ここが消えると、入れ替えた
	// 次の周でまた尋ねることになる。Vectorworks を閉じれば消えてよい類の覚えなので、
	// ファイルには落とさない。
	static bool sAutoUpdateNextTest = false;

	// **押した直後にパレットを開く。** 1 周の終わりに開いていたら、このコマンドと本番の
	// 取り込みが**見分けられなかった**——どちらもダイアログが出て 1 分以上黙るので、押した
	// 人には同じに見える（実機の指摘。ExtTestMenu.h「押した直後に開く」）。**更新の確認
	// より前**に置くのは、そこでも入れ替えのダイアログが出て時間がかかるからで、その間も
	// 「これは実機テストだ」と分かっていてほしい。この時点ではまだ何も投稿できていない
	// ので、見え方は「実行しています…」にしておく。
	//
	// **走っている間は駆動を止める**（同じ番人が下まで生きる）。取り込みの最中は進捗
	// ダイアログの DoYield でパレットの JS タイマーが動きうる——素通しすると、駆動が
	// 2 周目を始めようとして本体を降ろしにいき、降ろせずに往復を止めてしまう
	// （src/FeedbackLoop.h）。
	BeginFeedbackRound();
	const FeedbackLoopBusyScope busy;

	// **本体を確保する前に置くことに意味がある。** ここで新しい本体が入れば、下の
	// PayloadUse がそれを読み直すので、**この回からもう新しいコードが動く**
	// （src/PayloadSession.h）。
	//
	// NOLINTBEGIN(bugprone-empty-catch): 黙って諦めるのが**この場所では正しい**振る舞い
	// （オフラインのときに無言なのと同じ扱い）。
	const bool autoUpdate = sAutoUpdateNextTest;
	sAutoUpdateNextTest = false;
	bool proceed = true;
	try
	{
		proceed = CheckForUpdates(autoUpdate ? UpdateCheckKind::Auto : UpdateCheckKind::Silent);
	}
	catch (...)
	{
	}
	// NOLINTEND(bugprone-empty-catch)

	// **尋ねずに入れたのに効かせられなかったときは走らない。** 殻まで変わった・入れられ
	// なかった、のどちらかで、更新の側が理由を出し終えている（src/UpdaterFlow.cpp）。
	// 古い本体のまま 1 分以上かけて取り込み、前の周と同じ結果をもう一度 PR へ投げても
	// 仕方がない。
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
	bool active = false;
	const bool called = use->runTest(/*allowDialogs*/ true, active, error);
	if (!called)
	{
		gSDK->AlertInform("実機テストを開始できませんでした。", error.c_str(), false);
		return;
	}
	sAutoUpdateNextTest = active;

	// **往復が回っているなら、以後はパレットが続きを回す**（M24。src/FeedbackLoopHost.h）。
	// 新しいビルドが出るたびにパレットが入れて取り込んで投稿し、Claude の合図か人の
	// 「往復を止める」で止まる。
	//
	// 回っていない（人が「送らない」を選んだ・宛先が分からなかった・更新を入れられ
	// なかった）ときも、開いたパレットはそのままにする——**閉じるのは人の意思**で、
	// パレット自身に「止めて閉じる」がある（Extensions/ExtFeedbackPalette.h。**隠すだけの
	// 口は無い**——隠れたページの時計は止まらないので、往復を残して隠すと止める口が
	// 無くなる）。
	//
	// **「実行しています…」を明示的に消しに行かない。** 上の番人が生きている間 Tick は
	// 素通しされるので、ここで呼んでも書き換わらない。番人が外れれば JS のタイマーが
	// 数秒で実態（「往復は回っていません」等）を出す——自分で直るものを二重に直さない。
	if (active)
		ArmFeedbackLoop();
}
