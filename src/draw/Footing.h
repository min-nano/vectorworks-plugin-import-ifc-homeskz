//
//	draw/Footing.h
//
//	Phase 2（VW 描画）の基礎モジュール。Python 版 vw/footing.py に対応する。
//	命令セットの立上り（core::WallCommand）を**壁オブジェクト**へ、底盤
//	（core::SlabCommand）を**スラブオブジェクト**へ変換して配置する（ROADMAP.md M9）。
//
//	【SDK 依存】.cpp は PluginPrefix.h（VectorWorks SDK）を include するため、
//	SDK ビルドでのみコンパイルされる。この宣言ヘッダ自体は core::Document / core::Progress
//	しか参照せず SDK ヘッダを引き込まない（draw/*.h 共通の約束。draw/DrawUtil.h 参照）。
//
//	地中梁・人通口・壁結合・配筋は M10。本モジュールは立上りと底盤だけを描く。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"

#include <cstddef>

namespace HomeskzIfcImport::draw
{
	// 立上り（wall 命令）を壁オブジェクトとして描く。配置先レイヤ（"F-立上り"）が無い命令は
	// スキップする（レイヤは基礎ストーリの story 命令が作る）。実際に配置できた本数を返す。
	//
	// 描画は必ず**底盤より先**に行う（M10 の壁結合が立上りのハンドルを参照するため、Python 版の
	// 実行順 walls → wall_joins → slabs に揃えてある）。
	std::size_t drawWalls(const core::Document& document, core::ProgressReporter& progress);

	// 底盤（slab 命令）をスラブオブジェクトとして描く。配置先レイヤ（"F-底盤"）が無い命令は
	// スキップする。実際に配置できた枚数を返す。手順は床板（draw/Floor）と同じで、共通部分は
	// draw/DrawUtil（SetSlabComponents / SetSlabDatum / ResolveSlabStyle）にある。
	std::size_t drawSlabs(const core::Document& document, core::ProgressReporter& progress);
} // namespace HomeskzIfcImport::draw
