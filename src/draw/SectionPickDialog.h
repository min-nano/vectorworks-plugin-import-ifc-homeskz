//
//	draw/SectionPickDialog.h
//
//	軸組図にする通りを選ぶダイアログ（docs/DEV-NOTES.md M34）。取り込み設定ダイアログ
//	（draw/SettingsDialog）の**次に** 1 枚表示し、解析が軸組図にする通りを全部チェック付きで
//	並べる。チェックを外した通りは軸組図を描画しない（core::ImportOptions::skippedSections）。
//
//	【なぜ設定ダイアログと別の 1 枚か】候補は **IFC を解析して初めて決まり**、ファイルごとに
//	本数も名前も違う（数本〜30 本ほど）。設定ダイアログは「図面にあるシンボル」を選ぶもので
//	行の数が決まっており、コンパイル時の ID で組んだイベントマップと 2 つの形（サムネイル／
//	名前）を持つ——そこへ可変の本数を混ぜると、確実に表示するための構成を崩す。こちらは
//	チェックと名前だけの単純な形で、イベントを受けない（DDX で読むだけ）。
//
//	【毎回すべてチェックの入った状態で開く】通りの名前（"X1" / "い"）はファイルが違っても
//	同じ綴りになりうるので、前回除外した通りを記憶しておくと、別の図面で通知なしに軸組図が欠ける。
//	既定は「全部描画する」＝この選択を入れる前と同じ。
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
	// 軸組図にする通りを利用者に選ばせる。candidates は parse::buildSectionCandidates の結果
	// （X通り → Y通りの順）。
	//
	//   * Accepted    … 「取り込む」。チェックを外した通りの図番を options.skippedSections
	//     へ入れる。
	//   * Cancelled   … 「キャンセル」。取り込みを行わない（設定ダイアログと同じ扱い）。
	//   * Unavailable … 候補が無い・ダイアログを構築できなかった。options は変更しない
	//                   （**全部描画する**で続ける——選べないことを理由に取り込みを中止しない）。
	//
	// note には、構築できなかったときの原因を 1 行入れる（取り込みログへ出力する）。
	SettingsOutcome showSectionPicker(const std::vector<core::SectionCommand>& candidates,
									  core::ImportOptions& options, std::string* note = nullptr);
} // namespace HomeskzIfcImport::draw
