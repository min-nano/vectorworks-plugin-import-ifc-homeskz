//
//	draw/Member.h
//
//	Phase 2（VW 描画）の横架材モジュール。Python 版 vw/member.py に対応する。
//	命令セット（core::MemberCommand）を**構造材ツール（StructuralMember）**のオブジェクトと
//	して配置する（ROADMAP.md M7）。土台・梁・桁だけでなく、母屋・棟木・登り梁も同じ経路で
//	描く（違いは配置先レイヤと高さ基準レベルだけ）。
//
//	【SDK 依存】実装（draw/Member.cpp）は PluginPrefix.h（VectorWorks SDK）を include する。
//	このヘッダは core/Document.h までしか参照しないので、SDK を持たない翻訳単位からも
//	安全に include できる（CLAUDE.md「依存の向きは厳守する」）。
//

#pragma once

#include "core/Document.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	// Document の member 命令を描く。配置した本数を返す。
	//
	// 配置先レイヤ（"n-横架材天端" / "R-軒高" / "n-母屋" / "n-登り梁"）が無い命令は
	// スキップする（レイヤは story 命令が作るので、無い＝そのストーリの生成がスキップされた
	// ということ。横架材のために勝手にレイヤを作らない。Python 版 execute_members と同じ規約）。
	//
	// outDiagnostics に非 nullptr を渡すと、「配置はできたが断面を設定できなかった本数」
	// 「プラグインスタイルが見つからない」といった**描画側の異常**を人が読める 1 行として
	// 返す（異常が無ければ触らない）。実描画はローカルの VectorWorks でしか確認できないため、
	// 横架材が見えないときに原因を解析側と描画側で切り分けるための唯一の手掛かりになる。
	std::size_t drawMembers(const core::Document& document, std::string* outDiagnostics = nullptr);
} // namespace HomeskzIfcImport::draw
