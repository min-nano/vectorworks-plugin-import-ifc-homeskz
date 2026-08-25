//
//	draw/Section.h
//
//	Phase 2（VW 描画）の軸組図（断面ビューポート）モジュール（docs/DEV-NOTES.md M14）。
//	命令セット（core::SectionCommand の列）から、通りごとの
//	**断面ビューポートを新規に作って**シートレイヤへ並べる。
//
//	【SDK 依存】draw/ は VectorWorks SDK のみに依存し、IFC / STEP の知識を持たない。
//	.cpp は PluginPrefix.h（SDK）を include するため SDK ビルドでのみコンパイルされ、
//	無 SDK の core/parse ライブラリには含めない。この宣言ヘッダ自体は core::Document /
//	core::Progress しか参照せず、SDK ヘッダを引き込まない（CLAUDE.md「依存の向きは厳守する」）。
//
//	【断面ビューポートはその場で作る】ISDK::CreateSectionViewport で**通りの数だけ新規に作る**
//	ので、図面テンプレートに断面ビューポートを仕込んでおく必要も、その枚数に縛られることも無い。
//
//	実描画（切断位置・視線の向き・奥行き・高さ範囲の効き方、シート上の並び）はローカルの
//	VectorWorks で目視確認する（docs/DEV-NOTES.md M14「ローカル確認」）。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"
#include "draw/ObjectHandles.h"
#include "draw/TagStyle.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	// Document 内の全 section 命令（軸組図）を描く。命令ごとにシートレイヤ（無ければ作成・
	// 番号がレイヤ名）→ 断面ビューポート生成 →表示レイヤの絞り込み → クラス表示 → 縮尺 →
	// 図面タイトル・図番 → 更新を行い、最後に**シートレイヤ上で重ならないよう格子状に並べる**
	// （並べる位置は実際にできたビューポートの大きさで決まるので、命令ではなくここが決める）。
	// **作れた断面ビューポートの数**を返す。
	//
	// progress には 1 枚ごとに 1 ステップ報告し、**ループの先頭で中止を見て抜ける**
	// （フェーズの見出しと配分は draw/ExecuteDocument が決める）。描けたところまでは
	// 図面に残る。note には異常（ビューポートを作れなかった等）の説明を入れる（無ければ空）。
	//
	// memberHandles には drawMembers が記録した「命令インデックス → 横架材ハンドル」の
	// 対応表を渡す（断面寸法データタグの関連付け先。伏図と同じ。draw/Tag.h）。tagStyle は
	// createTagStyle が作ったデータタグスタイル（伏図と**同じ 1 つ**を共有する）。
	std::size_t drawSections(const core::Document& document, core::ProgressReporter& progress,
							 std::string* note = nullptr,
							 const ObjectHandles* memberHandles = nullptr,
							 const TagStyle* tagStyle = nullptr);
} // namespace HomeskzIfcImport::draw
