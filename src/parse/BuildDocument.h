//
//	parse/BuildDocument.h
//
//	Phase 1（IFC 解析）の全体を統括する。IFC ファイルを読み、各要素の parse
//	モジュール（Grid / Story / Member …）を呼んで命令セット（core::Document）を組み立てる。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。通常の C++
//	ツールチェインだけでコンパイル・単体実行・テストできる（CLAUDE.md「Phase 1」）。
//	この宣言も core/Document.h しか依存しない。
//
//	parse/Loader で IFC を読み（テキスト→STEP グラフ）、要素ごとの parse モジュールを呼んで
//	Document を組み立てる。呼ぶ順とその理由は parse/BuildDocument.cpp にある。
//

#pragma once

#include "core/Document.h"
#include "core/ImportOptions.h"
#include "core/Progress.h"

#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	// IFC ファイルを解析して命令セットを返す。フェーズ境界は値で返す（例外をフェーズ外へ漏ら
	// さない）。1 要素の欠損で全体を止めず、解決できないものはスキップ・フォールバックで済ま
	// せる。
	core::Document buildDocument(const std::string& ifcPath);

	// 進捗を報告しながら解析する。読み込みと要素ごとの解析を core/Progress の
	// 2 フェーズ（kLoadShare / kParseShare）として報告する。上のオーバーロードは
	// これを NullProgressReporter で呼ぶだけ（＝振る舞いは同じ）。
	//
	// **中止要求（cancelled）はこの処理に割り込めない。** 中止要求は描画側で反映する。
	// 解析は大きなホームズ君 IFC でも 0.1 秒程度で終わり、途中で切り上げる意味が無い
	// （体感時間はすべて描画側にある。core/Progress.h の配分の但し書き参照）。
	core::Document buildDocument(const std::string& ifcPath, core::ProgressReporter& progress);

	// 取り込み設定（置換するシンボルの対応。core/ImportOptions.h）を与えて解析する。
	// 上の 2 つはこれを既定の設定で呼ぶだけ＝**設定を変更しなければ従来と同じ結果**になる。
	//
	// 設定は解析の入口でだけ受け取り、共有コンテキスト（parse/Context）が全要素へ配る
	// （要素ごとに引数を追加していくと、シンボルを 1 つ増やすたびに経路が増える）。
	core::Document buildDocument(const std::string& ifcPath, core::ProgressReporter& progress,
								 const core::ImportOptions& options);

	// IFC を読み、設定ダイアログに出す伏図レベルの候補（横架材の高さ 1 つずつ）を返す
	// （parse/PlanLevel）。取り込みの前に 1 度だけ呼ぶ——まとめるかどうかは設計者が決める
	// ので、どんな高さがあるかを先に提示する。読めなければ空（ダイアログはまとめる行なしで
	// 出る。取り込みは止めない）。
	std::vector<core::PlanLevelChoice> scanPlanLevelChoices(const std::string& ifcPath);

	// 軸組図にする通りの**候補**を返す（取り込みの前に、描画する通りを選ばせるため。M34）。
	// options の除外する通り（skippedSections）を**無視して**解析し、軸組図の命令を返す
	// ——並び・図番は、同じ options で取り込んだときに描画される軸組図と一致する
	// （除外する通りは図番を一意にした後で除くので、残る通りの図番は変わらない。
	// parse/Section.h 冒頭「外す通り」）。
	std::vector<core::SectionCommand> buildSectionCandidates(const std::string& ifcPath,
															 const core::ImportOptions& options);
} // namespace HomeskzIfcImport::parse
