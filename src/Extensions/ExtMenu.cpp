//
//	ExtMenu.cpp
//
//	Implementation of the plug-in's menu command.
//
//	**ここに残るのは登録と取り次ぎだけ。** 取り込みの実処理（ファイル選択 → 解析 → 描画
//	→ 結果ダイアログ）は本体（ペイロード）側の draw::runImportCommand が持つ。こう割って
//	あるのは、**Vectorworks を再起動せずにプラグインを入れ替えられる**ようにするため——
//	Vectorworks が起動時に読み込むこのモジュール（＝殻）は滅多に変わらず、変わるのは本体の
//	ほうだけ、という形にしてある（src/PayloadAbi.h / src/PayloadSession.h）。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "Extensions/ExtMenu.h"
#include "PayloadAbi.h"
#include "PayloadSession.h"
#include "Updater.h"
#include "UpdaterHost.h"

#include <string>

using namespace HomeskzIfcImport;

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
// 文書アクティブ時のみ有効化（＝文書が無ければグレーアウト）は menuDef() の Needs =
// EMenuEnableFlags::DocIsActive で宣言的に行う（上のコメント参照）。このコマンドは追加の動的
// な有効／無効判定を持たないので GetItemEnabled() は override せず、基底の VWMenu_EventSink::
// GetItemEnabled()（常に true）に委ねる。
//
// **中身は本体（ペイロード）が持つ。** ここでするのは「本体を（必要なら読み直して）
// 確保し、呼ぶ」だけ。読み直しの判定は PayloadUse の構築時に行われるので、**アップデータ
// が新しい本体を置いていれば、この 1 回目の取り込みからもう新しいコードが動く**
// （Vectorworks の再起動は要らない。src/PayloadSession.h）。
void CImportIfcMenu_EventSink::DoInterface()
{
	// **本体が「もう 1 周」と言う限り呼び直す。** 実機フィードバックの往復
	// （draw/Feedback.h）で、修正版のビルドが出たときだけそうなる。
	//
	// **周と周のあいだに PayloadUse を壊すのが肝。** 本体を降ろせるのは、そのコードが
	// スタックに 1 つも無いとき——つまり入れ子の深さが 0 のとき——だけで、次の
	// PayloadUse がその判定と読み直しを行う（src/PayloadSession.h）。だから殻に要るのは
	// この「更新を確認して、呼び直す」だけで、往復の中身（何を投稿し、どのビルドを
	// 待つか）はすべて本体側にある——殻に実処理を書かない、というこの分割の決めごと
	// どおり（CLAUDE.md「アーキテクチャ: 殻と本体」）。
	//
	// 上限は歯止め。本体が壊れて常に「もう 1 周」を返しても、ここで必ず止まる。
	constexpr int kMaxRounds = 50;
	for (int round = 0; round < kMaxRounds; ++round)
	{
		// **取り込みの前に更新を確認する。** 起動時の自動確認をやめた代わりがここで
		// （src/Updater.h）、1 周目は更新があるときだけ尋ね、オフライン等は黙って
		// 取り込みへ進む（UpdateCheckKind::Silent）。
		//
		// **2 周目以降は尋ねない**（UpdateCheckKind::Auto）。そこにいるのは実機
		// フィードバックの往復で、本体が「新しいビルドが出た」と見届けて戻ってきた
		// 直後である——周ごとに「インストールしますか？」を挟むのは、この仕組みが
		// 無くそうとしている手間そのものになる。輪が止まるとき（殻まで変わった・
		// 入れられなかった）だけ false が返り、そこで打ち切る。
		//
		// **本体（ペイロード）を確保する前に置くことに意味がある。** ここで新しい本体が
		// 入れば、下の PayloadUse がそれを読み直すので、**この周からもう新しいコードが
		// 動く**（src/PayloadSession.h）。確保したあとでは、本体のコードがスタックに
		// 載っているぶん降ろせず、反映は次回に回る。
		//
		// 例外はここで止める——更新は取り込みの付随でしかなく、失敗しても取り込みは
		// 続けなければならない。
		//
		// NOLINTBEGIN(bugprone-empty-catch): 黙って諦めるのが**この場所では正しい**振る舞い
		// （オフラインのときに無言なのと同じ扱い）。握り潰しを禁じる規則をここだけ外す。
		bool keepGoing = true;
		try
		{
			keepGoing =
				CheckForUpdates(round == 0 ? UpdateCheckKind::Silent : UpdateCheckKind::Auto);
		}
		catch (...)
		{
		}
		// NOLINTEND(bugprone-empty-catch)

		// 1 周目は結末に関わらず取り込みへ進む（更新は付随でしかない）。2 周目以降で
		// false なら、新しい本体は載っていない——同じ結果をもう一度出しても仕方がない
		// ので、輪をここで止める（理由は更新の側が伝え終えている）。
		if (round > 0 && !keepGoing)
			return;

		bool again = false;
		{
			// 本体を確保する。**ここは唯一「読み込めなかった」をユーザーへ見せられる場所**
			// （PIO のリセットは黙って諦めるしかない——数百回出るダイアログに意味は無い）。
			const PayloadUse use;
			if (!use.ok())
			{
				gSDK->AlertInform("プラグインの本体を読み込めませんでした。", use.error().c_str(),
								  false /* not a minor alert: show a modal dialog */);
				return;
			}

			// 例外は本体側が境界の手前で受け止める（src/payload/PayloadMain.cpp）。ここへ
			// 返るのは「そもそも呼べなかった」ときだけ。
			std::string error;
			if (!use->runImport(again, error))
			{
				gSDK->AlertInform("取り込みを開始できませんでした。", error.c_str(), false);
				return;
			}
		} // ← ここで use が壊れ、深さが 0 に戻る（次の周で新しい本体が読み直される）

		if (!again)
			return;
	}
}
