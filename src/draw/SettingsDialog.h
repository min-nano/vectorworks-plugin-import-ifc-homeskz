//
//	draw/SettingsDialog.h
//
//	取り込み設定ダイアログ（docs/DEV-NOTES.md M20）。**IFC を選んだ直後**に 1 枚出し、
//	「どの要素を図面のどのシンボルで置くか」（core::ImportOptions）を決めてもらう。
//
//	【なぜ描画側にあるか】選択肢は「いま開いている図面にあるシンボル定義」で、それを
//	知っているのは VectorWorks だけ。解析側（parse/）は SDK を持たないので訊けない。
//	決めた結果は SDK も STEP も知らない値（core/ImportOptions.h）として解析側へ渡る
//	——Document と同じ「フェーズ間で運ぶ値」である（CLAUDE.md「依存の向きは厳守する」）。
//
//	【名前ではなく図で選べること】シンボルは名前を見ても中身が分からない（"床束" と
//	"床束2" のどちらが要る絵かは名前からは決まらない）。そこで各行に**シンボル表示
//	コントロール**（VWSymbolDisplayCtrl）を置き、選択中のシンボルの絵を Top/Plan・
//	ワイヤーフレームで出す——VectorWorks 自身がリソースのサムネイルに使っている
//	見え方に合わせてある（実装の SymbolImgInfo の既定値。draw/SettingsDialog.cpp）。
//
//	【SDK 依存】実装は PluginPrefix.h（VectorWorks SDK）を include する。このヘッダは
//	core/ImportOptions.h までしか参照しないので、SDK を持たない翻訳単位からも安全に
//	include できる。
//

#pragma once

#include "core/ImportOptions.h"

namespace HomeskzIfcImport::draw
{
	// 設定ダイアログの結果。
	enum class SettingsOutcome
	{
		Accepted,  // 「取り込む」を押した（options に選択が入っている）
		Cancelled, // 「キャンセル」を押した（取り込みは行わない）
		Unavailable, // ダイアログを組めなかった（**取り込みは既定の対応で続ける**）
	};

	// 取り込み設定ダイアログを 1 枚出す。
	//
	// 初期値は**同じ VectorWorks を起動している間の前回の選択**で、初回は役割の表の既定名
	// （core::symbolRoles()）。「取り込む」で確定した内容は次回の初期値として覚える
	// （図面を跨いでも覚えている。図面ごとに違うシンボルを使う場合は毎回選び直す）。
	//
	// 選択肢は**いま開いている図面のシンボル定義**（名前順）に、現在の対応先を足したもの。
	// 図面に無い名前は「（図面にありません）」を添えて出す——プラグインはシンボル定義を
	// 作らないので（draw/Symbol.cpp）、その対応のままだと何も置かれないことが選ぶ前に分かる。
	//
	// Unavailable のときは options を触らない（呼び出し側は既定の対応でそのまま取り込む
	// ——設定を出せないことを理由に取り込み自体を落とさない。docs/DEV-NOTES.md
	// 「結果ダイアログ」の逃げ道と同じ考え方）。
	SettingsOutcome showImportSettings(core::ImportOptions& options);
} // namespace HomeskzIfcImport::draw
