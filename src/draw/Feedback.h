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
//	     ——結果の本文＋**所見の記入欄**＋宛先（PR）＋「自動で続ける」「名前を伏せる」
//	  3. 本文（parse/Feedback が組む Markdown）を同梱スクリプトで PR へ投稿する
//	  4. 「自動で続ける」なら、**同じブランチの新しい dev ビルド**が出るまで待ち、
//	     出たら入れて（＝本体だけならホットリロード）「もう 1 周」を返す
//	  5. 殻が本体を持ち直して取り込みを呼び直す（src/Extensions/ExtMenu.cpp）。
//	     2 周目以降はファイル選択も設定ダイアログも出ない——1 周目の選択を覚えてある
//	     （core/FeedbackSession）。
//
//	【なぜ殻を経由して周回するのか】新しい本体を**その実行のまま**効かせるには、載っている
//	本体を降ろして読み直す必要があり、それができるのは**本体のコードがスタックに 1 つも
//	無いとき**だけである（src/PayloadSession.h）。だから「待って入れる」ところまでを本体が
//	行い、**入れ替えと呼び直しは殻に返してから**行う。
//
//	【殻の道具を借りる】同梱スクリプトの実行と殻の ID は本体からは手が届かないので、
//	境界越しに借りたもの（draw/HostServices）を使う。借りられなければフィードバックの
//	機能だけが静かに無効になる（取り込み自体は何も変わらない）。
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

	// **1 周の締めくくり。** 上記 2〜4 を行う。戻り値が true なら「もう 1 周」——
	// 呼び出し側は何も描かずに戻り、殻が本体を持ち直して取り込みを呼び直す。
	//
	// **結果ダイアログの代わりでもある。** フィードバックのダイアログが結果の本文と
	// ログを載せるので、この関数が true を返したかどうかに関わらず、呼び出し側は
	// 二重に結果ダイアログを出さない（shownResult が true で戻る）。
	bool runFeedbackRound(const FeedbackInput& input, bool& shownResult);

	// フィードバックの往復が**そもそも使えるか**（dev ビルドで、殻がスクリプトを
	// 貸してくれているか）。使えないときは呼び出し側が従来どおり結果ダイアログを出す。
	bool feedbackAvailable();
} // namespace HomeskzIfcImport::draw
