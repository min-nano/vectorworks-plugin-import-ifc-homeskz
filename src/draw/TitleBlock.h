//
//	draw/TitleBlock.h
//
//	Phase 2（VW 描画）の図面枠（タイトルブロック）モジュール（docs/DEV-NOTES.md M28）。
//	取り込みが**作ったシートレイヤすべて**（伏図＝draw/Sheet・軸組図＝draw/Section）に
//	図面枠 PIO を 1 つずつ置き、取り込み設定で選ばれた**図面枠スタイル**を当てる。
//
//	【命令の列を取らない】置くものは「シートレイヤ 1 枚につき 1 つ」で、IFC の中身では
//	数も内容も変わらない。しかも軸組図のシートレイヤは**何枚できるかが描くときにしか
//	決まらない**（core/Document.h の SectionSheetCommand）ので、命令を 1 枚ずつ並べる
//	ことがそもそもできない。命令セットが持つのはスタイル名ただ 1 つ
//	（core::Document::titleBlockStyle）で、ここはそれを受けて置く。
//
//	【★スタイルは当てるが、作らない】図面枠スタイルは**利用者が自分の図面に用意したもの**
//	を名前で指すだけで、プラグインは作らない（CLAUDE.md 開発の基本方針 4）。したがって
//	  * 図面に**そのスタイルが無ければ 1 つも置かない**——スタイル無しの図面枠は「線だけの
//	    空の枠」になり、図面を汚すだけで誰の役にも立たない。置かずに診断へ残すほうが良い。
//	  * スタイルは `SetPluginObjectStyle` で関連付けるだけでは**中身が流れない**ので、
//	    全部置いてから `UpdateStyledObjects` を 1 回呼ぶ（横架材・柱と同じ作法。
//	    [SDK リファレンス「Parametric Objects」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Parametric%20Objects.md)
//	    の「プラグインスタイル」）。
//
//	【★PIO の登録名は候補から実地に決める（実験）】図面枠の**登録名（universal 名）は
//	SDK リファレンスの `Findings/` に載っていない**。載っているのは「図面枠スタイルは
//	シンボル定義の `GetSymbolDefSubType` が 552 になる」ことだけで
//	（[Findings「Symbols」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Symbols.md)）、
//	その型番号から登録名を引く呼び出しは知られていない。そこで**候補を順に試し、実際に
//	オブジェクトが作れた名前を採る**——PIO のパラメータ名を universal 名 → ローカライズ名の
//	順で引き直すのと同じ作法（draw/DrawUtil.h の ResolveParamName）で、「名前を決め打って
//	黙って効かない」を避けるための形である。**通った名前は診断ログへ必ず出す**ので、実機
//	（または実機フィードバックの往復）の 1 周で答えが確定する。
//	**確定したら SDK リファレンス側へ知見として送り、候補を 1 つに畳むこと。**
//
//	【置き場所は測って決める】図面枠の挿入点が枠のどこを指すかは分からないので、
//	**置いた後に外形を測って用紙の中心へ寄せる**（データタグ・グラフィック凡例と同じ
//	「置いた後に測って動かす」作法。
//	[Findings「Sheet Layers and Page Layout」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Sheet%20Layers%20and%20Page%20Layout.md)）。
//	用紙の中心は**原点**である（同 Findings「用紙は原点を中心に置かれている前提」。
//	draw/DrawUtil.h の SheetPaperArea）。**印刷可能領域の中心ではない**——余白が左右／上下で
//	違う用紙ではそこが原点からずれるが、図面枠は用紙に属するものなので用紙に合わせる。
//
//	【ビューポートより先に置く】オブジェクトは後から作ったものが手前に来るので、図面枠は
//	**シートレイヤを用意した直後**（ビューポート・凡例より前）に置く。呼び出し側
//	（draw/Sheet・draw/Section）がその順を守る。
//
//	【SDK 型を公開するヘッダ】シートレイヤのハンドルを引数に取るため、draw/Legend.h・
//	draw/Tag.h と同じく**SDK 型を公開する共通ヘッダ**で、自分で PluginPrefix.h を
//	（DrawUtil.h 経由で）取り込む。したがって**要素ごとの draw/*.h から include しては
//	ならない**（DrawUtil.h 冒頭の約束）。呼び出し元は draw/Sheet.cpp と draw/Section.cpp。
//
//	実描画（枠が用紙のどこに出るか・スタイルの中身）はローカルの VectorWorks で目視確認する。
//

