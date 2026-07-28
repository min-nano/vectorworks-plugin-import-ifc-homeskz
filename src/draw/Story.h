//
//	draw/Story.h
//
//	Phase 2（VW 描画）のストーリモジュール。Python 版 vw/story.py に対応する。
//	命令セット（core::StoryCommand の列）から VectorWorks のストーリ・ストーリレベル・
//	デザインレイヤを生成し、希望するスタック順へ並べ替える。ExecuteDocument から
//	通り芯より先にディスパッチされる（以降の要素はここで作ったレイヤに配置される。
//	ROADMAP.md M3）。
//
//	【SDK 依存】draw/ は VectorWorks SDK のみに依存し、IFC / STEP の知識を持たない。
//	.cpp は PluginPrefix.h（SDK）を include するため SDK ビルドでのみコンパイルされ、
//	無 SDK の core/parse ライブラリには含めない。この宣言ヘッダ自体は core::Document
//	しか参照せず、SDK ヘッダを引き込まない（CLAUDE.md「依存の向きは厳守する」）。
//	なお希望スタック順の**計算**（core::desiredStoryLayerOrder）は SDK 非依存で core に
//	あり無 SDK テスト済み。ここはその順に実際のレイヤを SDK で並べ替える薄い層。
//

#pragma once

#include "core/Document.h"

#include <cstddef>

namespace HomeskzIfcImport::draw
{
	// Document 内の全ストーリを描く。各 StoryCommand ごとに CreateStory →
	// SetStoryElevationN → 各レベルをレベルテンプレートで生成（レイヤも同時に作成し
	// 意図した名前へリネーム）する。実際に作成できたストーリ数を返す。
	//
	// レイヤのスタック順並べ替え（reorderStoryLayers）は、通り芯レイヤ "共通" が生成
	// された後（全描画後）に呼ぶ必要があるため、ここでは行わず executeDocument が
	// 全要素の描画後にまとめて呼ぶ（Python 版 execute_stories と同じ分担）。
	std::size_t drawStories(const core::Document& document);

	// デザインレイヤを希望スタック順（core::desiredStoryLayerOrder）どおりに並べ替える。
	// レベルテンプレートはレイヤをレベル高さ順に挿入するため、明示的な並べ替えが要る。
	// 生成されていないレイヤ（通り芯描画前の "共通" 等）は SDK 側でスキップされる。
	// 全要素の描画が済んだ後に executeDocument から一度だけ呼ぶ（ROADMAP.md M3）。
	void reorderStoryLayers(const core::Document& document);
} // namespace HomeskzIfcImport::draw
