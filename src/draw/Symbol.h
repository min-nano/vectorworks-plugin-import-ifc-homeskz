//
//	draw/Symbol.h
//
//	Phase 2（VW 描画）のシンボル配置モジュール。Python 版 vw/anchor_bolt.py /
//	vw/floor_post.py / vw/fire_brace.py / vw/joint.py に対応する（ROADMAP.md M11）。
//	命令セット（core::SymbolCommand）をハイブリッドシンボルのインスタンスとして配置する。
//
//	［Python 版との差異・意図的］Python 版は要素ごとに 4 つの vw モジュールを持つが、
//	中身は「配置先レイヤが在るか確かめて vs.Symbol(name, point, angle) を呼ぶ」だけで
//	**逐語的に同じ**。C++ ではここ 1 本にまとめ、要素の区別は呼び出し側（draw/ExecuteDocument
//	が Document のどのリストを渡すか）が担う（CLAUDE.md「重複を作らない置き場所」。
//	命令型を 1 つにまとめた理由は core/Document.h の SymbolCommand 参照）。
//
//	【SDK 依存】実装（draw/Symbol.cpp）は PluginPrefix.h（VectorWorks SDK）を include する。
//	このヘッダは core/Document.h までしか参照しないので、SDK を持たない翻訳単位からも
//	安全に include できる（CLAUDE.md「依存の向きは厳守する」）。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	// シンボル配置命令の列を描く。配置できた数を返す。
	//
	// 配置先レイヤが無い命令はスキップする（レイヤは story 命令が作るので、無い＝その
	// ストーリの生成がスキップされたということ。シンボルのために勝手にレイヤを作らない。
	// Python 版 execute_anchor_bolts ほかと同じ規約）。**シンボル定義が図面に無い場合も
	// スキップする**——ハイブリッドシンボル（"アンカーボルト_M12" / "床束" / "鋼製火打" /
	// "仕口"）はテンプレートやリソースライブラリから供給される前提で、プラグインは作らない。
	//
	// note に nullptr でない値を渡すと、スキップの内訳（配置先レイヤが無い件数・シンボル
	// 定義が無い名前）を 1 行の診断文へ入れる（何も無ければ空のまま）。「命令はあるのに
	// 描かれない」ときの原因をローカル確認で切り分けるための手掛かり（draw/Member と同じ枠組み）。
	//
	// progress には 1 件描くごとに 1 ステップ報告し、**ループの先頭で中止を見て抜ける**
	// （フェーズの見出しと配分は draw/ExecuteDocument が決める）。
	std::size_t drawSymbols(const std::vector<core::SymbolCommand>& commands,
							core::ProgressReporter& progress, const char* elementLabel,
							std::string* note = nullptr);
} // namespace HomeskzIfcImport::draw
