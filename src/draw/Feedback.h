//
//	draw/Feedback.h
//
//	**実機フィードバックの往復**（docs/DEV-NOTES.md M23）。開発版（dev）ビルドで取り込みを
//	走らせた結果を、そのビルドの元になった PR へ投稿し、**修正版のビルドが出たら自動で
//	取り込み直す**——この 1 周ぶんを受け持つ。
//
//	【なぜ要るか】`draw/` の実描画は CI では検証できず、ローカルの VectorWorks でしか
//	確かめられない（CLAUDE.md「テスト方針」）。そのため 1 往復ごとに
//	「新しいビルドを入れる → 図面を戻す → ファイルを選ぶ → 設定を選ぶ → 取り込む →
//	ログを写して貼る」という手作業が挟まり、これが**実装そのものより時間を食っていた**。
//	**プラグイン自身にこれを回させれば、人がするのは「絵を見て一言書く」だけになる。**
//
//	【1 周の形】
//	  1. 取り込みが終わる（draw/ImportCommand）
//	  2. このモジュールがフィードバックのダイアログを出す
//	     ——結果の本文＋宛先（PR）＋「名前を伏せる」
//	  3. 本文（parse/Feedback が組む Markdown）を同梱スクリプトで PR へ投稿する
//	  4. **所見を訊く仕事を同梱スクリプトへ渡して、待たずに戻る**（下記）
//	  5. 直したものが push され、新しい dev ビルドが出たら、**人が取り込みをもう一度
//	     実行する**。殻が**尋ねずに**その版を入れ（UpdateCheckKind::Auto。
//	     src/UpdaterFlow.cpp）、本体を読み直して 1 へ戻る。**ファイル選択も設定
//	     ダイアログも出ない**——1 周目の選択を覚えてある（core/FeedbackSession）。
//
//	【所見は別プロセスのダイアログで訊く】**絵を見てから書くには Vectorworks を操作でき
//	なければならない。** ところが VW のレイアウトダイアログはモーダル前提で、開いている間は
//	図面を拡大することもレイヤを切り替えることもできない（モードレスにするには別の拡張種別
//	`IExtensionWebPalette` が要る——[SDK リファレンス「モードレス（非モーダル）なパレット」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Layout%20Dialogs.md)。
//	実機未確認）。
//
//	そこで**所見だけは OS 標準のダイアログを別プロセスで出す**（`scripts/vw-feedback.*` の
//	`ask-note`）。あちらは自分を起こし直して即座に返るので、プラグインは待たずに戻り、
//	Vectorworks は完全に操作できる状態になる。**戻ってしまう以上こちらへは返せない**ので、
//	尋ねてから投稿するところまでスクリプトの仕事になり、所見は**独立した 1 通**として
//	PR へ載る（docs/DEV-NOTES.md M23）。
//
//	【ビルドを待たない】以前はここで**新しいビルドが出るまで待って**いた。人の操作が
//	1 回も要らないという点では理想だったが、待つあいだ進捗ダイアログが Vectorworks を
//	止めてしまい、**その周の絵が見られない**（実機 round 3 の所見。docs/DEV-NOTES.md M23）。
//	絵を見るために回している往復でそれでは本末転倒なので、待つのをやめた。
//
//	代わりに残る人の操作は**メニューを 1 回押すこと**だけで、それ以外——どのビルドを
//	入れるか・何を取り込むか・どの設定で描くか——はすべて自動のままである。押した瞬間に
//	新しいビルドが尋ねられずに入るよう、**戻り値で殻へ「次は尋ねずに入れてよい」と伝える**
//	（src/Extensions/ExtMenu.cpp）。
//
//	【入れるのも殻】新しい本体を効かせるには載っている本体を降ろす必要があり、それが
//	できるのは**本体のコードがスタックに 1 つも無いとき**だけである（src/PayloadSession.h）。
//	だから入れ替えは必ず殻の側で起きる——おかげで**インストールの経路はこのリポジトリに
//	1 本しかない**（src/UpdaterFlow.cpp）。往復のために 2 本目を作らない。
//
//	【殻の道具を借りる】同梱スクリプトの実行は本体からは手が届かないので、境界越しに
//	借りたもの（draw/HostServices）を使う。借りられなければフィードバックの機能だけが
//	静かに無効になる（取り込み自体は何も変わらない）。
//
//	【SDK 依存】実装は PluginPrefix.h（VectorWorks SDK）と VWFC のダイアログを include する。
//	このヘッダは core/ までしか参照しない。
//

#pragma once

#include "core/Document.h"
#include "core/FeedbackSession.h"
#include "core/ImportOptions.h"
#include "parse/Summary.h"

#include <string>

namespace HomeskzIfcImport::draw
{
	// この取り込みが**フィードバックの往復として自動で走る周**かどうかを決める材料。
	// 記憶が無ければ既定（＝手動の 1 周目）で返る。
	//
	// branch は**いま動いているビルドのブランチ**。記憶が別のブランチのものなら
	// 自動継続はしない——別のブランチのビルドに入れ替わったなら、それは新しい往復の
	// 1 周目である。
	core::FeedbackSession loadFeedbackSession(const std::string& branch);

	// 1 周ぶんの材料（draw/ImportCommand が詰める）。
	struct FeedbackInput
	{
		const core::Document* document = nullptr;
		const core::DrawCounts* counts = nullptr;
		parse::BuildInfo build;		 // 動いていたビルド
		core::ImportOptions options; // この周で使った取り込み設定（記憶へ残す）
		std::string ifcPath;
		unsigned long long bytes = 0;
		double seconds = 0.0;
		std::string startedAt;
		std::string resultBody; // 完了ダイアログの短い本文（そのままダイアログに出す）
		std::string log; // 診断ログ全文
	};

	// **1 周の締めくくり。** 上記 2〜4 を行う。戻り値が true なら「**次にこのコマンドが
	// 走るときは、更新を尋ねずに入れてよい**」——投稿できた（＝往復の最中にいる）ときだけ
	// 立ち、呼び出し側はそのまま殻へ返す（src/Extensions/ExtMenu.cpp）。
	//
	// shownResult は「**結果を見せ切ったか**」。投稿できたときだけ true になる——
	// そのときは内訳もログも PR にあるので、フィードバックのダイアログで足りている。
	// **送れなかった／送らなかったときは false** で、呼び出し側はいつもの結果ダイアログを
	// 出す（そちらにしか「ログを表示」が無く、困ったときに貼るものへ手が届かなくなる）。
	bool runFeedbackRound(const FeedbackInput& input, bool& shownResult);

	// フィードバックの往復が**そもそも使えるか**（dev ビルドで、殻がスクリプトを
	// 貸してくれているか）。使えないときは呼び出し側が従来どおり結果ダイアログを出す。
	bool feedbackAvailable();
} // namespace HomeskzIfcImport::draw
