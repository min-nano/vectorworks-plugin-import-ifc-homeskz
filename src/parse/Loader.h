//
//	parse/Loader.h
//
//	IFC の読み込みと読み込み時サニタイズ。Python 版 ifc/loader.py に対応する。
//	ファイル（またはテキスト）を読み、ホームズ君 IFC2X3 に混入する IFC4 専用
//	エンティティ（IFCFOOTINGTYPE）を除去してから、最小 STEP リーダ（parse/Step）
//	でエンティティグラフ（Model）へ変換する。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md「Phase 1」）。
//
//	なぜサニタイズするか（Python 版 loader.py の docstring 参照）: ホームズ君構造EX が
//	出力する IFC はスキーマ上 IFC2X3 だが、IFC4 でしか定義されない IFCFOOTINGTYPE の
//	宣言が混入することがある。本プラグインは基礎の「型」を参照しないため、STEP テキスト
//	から当該インスタンス文を丸ごと除去して問題ない。自前 STEP リーダは未知エンティティ
//	も読めてしまうので、除去して被参照（IFCRELDEFINESBYTYPE 経由）を宙ぶらりんにしても、
//	リーダ側が未解決参照を寛容に扱う（entity(id) が nullptr を返す）。
//

#pragma once

#include "parse/Step.h"

#include <string>

namespace HomeskzIfcImport::parse
{
	// IFC テキストをサニタイズする。dropTypes に挙げた型名（大文字で比較）の
	// インスタンス文（#id=TYPE(...);）を丸ごと取り除いた新しいテキストを返す。
	// 既定では IFCFOOTINGTYPE を除去する。文字列内の ';' は文末とみなさない。
	std::string sanitizeIfcText(const std::string& text);

	// ファイルを読み、サニタイズしてから STEP Model を返す。読み込みに失敗した
	// ときは空の Model を返し、ok（非 null のとき）に false を入れる。フェーズ境界は
	// 値で返し、例外をフェーズ外へ漏らさない（CLAUDE.md「エラーハンドリング」）。
	Model loadIfc(const std::string& path, bool* ok = nullptr);

	// テキスト本文から直接 Model を作る（サニタイズ込み）。単体テストを容易にする。
	Model loadIfcFromText(const std::string& text);
} // namespace HomeskzIfcImport::parse
