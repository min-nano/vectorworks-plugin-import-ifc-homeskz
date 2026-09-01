//
//	parse/Summary.h
//
//	IFC の読み取り結果サマリ。ホームズ君 IFC を読み込み、主要エンティティ型の件数を
//	数えて返す（M0 の「ローカル確認」用の診断: パースが動いている確証を件数で示す）。
//
//	【3 つの役割】
//	  * summarizeModel / formatSummary … Document を経由せず生の Model から「どの IFC 型が
//	    何件あるか」を数える診断（M0 の名残。パースが動いている確証を件数で示す）。
//	  * documentCommandCount / importOutcome / formatImportResult … **インポート完了
//	    ダイアログの本文**を組み立てる（M15「完了文言の集約」）。以前は
//	    Extensions/ExtMenu.cpp に要素名を手書きで連ねていたが、要素が増えるたびに SDK 側の
//	    文字列を伸ばす作業が必要で、しかもその文言はテストできなかった。要素の一覧
//	    （ラベル・単位・命令数・描けた数）を Summary.cpp の表 1 つに集約し、SDK 側は
//	    組み上がった本文を出すだけにしてある。
//	  * formatLogHeader / formatLogResult … **診断ログの見出しと結果**を組み立てる（M19）。
//	    完了ダイアログを「終わったか・問題があったか」だけに絞った代わりに、要素ごとの
//	    内訳・注意・記録はすべてログへ回した。要素の一覧は上と**同じ表**から出すので、
//	    要素を足すときに触るのは相変わらず 1 行だけ。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md「Phase 1」）。
//	ここは Model（parse/Step）を読むだけの純ロジックなので、通常の C++ ツールチェインで
//	コンパイル・単体テストできる。ダイアログ表示（SDK 依存）は呼び出し側に任せ、この
//	文字列整形（formatSummary）までを無 SDK でテストできるようにしておく
//	（CLAUDE.md「テスト方針」: SDK から切り離せる部分は無 SDK 側へ寄せる）。
//

#pragma once

