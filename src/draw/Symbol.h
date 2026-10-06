//
//	draw/Symbol.h
//
//	Phase 2（VW 描画）のシンボル配置モジュール（docs/DEV-NOTES.md M11）。命令セット（core::
//	SymbolCommand）をハイブリッドシンボルのインスタンスとして配置する。
//
//	【5 要素で 1 本の実装】アンカーボルト・床束・火打・仕口・継手の描画は「配置先レイヤがあるか
//	確認してシンボルを置く」だけで**まったく同じ**なので、ここ 1 本にまとめ、要素の区別は
//	呼び出し側（draw/ExecuteDocument が Document のどのリストを渡すか）が担う
//	（CLAUDE.md「重複を作らない置き場所」。命令型を 1 つにまとめた理由は
//	core/Document.h の SymbolCommand 参照）。
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
	// シンボル配置命令の列を描画する。配置できた数を返す。
	//
	// 配置先レイヤが無い命令はスキップする（レイヤは story 命令が作るので、無い＝そのストーリ
	// の生成がスキップされたということ。シンボルのために独自にレイヤを作らない）。
	// **シンボル定義が図面に無い場合もスキップする**——ハイブリッドシンボル（取り込み設定で
	// 選ぶ。既定名は "アンカーボルト_M12" / "床束" / "鋼製火打" / "仕口" / "継手" など）は
	// テンプレートやリソースライブラリから供給される前提で、プラグインは生成しない。
	//
	// note に nullptr でない値を渡すと、スキップの内訳（配置先レイヤが無い件数・シンボル
	// 定義が無い名前）を 1 行の診断文へ入れる（何も無ければ空のまま）。「命令はあるのに
	// 描画されない」ときの原因をローカル確認で切り分けるための手掛かり（draw/Member
	// と同じ枠組み）。
	//
	// progress には 1 件描画するごとに 1 ステップ報告し、**ループの先頭で中止要求を確認して
	// 抜ける**（フェーズの見出しと配分は draw/ExecuteDocument が決める）。
	std::size_t drawSymbols(const std::vector<core::SymbolCommand>& commands,
							core::ProgressReporter& progress, const char* elementLabel,
							std::string* note = nullptr);
} // namespace HomeskzIfcImport::draw
