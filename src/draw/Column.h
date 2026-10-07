//
//	draw/Column.h
//
//	Phase 2（VW 描画）の柱モジュール。命令セット（core::ColumnCommand）を**構造材ツール
//	（StructuralMember）**の鉛直材として配置する（docs/DEV-NOTES.md M8）。管柱・通し柱・
//	小屋束を同じ経路で描画し、違いは構造用途（StructuralUse）とクラス・配置先の span レイヤだけ。
//	**柱の高さは上下端のストーリバウンドだけで決まる**（パスは配置点しか与えない。
//	draw/Column.cpp 冒頭）。
//
//	【SDK 依存】実装（draw/Column.cpp）は PluginPrefix.h（VectorWorks SDK）を include する。
//	このヘッダは core/Document.h までしか参照しないので、SDK を持たない翻訳単位からも
//	安全に include できる（CLAUDE.md「依存の向きは厳守する」）。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"
#include "draw/ObjectHandles.h"
#include "draw/Verify.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	// Document の column 命令を描画する。配置した本数を返す。
	//
	// handles を渡すと、**命令のインデックスをキーに**配置した柱ハンドルを記録する
	// （取り込み後の測り直し recheckColumns が参照する。フォールバック描画＝構造材ツールを
	// 生成できなかった命令と、レイヤ未生成でスキップした命令は記録しない）。
	//
	// outDiagnostics に非 nullptr を渡すと、「配置はできたが断面を設定できなかった本数」
	// 「高さ基準を図面へ書けなかった本数」といった**描画側の異常**を人が読める 1 行として
	// 返す（異常が無ければ変更しない）。柱は**部材長を数えない**（横架材と同じ枠組みだが、
	// パスが長さを持たず 0 が正常。draw/Column.cpp 冒頭）。
	// 理由: 実描画はローカルの VectorWorks でしか確認できないため、柱が表示されないときに
	// 原因を解析側と描画側で切り分けるための唯一の手掛かりになる。
	//
	// 配置先の span レイヤ（"1to2-柱" 等）が無い命令はスキップする。柱のためにレイヤを
	// 作成しない（レイヤは story 命令が作るので、無い＝そのストーリの生成がスキップされた
	// ということ）。
	//
	// progress には 1 件描画するごとに 1 ステップ報告し、**ループの先頭で中止要求を確認して
	// 抜ける**（進捗ダイアログの「キャンセル」。フェーズの見出しと配分は draw/ExecuteDocument
	// が決める）。描画済みの分は図面に残る。
	std::size_t drawColumns(const core::Document& document, core::ProgressReporter& progress,
							std::string* outDiagnostics = nullptr,
							ObjectHandles* handles = nullptr);

	// **取り込みが終わったあとに**柱を測り直す（draw/ExecuteDocument が最後に 1 回呼ぶ）。
	//
	// 実体が無い柱・命令と食い違う柱の件数を outDiagnostics へ返す。outNotes には 1 本目の
	// 実測（どのパラメータが何を返しているか）を入れる——平常でも出る記録なので診断ログに
	// だけ出す。
	//
	// **開発ビルドだけ**（draw/Verify.h）。測定して診断へ載せるだけで図面は一切変更しないので、
	// 除外しても利用者の描画結果は 1 つも変わらない——逆に、**有効にしておくと取り込みの
	// たびに全柱のパラメータを走査する**。
	//
	// 【なぜ最後にもう一度測るのか】「柱オブジェクトは在り、OIP の値も命令どおりなのに実体が
	// 無い」という事故を追跡するのに、**描画した直後か・描画したあとか**を分ける地点がここしか
	// 無い（全要素・伏図・軸組図まで済んだ唯一の場所）。M27 の原因は描画直後とここの両方で
	// 測定して特定した——**生成直後から退化しており**、原因は渡したパスの両端の Z に 1 ULP の
	// 丸めが残り、`ResetObject` の再構築から外れたことだった（上端のバウンドの指し方を疑った
	// 見立ては誤り。docs/DEV-NOTES.md「柱が長さ 0 で描かれる（M27）」）。パスに Z を渡さなく
	// なった（draw/StructuralMember.h 冒頭）ので、ここは**同じ事故が再発していないか**を
	// 数える監視として残す。
#if VW_DRAW_VERIFY
	void recheckColumns(const core::Document& document, const ObjectHandles& handles,
						std::string* outDiagnostics, std::string* outNotes);
#endif

} // namespace HomeskzIfcImport::draw