#include "core/Document.h"
#include "core/ImportOptions.h"
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
	// インポート完了ダイアログの本文（M15「完了文言の集約」／M19「短い完了・厚いログ」）
	// ------------------------------------------------------------------------

	// 命令セットに含まれる命令の総数（要素ごとの命令数の合計）。0 なら「取り込める要素が
	// 1 つも無かった」。要素の一覧は Summary.cpp の kElements ただ 1 か所に持つので、
	// 要素を足すときに数え漏らすことがない。
	std::size_t documentCommandCount(const core::Document& document);

	// インポートの結末。**完了ダイアログもログも同じ判断を使う**ので、「成功と言いながら
	// ログには問題が並ぶ」という食い違いが起きない。
	enum class ImportStatus
	{
		Empty,	 // 解析は通ったが取り込める要素が 1 つも無かった
		Invalid, // 命令セットの検証に落ちた（何も描いていない）
		Cancelled, // ユーザーが進捗ダイアログで中止した（描けたところまでは残る）
		Warning, // 描き切れなかった要素があった／描画側の異常が出た
		Success	 // 命令をすべて描けて、異常も無かった
	};

	struct ImportOutcome
	{
		ImportStatus status = ImportStatus::Empty;
		std::size_t commands = 0; // 命令の総数
		std::size_t placed = 0;	  // 実際に描けた総数
	};

	// 命令セットと描画結果から結末を求める。中止は「描き切れなくて当然」なので Warning
	// より優先する（中止したのに「問題あり」と言われると、原因を探しに行ってしまう）。
	ImportOutcome importOutcome(const core::Document& document, const core::DrawCounts& counts);

	// **インポート完了ダイアログの本文**（M19）。読むのは「どのファイルを・成功したのか・
	// 問題はあったのか」の 3 つだけで済むよう短く保つ——**件数も所要時間もログにある**ので
	// ここには出さない（描けた数はうまくいっているときには読む必要が無く、うまくいって
	// いないときはその数だけでは足りない）。**一覧も戻さない**（読まれないものを毎回
	// 見せると、肝心の「問題あり」が埋もれる）。fileName は取り込んだファイル名
	// （空なら行を出さない）。
	//
	// 例外として残す 2 行は、どちらも**その場で操作が要る**もの:
	//   * 伏図・軸組図を作ったなら「1 回更新してください」（黙ると誤った絵を見せる）
	//   * 「取り消し」が普通に効かないとき——戻せない／新しく作ったレイヤの分しか戻らない
	//     （間違えたときの戻し方が変わる。**1 回で戻せるときは黙る**——「取り消し」が
	//     効くのは当たり前で、書くと読む量が増えるだけ。ログには常に残す）
	//
	// **ログの場所もここには出さない。** ログ自身の見出しが持つ（formatLogHeader）ので、
	// ダイアログの「ログを表示」を開けば 1 行目の近くで読める。
	std::string formatImportResult(const core::Document& document, const core::DrawCounts& counts,
								   const std::string& fileName = {});

	// インポートが例外で中断したときのダイアログ本文。detail は例外の説明
	// （std::exception::what()。分からなければ空）で、空なら「原因不明」として出す。
	//
	// **なぜ要るか**: ネイティブプラグインの未捕捉例外は VectorWorks 本体を巻き込んで落とす。
	// フェーズ境界（Extensions/ExtMenu の DoInterface）で必ず受け止め、ユーザーへ
	// 1 通のエラーダイアログとして見せる（docs/DEV-NOTES.md M15「例外処理」）。文言はここに置
	// いて無 SDK でテストする（完了文言と同じ理由）。
	std::string formatImportError(const std::string& detail, const std::string& fileName = {});

	// ------------------------------------------------------------------------
	// 診断ログの本文（M19「短い完了・厚いログ」）
	// ------------------------------------------------------------------------

	// 動かしているビルドの素性。**SDK 側（Extensions/ExtMenu）が BuildConfig.h から詰める**
	// ——ビルド種別のマクロを見られるのはあちらだけで、こちらは受け取った文字列を並べるだけ。
	struct BuildInfo
	{
		std::string plugin;	 // プラグイン名（"HomeskzIfcImport" / "…Dev"）
		std::string channel; // 配布チャンネル（"stable" / "dev"）
		std::string commit; // ビルドの短い識別子（git コミット。ローカルは "local"）
		std::string branch;	  // ビルド元のブランチ
		std::string platform; // 実行環境（"macOS" / "Windows"）
	};

	// **診断ログの見出し**。不具合の報告に貼られたとき、こちらが最初に知りたいのは
	// 「どのリビジョンを・いつ・どのファイルに対して動かしたか」なので、その 3 つを頭に置く
	// （docs/DEV-NOTES.md M19）。startedAt は壁時計（core::trace::localTimestamp）、
	// bytes は対象ファイルの大きさ（0 なら出さない）。
	//
	// logPath は**このログ自身の置き場所**（core::trace::path。書けなかったなら空）。
	// **ダイアログではなくここに書く**——場所を知りたいのはログを見ようとしたときだけで、
	// そのときログはもう目の前にある。書けなかったときは「ファイルは無い」と明示する
	// （黙ると、出ていないログを探しに行かせる）。
	std::string formatLogHeader(const BuildInfo& build, const std::string& ifcPath,
								unsigned long long bytes, const std::string& startedAt,
								const std::string& logPath = {});

	// **診断ログの結果**。結末・所要時間・要素ごとの内訳（描けた数／命令数）・描画側の注意・
	// 平常の記録（用紙の割り付け等）・取り消しの効き方を、この順に並べる。完了ダイアログから
	// 外した細かい情報はすべてここにある。
	std::string formatLogResult(const core::Document& document, const core::DrawCounts& counts,
								double seconds);

	// **取り込み設定（置換するシンボルの対応）の記録**。設定ダイアログで選んだ結果を
	// ログの見出しの次に置く（docs/DEV-NOTES.md M20）。**「シンボルが 1 つも置かれない」
	// という報告の原因はまずここ**——既定と違う対応になっていないか、図面に無い名前の
	// ままかが、ログだけで切り分けられる。既定のままの行には「（既定）」を添える。
	std::string formatImportOptions(const core::ImportOptions& options);
} // namespace HomeskzIfcImport::parse
