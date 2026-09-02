//
//	draw/Footing.h
//
//	Phase 2（VW 描画）の基礎モジュール。命令セットの基礎（core::FoundationCommand）を
//	**自作 PIO 1 つ**（Extensions/ExtFoundation）として配置する（docs/DEV-NOTES.md M21）。
//	立上り・底盤・地中梁・床付けのソリッドは PIO 自身がリセット時に描くので、ここは
//	「PIO を置いてパラメータと部品を書き、リセットする」だけ。
//
//	【SDK 依存】.cpp は PluginPrefix.h（VectorWorks SDK）を include するため、
//	SDK ビルドでのみコンパイルされる。この宣言ヘッダ自体は core::Document / core::Progress
//	しか参照せず SDK ヘッダを引き込まない（draw/*.h 共通の約束。draw/DrawUtil.h 参照）。
//
//	M9〜M17 の壁・スラブ・モディファイア・可視ソリッドによる描画は M21 で無くなった。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	// 基礎（foundation 命令）を PIO として置く。命令が無ければ 0、置けたら 1 を返す。
	// 配置先レイヤ（"F-基礎"）が無い・PIO を作れない・部品を PIO のレコードへ書けなかった・
	// リセットしてもソリッドが 1 つも描かれなかった、という異常は outNote に残す（完了
	// ダイアログの「問題あり」の根拠。draw/Member と同じ流儀）。
	std::size_t drawFoundation(const core::Document& document, core::ProgressReporter& progress,
							   std::string* outNote = nullptr);
} // namespace HomeskzIfcImport::draw
