//
//	draw/McpBridge.h
//
//	**MCP ブリッジの、Vectorworks を触る半分。** 殻のパレット（Extensions/ExtMcpPalette.h）の
//	時計が数百 ms ごとにここを 1 回呼び、置かれている要求を捌いて**すぐ戻る**。受け渡しの
//	作法（スプール・要求と応答の形）は SDK を知らない core/Bridge.h にあり、ここが持つのは
//
//	  * 1 回ぶんの受け付け（スプールを覗いて応え、生存の印を書き直す）と、
//	  * **道具の表**（名前・説明・引数の形・実装）
//
//	の 2 つだけである。道具を足すときに触るのはその表 1 か所（McpBridge.cpp の kTools）。
//
//	【殻に頼む道具】更新（`vw_update`）と再起動（`vw_restart`）は本体の中ではできない
//	——本体は自分を降ろせない（src/PayloadSession.h）。そこで要求を引き取って殻へ渡し、
//	殻が済ませた結末を次の 1 回で受け取って応える（M38。src/Extensions/ExtMcpPalette.cpp）。
//
//	【なぜ「1 回呼ばれて戻る」なのか＝Vectorworks の都合】SDK の呼び出しは**メインスレッド
//	から**行う決まりなので、受け口を別スレッドへ置けない。図面を触る口を常時開けておく
//	仕組み（アイドルコールバック）も SDK に無い（[SDK リファレンス「レイヤ・ストーリ」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Layers%20and%20Stories.md)
//	の「SDK に遅延実行の口は無い」）。M24 ではそのためにメニューの実行そのものを橋の寿命に
//	して、進捗ダイアログの中でループしていた——**架けている間は人が図面を触れなかった**。
//
//	M30 で**モードレスのパレットの JS タイマー**に載せ替えた。M24〜M25 の実機フィードバックの
//	往復で「JS のタイマーから C++ へメインスレッドで届く」「隠れたパレットでも時計は
//	止まらない」の 2 つが実機で確かめられている（docs/DEV-NOTES.md M25「隠れたパレットは
//	止まらない」）。時計の刻み 1 つがここの 1 回で、**戻っている間は Vectorworks が自由に
//	動く**——人は図面を触ったまま、Claude は読める。
//
//	【SDK 依存】この翻訳単位は PluginPrefix.h（Vectorworks SDK）を include するので、
//	**本体（ペイロード）側にだけ入る**（CLAUDE.md「依存の向きは厳守する」）。
//

#pragma once

#include <string>

namespace HomeskzIfcImport::draw
{
	// 置かれている要求を捌いて戻る（1 回ぶん。**待たない**——ただし `vw_run_test` を
	// 頼まれた回だけは、その 1 周が終わるまで戻らない）。戻り値はパレットに見せる見え方の
	// JSON（受付中か・スプールの場所・捌いた件数・最後の道具・理由）に、殻に頼む要求
	// （`action`）と、shellReport を応答として書けたか（`reportDone`）が載る。
	// shellReport は殻が済ませた頼みごとの結末（無ければ空。src/PayloadAbi.h）。
	// 例外は中で受け止める（境界へ漏らさない）。
	std::string serveMcpBridge(const std::string& shellReport);
} // namespace HomeskzIfcImport::draw
