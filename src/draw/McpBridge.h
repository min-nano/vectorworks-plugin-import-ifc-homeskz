//
//	draw/McpBridge.h
//
//	**MCP ブリッジのうち、Vectorworks を操作する側。** 殻のパレット（Extensions/ExtMcpPalette.h）の
//	タイマーが数百 ms ごとにここを 1 回呼び、置かれている要求を処理して**すぐ戻る**。受け渡しの
//	取り決め（スプール・要求と応答の形）は SDK を知らない core/Bridge.h にあり、ここが持つのは
//
//	  * 1 回分の受け付け（スプールを確認して応答し、生存の印を書き直す）と、
//	  * **道具の表**（名前・説明・引数の形・実装）
//
//	の 2 つだけである。道具を追加するときに変更するのはその表 1 か所（McpBridge.cpp の kTools）。
//
//	【殻に依頼する道具】更新（`vw_update`）と再起動（`vw_restart`）は本体の中では実行できない
//	——本体は自身をアンロードできない（src/PayloadSession.h）。そこで要求を受け取って殻へ渡し、
//	殻が実行した結果を次の 1 回で受け取って応答する（M38。src/Extensions/ExtMcpPalette.cpp）。
//
//	【「1 回呼ばれて戻る」形にした理由（Vectorworks の制約）】SDK の呼び出しは**メインスレッド
//	から**行う決まりなので、受け口を別スレッドへ置けない。図面を操作する口を常時開けておく
//	仕組み（アイドルコールバック）も SDK に無い（[SDK リファレンス「レイヤ・ストーリ」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Layers%20and%20Stories.md)
//	の「SDK に遅延実行の口は無い」）。M24 ではそのためにメニューの実行期間をそのままブリッジの
//	稼働期間とし、進捗ダイアログの中でループしていた——**ブリッジの稼働中は利用者が図面を
//	操作できなかった**。
//
//	M30 で**モードレスのパレットの JS タイマー**に載せ替えた。M24〜M25 の実機フィードバックの
//	往復で「JS のタイマーから C++ へメインスレッドで届く」「隠れたパレットでもタイマーは
//	止まらない」の 2 つが実機で確かめられている（docs/DEV-NOTES.md M25「隠れたパレットは
//	止まらない」）。タイマーの 1 刻みがここの 1 回で、**戻っている間は Vectorworks が通常どおり
//	動く**——利用者は図面を操作でき、Claude は図面を読める。
//
//	【SDK 依存】この翻訳単位は PluginPrefix.h（Vectorworks SDK）を include するので、
//	**本体（ペイロード）側にだけ入る**（CLAUDE.md「依存の向きは厳守する」）。
//

#pragma once

#include <string>

namespace HomeskzIfcImport::draw
{
	// 置かれている要求を処理して戻る（1 回分。**待たない**——ただし `vw_run_test` を
	// 要求された回だけは、その 1 周が終わるまで戻らない）。戻り値はパレットに表示する状態の
	// JSON（受付中か・スプールの場所・処理した件数・最後の道具・理由）で、殻に依頼する要求
	// （`action`）と、shellReport を応答として書けたか（`reportDone`）を含む。
	// shellReport は殻が実行した依頼の結果（無ければ空。src/PayloadAbi.h）。
	// 例外は内部で捕捉する（境界へ漏らさない）。
	std::string serveMcpBridge(const std::string& shellReport);
} // namespace HomeskzIfcImport::draw
