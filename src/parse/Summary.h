//
//	parse/Summary.h
//
//	IFC の読み取り結果サマリ。ホームズ君 IFC を読み込み、主要エンティティ型の件数を
//	数えて返す（M0 の「ローカル確認」用の診断: パースが動いている確証を件数で示す）。
//
//	【2 つの役割】
//	  * summarizeModel / formatSummary … Document を経由せず生の Model から「どの IFC 型が
//	    何件あるか」を数える診断（M0 の名残。パースが動いている確証を件数で示す）。
//	  * documentCommandCount / formatImportResult … **インポート完了ダイアログの本文**を
//	    組み立てる（M15「完了文言の集約」）。以前は Extensions/ExtMenu.cpp に要素名を
//	    手書きで連ねていたが、要素が増えるたびに SDK 側の文字列を伸ばす作業が必要で、
//	    しかもその文言はテストできなかった。要素の一覧（ラベル・単位・命令数・描けた数）を
//	    Summary.cpp の表 1 つに集約し、SDK 側は組み上がった本文を出すだけにしてある。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md「Phase 1」）。
//	ここは Model（parse/Step）を読むだけの純ロジックなので、通常の C++ ツールチェインで
//	コンパイル・単体テストできる。ダイアログ表示（SDK 依存）は呼び出し側に任せ、この
//	文字列整形（formatSummary）までを無 SDK でテストできるようにしておく
//	（CLAUDE.md「テスト方針」: SDK から切り離せる部分は無 SDK 側へ寄せる）。
//

#pragma once

#include "core/Document.h"
#include "parse/Step.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	// 1 つの IFC 型の検出件数。ifcType は表示用のキャメルケース名（例: "IfcGridAxis"）、
	// label はホームズ君での役割を表す日本語ラベル（例: "通り芯"）。
	struct IfcTypeCount
	{
		std::string ifcType; // 表示用 IFC 型名（例: "IfcGridAxis"）
		std::string label;	 // 日本語ラベル（例: "通り芯"）
		std::size_t count = 0;
	};

	// IFC 読み取りサマリ。loaded はファイルを読み込めたか、entityCount は総エンティティ
	// 数、counts はホームズ君が使う主要型ごとの件数（固定順・欠けている型も count=0 で
	// 含める。件数表示が入力によってブレず、テストしやすいようにするため）。
	struct IfcSummary
	{
		bool loaded = false;
		std::size_t entityCount = 0;
		std::vector<IfcTypeCount> counts;
	};

	// 既に読み込んだ Model の主要型を数える。ホームズ君 IFC の骨格をなす型
	// （通り芯・階・横架材・柱・基礎・スラブ・金物）を固定順で数える（CLAUDE.md
	// 「移植の基本方針」で挙げる IfcGridAxis / IfcBuildingStorey / IfcBeam / IfcColumn /
	// IfcFooting / IfcSlab / IfcMechanicalFastener に対応）。loaded は true。
	IfcSummary summarizeModel(const Model& model);

	// ファイルを読み込んでサマリを返す。読み込みに失敗したら loaded=false・counts 空で
	// 返す（フェーズ境界は値で返し、例外をフェーズ外へ漏らさない）。
	IfcSummary summarizeIfc(const std::string& path);

	// サマリを人が読める複数行テキストへ整形する（メニューコマンドのダイアログ本文）。
	// 読み込み失敗時は失敗メッセージを返す。SDK 非依存なので単体テストで文言を検証できる。
	std::string formatSummary(const IfcSummary& summary);

	// ------------------------------------------------------------------------
	// インポート完了ダイアログの本文（M15「完了文言の集約」）
	// ------------------------------------------------------------------------

	// 命令セットに含まれる命令の総数（要素ごとの命令数の合計）。0 なら「取り込める要素が
	// 1 つも無かった」。要素の一覧は Summary.cpp の kElements ただ 1 か所に持つので、
	// 要素を足すときに数え漏らすことがない。
	std::size_t documentCommandCount(const core::Document& document);

	// インポート結果を完了ダイアログの本文へ整形する。**実際に描けた数**を要素ごとに
	// 1 行で並べ、命令はあるのに描けなかった要素は「3/12 本」の形にして、配置先
	// レイヤが無い・オブジェクトを作れない等の描画側の問題をローカル確認で解析側と
	// 切り分けられるようにする。命令が 0 件の要素は行ごと出さない（無い物の「0 件」は
	// 読む側の邪魔になるだけで、行が無いこと自体が「解析で 0 件」を意味する）。
	//
	// 検証に落ちたとき（counts.valid=false）・命令が 1 件も無いときはその旨だけを返す。
	// 中止（counts.cancelled）と描画側の診断（counts.diagnostics）も末尾へ足すので、
	// **呼び出し側（Extensions/ExtMenu）はこの戻り値をそのまま出すだけでよい**。
	//
	// logPath はクラッシュ診断ログ（core/Trace）の場所（無効なら空）。**空でなければ必ず
	// 出す**——一時ディレクトリは macOS では `/var/folders/…/T/` のような当てられない場所で、
	// 「ログはどこ？」に毎回答えることになるため（エラーダイアログと同じ扱い）。
	std::string formatImportResult(const core::Document& document, const core::DrawCounts& counts,
								   const std::string& logPath = {});

	// インポートが例外で中断したときのダイアログ本文。detail は例外の説明
	// （std::exception::what()。分からなければ空）で、空なら「原因不明」として出す。
	// logPath はクラッシュ診断ログ（core/Trace）の場所（無効なら空）。空でなければ
	// 「どこを見れば直前のフェーズが分かるか」を本文の最後で案内する。
	//
	// **なぜ要るか**: ネイティブプラグインの未捕捉例外は Python 版と違って VectorWorks
	// 本体を巻き込んで落とす。フェーズ境界（Extensions/ExtMenu の DoInterface）で必ず
	// 受け止め、ユーザーへ 1 通のエラーダイアログとして見せる（ROADMAP.md M15「例外処理」）。
	// 文言はここに置いて無 SDK でテストする（完了文言と同じ理由）。
	std::string formatImportError(const std::string& detail, const std::string& logPath = {});
} // namespace HomeskzIfcImport::parse
