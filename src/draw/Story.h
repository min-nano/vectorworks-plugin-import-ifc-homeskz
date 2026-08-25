//
//	draw/Story.h
//
//	Phase 2（VW 描画）のストーリモジュール。命令セット（core::StoryCommand の列）から
//	VectorWorks のストーリ・ストーリレベル・デザインレイヤを生成する。ExecuteDocument
//	から通り芯より先にディスパッチされる（以降の要素はここで作ったレベルに配置される。
//	docs/DEV-NOTES.md M3）。
//
//	【SDK 依存】draw/ は VectorWorks SDK のみに依存し、IFC / STEP の知識を持たない。
//	.cpp は PluginPrefix.h（SDK）を include するため SDK ビルドでのみコンパイルされ、
//	無 SDK の core/parse ライブラリには含めない。この宣言ヘッダ自体は core::Document
//	しか参照せず、SDK ヘッダを引き込まない（CLAUDE.md「依存の向きは厳守する」）。
//
//	【レイヤのスタック順について】目的は「伏図ビューポートで床・野地板が柱・梁を覆い隠さない
//	ようにする」こと。希望順の**計算**は SDK 非依存の core::desiredStoryLayerOrder が持ち
//	（無 SDK テスト済み）、適用は 2 通りある——**本命はビューポート単位の重ね順上書き**
//	（draw/DrawUtil の ConfigureViewport。図面のレイヤの並びを動かさない）で、下記の
//	reorderStoryLayers は**それが図面に記録されなかったときの退避路**（draw/Sheet が
//	1 枚目の読み戻しで判断して呼ぶ）。デザインレイヤ自体は命令の levels の並び順に生成される。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"

#include <cstddef>

namespace HomeskzIfcImport::draw
{
	// Document 内の全ストーリを描く。まず命令に登場するレベル種別を登場順に登録し、各
	// StoryCommand ごとに CreateStory（同名のストーリが既にあればそれを再利用）→
	// SetStoryElevation → 各レベルをレベルテンプレートで生成（レイヤも同時に作成し
	// 意図した名前へリネーム）する。実際に用意できたストーリ数を返す。
	//
	// progress には 1 件描くごとに 1 ステップ報告し、**ループの先頭で中止を見て抜ける**
	// （進捗ダイアログの「キャンセル」。フェーズの見出しと配分は draw/ExecuteDocument が
	// 決める）。描けたところまでは図面に残る。
	std::size_t drawStories(const core::Document& document, core::ProgressReporter& progress);

	// デザインレイヤのスタック順を希望順（core::desiredStoryLayerOrder）へ並べ替える。
	// 動かせたレイヤ数を返す。**ビューポート単位の重ね順上書きが効かなかったときの退避路**で、
	// 効く図面では 1 度も呼ばれない（判断と呼び出しは draw/Sheet の drawSheets）。
	//
	// 【なぜ要るか】伏図ビューポートは、ビューポート単位の上書きが無ければ**ドキュメントの
	// レイヤ重ね順で描かれる**ので、床（"n-FL"）・野地板が柱・梁より前面にあると覆い隠して
	// しまう。希望順は「共通（通り芯）を最前面 → 最上階→最下階 → 床・野地板は最背面」で、
	// 計算そのものは SDK 非依存の core::desiredStoryLayerOrder が持つ（無 SDK テスト済み）。
	//
	// 【なぜ退避路なのか】これは**ユーザーの図面のレイヤの並びを組み替えてしまう**——伏図の
	// ためにナビゲーションパレットの並びが変わるのは副作用として大きい。だからまず
	// ビューポート単位の上書きを試し、記録されなかったときだけここへ落ちる
	// （M13 の 1 回目は上書きが空振りしたため、当時はこちらが唯一の手段だった）。
	//
	// 【並べ替えの手段】**ISDK には InsertObjectAfter / InsertObjectBefore があり、レイヤは
	// 図面のオブジェクト列に並んでいる**（VectorScript の HMoveForward に当たるのがこの 2 つ）
	// ので、列そのものを組み替える。
	//
	// **ビューポートを作る前に呼ぶこと。** ビューポートは生成時の重ね順で描かれるので、
	// 並べ替えを後にすると既にある図には反映されない（drawSheets は退避路へ落ちた 1 枚目の
	// ビューポートを作り直す）。
	std::size_t reorderStoryLayers(const core::Document& document);
} // namespace HomeskzIfcImport::draw