#pragma once

#include "draw/DrawUtil.h"

#include "core/Document.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	// 図面枠の設置の集計。**実描画はローカルの VW でしか確認できない**ので、枠が出ない・
	// 位置がおかしいときに原因（スタイルが無い／PIO を作れない／測れない）を切り分けられる
	// ように件数で持ち帰る（draw/Legend の LegendCounts と同じ流儀）。
	//
	// 呼び出し側は prepareTitleBlocks で 1 つ作り、シートレイヤごとに drawSheetTitleBlock へ
	// 渡し、最後に finishTitleBlocks を呼ぶ。**伏図と軸組図は別々の集計を持つ**（フェーズが
	// 分かれており、診断行も別々に出るため）。
	struct TitleBlockCounts
	{
		// 当てるスタイル名（命令セットの写し。空なら「置かない」）。
		std::string style;
		// 解決したスタイルの RefNumber。0 なら**その名前のスタイルが図面に無い**ので、
		// 1 つも置かない（ヘッダ冒頭の ★）。
		RefNumber styleRef = 0;
		// 実際にオブジェクトを作れた PIO の登録名（空なら 1 つも通っていない）。
		// ヘッダ冒頭の ★「登録名は候補から実地に決める」。
		std::string plugin;

		std::size_t drawn = 0;	// 置けた図面枠
		std::size_t failed = 0; // どの候補名でも PIO を作れなかったシートレイヤ
		std::size_t placeLeft = 0; // 外形を測れず、用紙の中心へ寄せられなかった

		// 置いた図面枠そのもの（スタイルを流し込んでから測って動かすので覚えておく）。
		std::vector<MCObjectHandle> objects;
		// もう図面枠を置いたシートレイヤ。**軸組図は同じシートレイヤへ複数の命令が載る**
		// ので、これが無いと 1 枚の用紙に図面枠が何重にも積まれる（draw/Section）。
		std::vector<MCObjectHandle> sheets;
	};

	// 命令セットの図面枠の設定を読み、スタイルを解決する。置かない（スタイル名が空・
	// その名前のスタイルが図面に無い）ときは styleRef が 0 のまま返り、以降の
	// drawSheetTitleBlock は何もしない。**図面枠を置くフェーズの先頭で 1 回**呼ぶ。
	TitleBlockCounts prepareTitleBlocks(const core::Document& document);

	// シートレイヤ 1 枚へ図面枠を 1 つ置く（置けたら true）。同じシートレイヤへ 2 つ目は
	// 置かない（TitleBlockCounts::sheets）。**カレントレイヤをそのシートレイヤへ移す**
	// （PIO はカレントレイヤに入るため）ので、呼び出し側は必要なら後で戻すこと。
	//
	// 位置合わせはまだ行わない——スタイルを流し込むまで枠の大きさが決まらないので、
	// **測って動かすのは finishTitleBlocks**（draw/Legend の placeLegends と同じ事情）。
	bool drawSheetTitleBlock(MCObjectHandle sheetLayer, TitleBlockCounts& counts);

	// 置いた図面枠へスタイルを流し込み（`UpdateStyledObjects` を 1 回）、外形を測って
	// 用紙の中心＝**原点**へ寄せる。**すべて置き終えてから**呼ぶ。
	void finishTitleBlocks(TitleBlockCounts& counts);

	// 集計を人が読める 1 行の診断にする。**通った登録名は異常が無くても出す**——
	// ヘッダ冒頭の ★ を確定させるための唯一の手掛かりだから（outInfo 行き。異常は別に返す）。
	std::string titleBlockDiagnostics(const TitleBlockCounts& counts);

	// 平常でも出る内訳（当てたスタイル名・通った登録名・置いた枚数）。診断ログにだけ出す
	// （draw/Sheet の outInfo と同じ行き先）。
	std::string titleBlockInfo(const TitleBlockCounts& counts);
} // namespace HomeskzIfcImport::draw
