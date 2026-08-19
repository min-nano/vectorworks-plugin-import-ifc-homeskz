//
//	draw/Tag.h
//
//	Phase 2（VW 描画）の断面寸法データタグモジュール。Python 版 vw/sheet.py の draw_tag に
//	対応する（ROADMAP.md M13）。ビューポート命令が持つタグ（core::ViewportCommand::tags）を
//	**そのビューポートの注釈**としてデータタグ（PIO "Data Tag"）で置き、対象の横架材へ
//	関連付ける。
//
//	【伏図と軸組図で 1 つの実装】どちらのビューポートも「命令が持つ tags を注釈へ置く」だけで
//	済むのは、**どのタグがどの図に載るかを解析側（parse/Tag）が決めている**ため。Python 版は
//	平らな tag 命令をレイヤ名で振り分けていたが、断面（軸組図）はレイヤでは選べないので、
//	振り分けごと解析側へ寄せてある（parse/Tag.h 冒頭）。
//
//	【SDK 型を公開するヘッダ】ビューポートのハンドルを引数に取るため、このヘッダは
//	draw/DrawUtil.h・draw/StructuralMember.h と同じく**SDK 型を公開する共通ヘッダ**で、
//	自分で PluginPrefix.h を（DrawUtil.h 経由で）取り込む。したがって**要素ごとの draw/*.h から
//	include してはならない**（あちらは SDK を持たない翻訳単位＝Extensions/ExtMenu からも
//	include されるため。DrawUtil.h 冒頭の約束）。呼び出し元は draw/Sheet.cpp と
//	draw/Section.cpp の 2 つだけ。
//
//	【データタグの作法（Python 版と同じ順）】
//	  CreateCustomObject("Data Tag", 挿入点, 角度) → スタイルを関連付け → 引出線を OFF →
//	  ResetObject → 対象の横架材へ関連付け → ビューポート注釈へ追加 → タグを更新
//	引出線を OFF にするのは、**タグを部材の面ちょうどに置いても既定 ON だと引出線が描かれる**
//	ため（位置の決め方は parse/Tag.h）。関連付け先が無い（構造材ツールで描けずフォールバックの
//	直線になった）横架材のタグは、関連付けだけを省いて置く（Python 版と同じ）。
//
//	実描画（タグの見え方・スタイルの効き・注釈空間での位置）はローカルの VectorWorks で
//	目視確認する（ROADMAP.md M13「ローカル確認」）。
//

#pragma once

#include "draw/DrawUtil.h"

#include "core/Document.h"

#include <cstddef>
#include <string>

namespace HomeskzIfcImport::draw
{
	// タグ描画の集計。**実描画はローカルの VW でしか確認できない**ので、タグが 1 つも
	// 出ないときに原因（スタイルが無い／タグを作れない／関連付け先が無い）を切り分けられる
	// ように件数で持ち帰る（draw/Member の診断と同じ流儀）。
	struct TagCounts
	{
		std::size_t drawn = 0;	// 注釈に置けたタグ
		std::size_t failed = 0; // PIO を作れなかった／注釈に入れられなかった
		std::size_t unassociated = 0; // 関連付け先の横架材ハンドルが無かった
		std::size_t leaderLeft = 0; // 引出線を OFF にできなかった（引出線が残る）
		std::size_t classesShown = 0; // タグのクラスを表示へ戻せた数（0 ならタグが映らない）
		std::size_t updateFailed = 0; // クラスを戻した後の再更新に失敗したビューポート
		bool styleMissing = false; // "断面寸法" スタイルが文書に無い
	};

	// データタグ PIO の定義を**設定ダイアログを出さない**で用意する。タグを 1 つでも置く
	// フェーズ（伏図・軸組図）の先頭で 1 回呼ぶ。
	//
	// CreateCustomObject は、その名前の PIO が**その文書に**まだ定義されていなければ
	// DefineCustomObject で定義を作る。既定は kCustomObjectPrefAlways なので、最初の 1 個を
	// 作るときだけ「オブジェクトの設定」ダイアログが出てインポートが止まる（M12 の記号 PIO で
	// 実機確認済みの落とし穴。draw/ColumnMark.cpp）。**静的フラグで 1 回だけにはしない**
	// ——定義は文書ごとなので、次の文書へのインポートで抜けてしまう。
	void prepareDataTagPlugin();

	// ビューポート 1 枚ぶんのタグを注釈として置く。置けた数を返し、内訳を counts へ積む
	// （複数のビューポートぶんを 1 つの counts へ積んでよい）。memberHandles は drawMembers が
	// 記録した「命令インデックス → 横架材ハンドル」の対応表。
	std::size_t drawViewportTags(MCObjectHandle viewport, const core::ViewportCommand& command,
								 const ObjectHandleTable& memberHandles, TagCounts& counts);

	// 集計を人が読める 1 行の診断にする（異常が無ければ空文字）。label は図の種別
	// （"伏図" / "軸組図"）。
	std::string tagDiagnostics(const std::string& label, const TagCounts& counts);
} // namespace HomeskzIfcImport::draw
