//
//	parse/Loader.h
//
//	IFC ファイルの読み込み（テキスト → STEP エンティティグラフ）。Python 版
//	ifc/loader.py に対応するが、**サニタイズ（非正規エンティティの除去）は行わない**。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md「Phase 1」）。
//
//	なぜサニタイズしないか: Python 版は ifcopenshell が IFC2X3 スキーマに無い
//	IFC4 専用エンティティ（ホームズ君 IFC に混入する IFCFOOTINGTYPE 等）に出会うと
//	処理を中断してしまうため、STEP テキストから除去していた。本プラグインの自前
//	STEP リーダ（parse/Step）はスキーマ検証をせず、未知・非正規エンティティもそのまま
//	読めるので除去は不要。本プラグインはそれらの型を参照しないので、グラフ上に残って
//	いても無害——挙動は Python 版の除去と同値になる。
//

#pragma once

#include "parse/Step.h"

#include <string>

namespace HomeskzIfcImport::parse
{
	// ファイルを読み、STEP Model を返す。読み込みに失敗したときは空の Model を
	// 返し、ok（非 null のとき）に false を入れる。フェーズ境界は値で返し、例外を
	// フェーズ外へ漏らさない（CLAUDE.md「エラーハンドリング」）。
	Model loadIfc(const std::string& path, bool* ok = nullptr);

	// テキスト本文から直接 Model を作る。単体テストを容易にする。
	Model loadIfcFromText(const std::string& text);
} // namespace HomeskzIfcImport::parse
