//
//	draw/Footing.h
//
//	Phase 2（VW 描画）の基礎モジュール。Python 版 vw/footing.py に対応する。
//	命令セットの立上り（core::WallCommand）を**壁オブジェクト**へ、底盤
//	（core::SlabCommand）を**スラブオブジェクト**へ変換して配置する（ROADMAP.md M9）。
//
//	【SDK 依存】.cpp は PluginPrefix.h（VectorWorks SDK）を include するため、
//	SDK ビルドでのみコンパイルされる。この宣言ヘッダ自体は core::Document / core::Progress
//	しか参照せず SDK ヘッダを引き込まない（draw/*.h 共通の約束。draw/DrawUtil.h 参照）。
//
//	M10 で人通口（解析側で立上りが分割・切り下げ済みなので描画は変わらない）・壁結合
//	（drawWallJoins）・地中梁（底盤の modifiers）を足した。配筋は保留。
//

#pragma once

#include "core/Document.h"
#include "core/Progress.h"

#include <cstddef>
#include <memory>
#include <string>

namespace HomeskzIfcImport::draw
{
	// 立上りの壁ハンドル表の実体（draw/Footing.cpp の無名でない定義）。**SDK 型を持つので
	// このヘッダには中身を書かない**（draw/*.h は core までしか参照しない約束。
	// draw/DrawUtil.h 参照）。
	struct WallHandleTable;

	// 立上りの壁ハンドル表（命令インデックス → 壁ハンドル）を持ち回るための小さな所有者。
	// **SDK ハンドルは Document に載せられない**ので、壁結合はこの対応表で 2 本の壁を引く
	// （CLAUDE.md「所有権」: 命令インデックス → ハンドルの対応で受け渡す）。
	// executeDocument が 1 つ作り、drawWalls（書く）→ drawWallJoins（読む）へ渡す。
	class WallHandles
	{
	public:
		WallHandles();
		~WallHandles();
		WallHandles(const WallHandles&) = delete;
		WallHandles& operator=(const WallHandles&) = delete;

		WallHandleTable& table()
		{
			return *fTable;
		}
		const WallHandleTable& table() const
		{
			return *fTable;
		}

	private:
		std::unique_ptr<WallHandleTable> fTable;
	};

	// 立上り（wall 命令）を壁オブジェクトとして描く。配置先レイヤ（"F-立上り"）が無い命令は
	// スキップする（レイヤは基礎ストーリの story 命令が作る）。実際に配置できた本数を返す。
	//
	// handles を渡すと、**命令のインデックスをキーに**配置した壁ハンドルを記録する（壁結合
	// が引く。フォールバック描画＝壁を作れなかった命令とレイヤ未生成でスキップした命令は
	// 記録しない）。描画は必ず**底盤より先**に行う（Python 版の実行順 walls → wall_joins →
	// slabs に揃えてある）。
	// outNote には、命令数・配置数とレイヤ上の壁の本数が合わないときだけ診断を残す
	// （「命令に無い立上りが図面にある」を実機の 1 周で捕まえるため。draw/Footing.cpp 参照）。
	std::size_t drawWalls(const core::Document& document, core::ProgressReporter& progress,
						  WallHandles* handles = nullptr, std::string* outNote = nullptr);

	// 壁結合（wallJoin 命令）を実行して交差する立上りを結合する。結合できた件数を返す。
	// handles は drawWalls が記録した対応表で、a / b の**どちらかが未配置の命令はスキップ**
	// する。実行は立上りの直後・底盤の前（Python 版 execute_wall_joins と同じ位置）。
	//
	// 結合の**後に各立上りの端部キャップを命令どおりへ揃え直す**（JoinWalls が結合した端の
	// キャップを書き換えるため。draw/Footing.cpp「端部のキャップ」）。VW に拒否された結合が
	// あれば outNote に件数を残す（完了ダイアログの診断。draw/Member と同じ流儀）。
	std::size_t drawWallJoins(const core::Document& document, core::ProgressReporter& progress,
							  const WallHandles& handles, std::string* outNote = nullptr);

	// 底盤（slab 命令）をスラブオブジェクトとして描く。配置先レイヤ（"F-底盤"）が無い命令は
	// スキップする。実際に配置できた枚数を返す。手順は床板（draw/Floor）と同じで、共通部分は
	// draw/DrawUtil（SetComponents / SetSlabDatum / ResolveSlabStyle）にある。
	//
	// **地中梁（modifiers）を持つ底盤は台形プリズムを 2 回作る**: 削り取りモディファイア
	// （プロファイル群としてスラブへ渡し、底盤を clip する）と、可視の 3D ソリッド（削り取った
	// 位置を地中梁のコンクリートで埋める）。詳細は draw/Footing.cpp 冒頭。
	std::size_t drawSlabs(const core::Document& document, core::ProgressReporter& progress);
} // namespace HomeskzIfcImport::draw
