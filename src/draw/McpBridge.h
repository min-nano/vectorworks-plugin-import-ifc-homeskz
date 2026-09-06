//
//	draw/McpBridge.h
//
//	**MCP ブリッジの、Vectorworks を触る半分。** メニュー「MCP ブリッジ」の実行 1 回が
//	ここに来て、止められるまで戻らない。受け渡しの作法（スプール・要求と応答の形）は
//	SDK を知らない core/Bridge.h にあり、ここが持つのは
//
//	  * ループ（進捗ダイアログで yield しつつスプールを覗く）と、
//	  * **道具の表**（名前・説明・引数の形・実装）
//
//	の 2 つだけである。道具を足すときに触るのはその表 1 か所（McpBridge.cpp の kTools）。
//
//	【なぜループなのか＝Vectorworks の都合】SDK の呼び出しは**メインスレッドから**行う
//	決まりなので、受け口を別スレッドへ置けない。かといって図面を触る口を常時開けておく
//	仕組み（アイドルコールバック）は SDK に無い（[SDK リファレンス「レイヤ・ストーリ」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Layers%20and%20Stories.md)
//	の「SDK に遅延実行の口は無い」）。そこで**メニューコマンドの実行そのものを橋の寿命に
//	する**——押している間だけ橋が架かり、止めれば消える。
//
//	【その代わりの制約】ブリッジが動いている間、Vectorworks は進捗ダイアログの中にいる。
//	図面を人が編集することはできない（Claude からの読み書きは通る）。デバッグ用途では
//	これで十分と判断した。止めるのはダイアログの［キャンセル］か、道具 `vw_stop_bridge`。
//
//	【SDK 依存】この翻訳単位は PluginPrefix.h（Vectorworks SDK）を include するので、
//	**本体（ペイロード）側にだけ入る**（CLAUDE.md「依存の向きは厳守する」）。
//

#pragma once

namespace HomeskzIfcImport::draw
{
	// メニュー「MCP ブリッジ」の実行 1 回。止められるまで戻らない。
	// 例外は中で受け止め、利用者にはダイアログで見せる（境界へ漏らさない）。
	void runMcpBridge();
} // namespace HomeskzIfcImport::draw
