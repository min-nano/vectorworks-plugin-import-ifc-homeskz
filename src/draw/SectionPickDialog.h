//
//	draw/SectionPickDialog.h
//
//	軸組図にする通りを選ぶダイアログ（docs/DEV-NOTES.md M33）。取り込み設定ダイアログ
//	（draw/SettingsDialog）の**次に** 1 枚出し、解析が軸組図にする通りを全部チェック付きで
//	並べる。チェックを外した通りは軸組図を描かない（core::ImportOptions::skippedSections）。
//
//	【なぜ設定ダイアログと別の 1 枚か】候補は **IFC を解析して初めて決まり**、ファイルごとに
//	本数も名前も違う（数本〜30 本ほど）。設定ダイアログは「図面にあるシンボル」を選ぶもので
//	行の数が決まっており、コンパイル時の ID で組んだイベントマップと 2 つの形（サムネイル／
//	名前）を持つ——そこへ可変の本数を混ぜると、確実に出すための作りを崩す。こちらは
//	チェックと名前だけの素朴な形で、イベントを受けない（DDX で読むだけ）。
//
//	【毎回すべてチェックの入った状態で開く】通りの名前（"X1" / "い"）はファイルが違っても
//	同じ綴りになりうるので、前回外した通りを覚えておくと、別の図面で黙って軸組図が欠ける。
//	既定は「全部描く」＝この選択を入れる前と同じ。
//
//	【SDK 依存】実装は PluginPrefix.h（VectorWorks SDK）を include する。このヘッダは
//	core/ までしか参照しないので、SDK を持たない翻訳単位からも安全に include できる。
//

#pragma once

#include "core/Document.h"
#include "core/ImportOptions.h"
#include "draw/SettingsDialog.h"

#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	// 軸組図にする通りを選ばせる。candidates は parse::buildSectionCandidates の結果
	// （X通り → Y通りの順）。
	//
	//   * Accepted    … 「取り込む」。外した通りの図番を options.skippedSections へ入れる。
	//   * Cancelled   … 「キャンセル」。取り込みを行わない（設定ダイアログと同じ扱い）。
	//   * Unavailable … 候補が無い・ダイアログを組めなかった。options は触らない
	//                   （**全部描く**で続ける——選べないことを理由に取り込みを落とさない）。
	//
	// note には、組めなかったときに何が駄目だったかを 1 行入れる（取り込みログへ出す）。
	SettingsOutcome showSectionPicker(const std::vector<core::SectionCommand>& candidates,
									  core::ImportOptions& options, std::string* note = nullptr);
} // namespace HomeskzIfcImport::draw
