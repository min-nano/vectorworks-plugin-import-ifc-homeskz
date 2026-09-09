//
//	draw/ImportCommand.cpp
//
//	本番の取り込みコマンドの実装（意図は draw/ImportCommand.h 参照）。縦切りの通し処理:
//	ファイルを選ぶ → 設定を決める → 取り込む（draw/ImportRun）→ 結果をダイアログに出す。
//	要素が増えても入口はこの形のまま（各要素の追加は Document と draw 側で行う）。
//
//	**ここには往復（実機フィードバック）の分岐が 1 つも無い**——M25 でそれを dev だけの
//	テストコマンド（draw/Feedback.h の runTestRound）へ出したため。両者が共有するのは
//	絵を作るところ（draw/ImportRun.h）だけである。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "draw/ImportCommand.h"

#include "core/ImportOptions.h"
#include "core/Trace.h"
#include "draw/ImportRun.h"
#include "draw/ResultDialog.h"
#include "draw/SettingsDialog.h"

#include <string>

namespace HomeskzIfcImport::draw
{
	// -------------------------------------------------------------------
	// メニューコマンドの本体（draw/ImportCommand.h）。
	void runImportCommand()
	{
		// Note: the update check is NOT run here — it happens in the SHELL, before
		// the payload is even acquired (src/Extensions/ExtMenu.cpp). That ordering is
		// what lets a freshly installed payload take effect on THIS very import: by
		// the time this function runs, the payload is already loaded and cannot be
		// swapped. So the command just does its work below, every time it runs.

		// 1. ネイティブの「開く」ダイアログで IFC を 1 つ選ばせる。キャンセルなら静かに終える。
		std::string ifcPath;
		if (!chooseIfcFile(ifcPath))
			return;

		// 2. 取り込みの設定（配置するシンボルの対応）を決める。キャンセルなら静かに終える
		//    ——ファイルは選んだが取り込みたくない、という意思表示なので何も描かない。
		//    ダイアログを組めなかったときは**既定の対応でそのまま進む**（設定を出せない
		//    ことを理由に取り込み自体を落とさない。draw/SettingsDialog.h）。
		core::ImportOptions options;
		std::string settingsNote;
		const draw::SettingsOutcome settings = draw::showImportSettings(options, &settingsNote);
		if (settings == draw::SettingsOutcome::Cancelled)
			return;

		// 3. 取り込み本体。例外は中で受け止められ、failed=true と説明つきの body で返る
		//    （draw/ImportRun.h）。
		const ImportRound round =
			runImportRound(ifcPath, options, settings == draw::SettingsOutcome::Accepted,
						   settingsNote, /*prologue*/ std::string());

		// 4. 結果をダイアログ表示。本文は短く、**診断ログは折り畳んだテキスト欄**として同じ
		//    ダイアログに載せる（draw/ResultDialog.h。ふだんは開かず、不具合の報告のときに
		//    開いて丸ごとコピーする）。
		if (!draw::showImportResult("ホームズ君 IFC 取り込み", round.body, core::trace::text()))
		{
			// ダイアログを組めなかったときの逃げ道。結果を伝えられないまま黙って終わるのが
			// 最悪なので、素のアラートへ落とす（advice 行にファイルパス。false = 最小アラート
			// でなくモーダル）。TXString は UTF-8 の const char* から暗黙変換される。
			gSDK->AlertInform(round.body.c_str(), ifcPath.c_str(),
							  false /* not a minor alert: show a modal dialog */);
		}
	}
} // namespace HomeskzIfcImport::draw
