//
//	draw/Feedback.h
//
//	**実機フィードバックの往復**（docs/DEV-NOTES.md M23）。開発版（dev）ビルドで取り込みを
//	走らせた結果を、そのビルドの元になった PR へ投稿する——この 1 周ぶんを受け持つ。
//
//	【1 周の形】
//	  1. **取り込みの前に**、送るかどうか・宛先・伏せ字を決める（planFeedbackRound）。
//	     2 周目以降は記憶から即座に決まるので**何も出ない**。
//	  2. 取り込みが走る（draw/ImportCommand）
//	  3. 終わったら**黙って投稿する**（postFeedbackRound）。結果ダイアログも出さない。
//	  4. 直った版が push され、CI が dev プレリリースを出したら、**人が取り込みをもう一度
//	     実行する**。殻が**尋ねずに**その版を入れ（UpdateCheckKind::Auto。
//	     src/UpdaterFlow.cpp）、本体を読み直して 1 へ戻る。**ファイル選択も設定
//	     ダイアログも出ない**——1 周目の選択を覚えてある（core/FeedbackSession）。
//
//	【続きの周は「新しいビルドが来たとき」だけ】記憶に残した sha と動いているビルドが同じ
//	なら、それは往復の続きではない（同じビルドで取り込み直しても同じ数字が並ぶだけ）。
//	**これが往復の終わり方でもある**——Claude が push をやめれば新しい dev ビルドは出ず、
//	続きの周はそれ以上走らない。だから「やめる」ボタンを持たない（M23 で一度置いて外した。
//	docs/DEV-NOTES.md「やめるボタンを置かずに済ませる」）。新しいビルドを待っているあいだに
//	取り込みを実行したら 1 周目のダイアログが出るので、そこで「送らない」を選べば記憶は消える。
//
//	【取り込み前へ戻すのは人の手仕事】2 周目以降は同じ文書へもう一度描くので、前の周を
//	「取り消し」で戻していないと図が二重になる。**プログラムから戻す手立ては確かめていない**
//	（SDK の調査はリファレンス側で行う。CLAUDE.md。起票済み:
//	min-nano/vectorworks-developer-sdk-reference#23）。だからといって周ごとに確認を出さない
//	——押したかどうかは戻したかどうかではないので、**確認は嘘をつく**。頼むのは 1 周目の
//	ダイアログで 1 度だけにし、**実際にどうだったかは描画側の実測**（DrawCounts::undoPartial）
//	として PR コメントへ載せる（parse/Feedback.cpp）。読む側はそれを見る。
//
//	【取り込みのあとに人の操作を残さない】**決めることは全部、取り込みが始まる前に決める。**
//	取り込みは 1 分以上かかるので、終わったところに確認が待っていると、その人は席を離れ
//	られない——それでは「実行して放っておく」が成立しない（実機の指摘）。だから宛先も
//	伏せ字もトークンの登録も前に済ませ、終わったあとは投稿して黙る。
//
//	【所見は PR へ載せない】絵を見て気付いたことは**人が Claude とのチャットへ直接書く**。
//	所見を書くには結局その人が実機を見ている必要があり、見ているならチャットのほうが速く、
//	スクリーンショットも貼れる。プラグインが所見を訊く仕組みは持たない（M23 で一度作って
//	外した。docs/DEV-NOTES.md「所見はプラグインの仕事ではなかった」）。
//
//	【なぜ要るか】`draw/` の実描画は CI では検証できず、ローカルの VectorWorks でしか
//	確かめられない（CLAUDE.md「テスト方針」）。そのため 1 往復ごとに
//	「新しいビルドを入れる → 図面を戻す → ファイルを選ぶ → 設定を選ぶ → 取り込む →
//	ログを写して貼る」という手作業が挟まり、これが**実装そのものより時間を食っていた**。
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
	// フィードバックの往復が**そもそも使えるか**（dev ビルドで、殻がスクリプトを
	// 貸してくれているか）。使えないときは取り込みが従来どおり動くだけである。
	bool feedbackAvailable();

	// この取り込みが**フィードバックの往復として走る周**かどうかを決める材料。
	// 記憶が無ければ既定（＝手動の 1 周目）で返る。
	//
	// branch は**いま動いているビルドのブランチ**。記憶が別のブランチのものなら
	// 続きにはしない——別のブランチのビルドに入れ替わったなら、それは新しい往復の
	// 1 周目である。
	core::FeedbackSession loadFeedbackSession(const std::string& branch);

	// **取り込みの前に決めたこと。**
	struct FeedbackPlan
	{
		bool send = false;			   // 取り込みが終わったら投稿するか
		core::FeedbackSession session; // 送るときの宛先と記憶（呼び出し側が ifcPath /
									   // options を埋めてから postFeedbackRound へ渡す）
	};

	// **取り込みの前に 1 度だけ尋ねる。** continuing（往復の続き）なら何も出さず、記憶の
	// まま送ると決める。1 周目は宛先 PR と伏せ字を尋ね、送ると決まればトークンもここで
	// 確保する（未登録なら 1 度だけ貼り付けを求める）——**取り込みが終わったあとに人の
	// 操作を残さない**ため、尋ねることは全部ここで尋ね切る。
	FeedbackPlan planFeedbackRound(const core::FeedbackSession& remembered,
								   const parse::BuildInfo& build, bool continuing);

	// 1 周ぶんの材料（draw/ImportCommand が詰める）。
	struct FeedbackInput
	{
		const core::Document* document = nullptr;
		const core::DrawCounts* counts = nullptr;
		parse::BuildInfo build; // 動いていたビルド
		std::string ifcPath;
		unsigned long long bytes = 0;
		double seconds = 0.0;
		std::string startedAt;
		std::string log; // 診断ログ全文
	};

	// **取り込みのあと、黙って投稿する。** 人の操作は 1 つも要らない。
	//
	// 戻り値は「**次にこのコマンドが走るときは、更新を尋ねずに入れてよいか**」——投稿
	// できたときだけ true になり、呼び出し側はそのまま殻へ返す（src/Extensions/ExtMenu.cpp）。
	//
	// error は**投稿できなかった理由**（空なら成功）。呼び出し側はこれをいつもの結果
	// ダイアログへ添える——ここでアラートを出すと、無操作で終わるはずの取り込みの最後に
	// ボタンが 1 つ増えてしまう。
	bool postFeedbackRound(const FeedbackPlan& plan, const FeedbackInput& input,
						   std::string& error);
} // namespace HomeskzIfcImport::draw
