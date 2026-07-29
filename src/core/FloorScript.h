//
//	core/FloorScript.h
//
//	floor 命令 1 件を描く VectorScript を組み立てる（SDK 非依存の純粋な文字列生成）。
//
//	【なぜスクリプトなのか】VectorWorks 2026 の C++ SDK（ISDK）には**床ツール（Floor
//	オブジェクト）を生成する API が無い**（`Roof` / `Slab` はあるが Floor は
//	`Kernel` に種別定数 `kFloorSubT` があるだけで生成手段が公開されていない）。一方で
//	VectorScript には Python 版が使っている `BeginFloor` → 外形 → `EndGroup` があり、
//	SDK からは `VectorWorks::Scripting::IVectorScriptEngine::ExecuteScript` で
//	VectorScript を実行できる。そこで床だけは**スクリプト経由で床ツールを使う**ことで、
//	Python 版と同じオブジェクト種別・同じ高さの与え方を保つ（押し出しソリッドや
//	スラブで代替すると、床ツール固有の挙動・OIP・IFC 書き出しの意味付けが変わる）。
//
//	【なぜ core にあるのか】生成するのは**ただの文字列**で、SDK も STEP も要らない。
//	CLAUDE.md「テスト方針」の「SDK から切り離せる部分は core へ寄せて無 SDK テストする」
//	に従い、スクリプト本文の組み立て（座標の書式・エスケープ・命令の並び）をここで
//	組み立てて単体テストし、draw/Floor はレイヤの用意とスクリプト実行だけを担う。
//
//	組み立てるスクリプトは Python 版 vw/floor.py の draw_floor と 1 対 1 で対応する:
//	  BeginFloor(厚み) → ClosePoly → BeginPoly → MoveTo/LineTo → EndPoly → EndGroup
//	  → LNewObj → Move3D(0,0,床下端の絶対 Z) → SetClass → 各属性を by-class
//	  → SetObjectStoryBound(h, 0, 2, story_offset, level, offset) → ResetObject
//	床が作れなかった場合（LNewObj が NIL）は外形ポリゴンにフォールバックするのも同じ。
//

#pragma once

#include "core/Document.h"

#include <string>

namespace HomeskzIfcImport::core
{
	// 数値を VectorScript のリテラルへ整形する（常に '.' 小数点・指数表記なし）。
	// ロケールに依存しない（VW 上のロケールが変わってもスクリプトが壊れない）。
	std::string formatScriptNumber(double value);

	// 文字列を VectorScript の文字列リテラル（'…'）へ整形する。含まれる ' は '' へ
	// エスケープする（クラス名・レベル名に ' が入っていても壊れない）。
	std::string quoteScriptString(const std::string& text);

	// floor 命令 1 件を描く VectorScript（PROCEDURE …; RUN(…); の完結した本文）を返す。
	// 配置先レイヤの用意（存在確認とアクティブ化）は呼び出し側（draw/Floor）が SDK で
	// 行い、このスクリプトは**現在のアクティブレイヤ**に床を描く。
	std::string buildFloorScript(const FloorCommand& floor);
} // namespace HomeskzIfcImport::core
