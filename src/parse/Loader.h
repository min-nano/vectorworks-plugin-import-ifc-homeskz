//
//	parse/Loader.h
//
//	IFC ファイルの読み込み（テキスト → STEP エンティティグラフ）。**サニタイズ（非正規
//	エンティティの除去）は行わない**。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md「Phase 1」）。
//
//	なぜサニタイズしないか: ホームズ君の IFC2X3 には IFC4 専用エンティティ（IFCFOOTINGTYPE 等）
//	が混入するが、自前の STEP リーダ（parse/Step）は**スキーマ検証をせず、未知・非正規
//	エンティティもそのまま読める**。本プラグインはそれらの型を参照しないので、グラフ上に残って
//	いても無害——読み込み時に除去したのと同じ結果になる。
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
