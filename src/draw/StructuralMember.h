//
//	draw/StructuralMember.h
//
//	**構造材ツール（StructuralMember PIO）を 1 本描く**ための共通ヘルパー。横架材
//	（draw/Member）と柱（draw/Column）は同じ PIO を同じ手順——パス＋断面プロファイルから
//	CreateCustomObjectPath → クラス分け → スタイル関連付け → 両端のストーリバウンド →
//	個別フィールド（構造材 ID・断面・種別・端部条件）→ ResetObject——で作る。以前は
//	この手順が両 .cpp に**逐語的な複製**として置かれており、フィールド名を 1 つ足す・
//	ポップアップのキーを直すといった変更が、直した側でしか効かない形になっていた
//	（CLAUDE.md「重複を作らない置き場所」）。ここが唯一の定義。
//
//	【パスの作り方だけが要素ごとに違う】共通化しないのはパスだけで、これは実装の都合では
//	なく**部材の向きが本質的に違う**ためである。
//	  * 横架材（水平材）… **平面の 2D ポリライン**（CreateFlatPath）。高さ・傾斜は
//	    始端／終端のストーリバウンドの offset 差だけで与える。**パスに Z 成分を持たせては
//	    ならない**——構造材ツールはバウンドの高さ差をパス由来の部材長へ**加算**するので、
//	    パスにも傾斜を持たせると二重に適用され終端が 2 倍の高さになる（draw/Member.cpp 冒頭）。
//	  * 柱（鉛直材）… **鉛直な 2 点の NURBS 曲線**（CreateVerticalPath）。平面へ落とすと
//	    1 点に潰れるので 2D ポリラインでは表せない。実体の高さはこの鉛直パスが作り、
//	    上下端のバウンド差がその高さを支配する（draw/Column.cpp 冒頭）。
//	つまり「M7 の横架材が 2D パスなのは `Add3DVertex`（VS の AddVertex3D）を見つけられて
//	いなかったから」ではない。両方の作法が揃った今も**水平材は平面パス・鉛直材は 3D パス**が
//	正しく、どちらの選択も実機で確認済みである。**パスの種別を入れ替えてはならない。**
//
//	【SDK 依存・include の規約】このヘッダは draw/DrawUtil.h と同じく SDK 型
//	（MCObjectHandle / RefNumber）を公開するので、**draw/*.cpp からのみ include する**
//	（要素ごとの draw/*.h は従来どおり core/Document.h までしか参照しない。
//	CLAUDE.md「依存の向きは厳守する」）。
//

#pragma once

#include "PluginPrefix.h"

#include "core/Document.h"

#include <string>

namespace HomeskzIfcImport::draw
{
	// 断面基準点（PIO の AxisAlign）。パスが断面のどこを通るかを決めるので、**呼び出し側が
	// 渡す断面矩形の置き方と必ず一致させる**（天端中央なら原点が上辺中央、中央なら原点が
	// 断面中心）。値は構造材ツールのポップアップのキー（DrawStructuralMember 内で変換）。
	enum class StructuralAxisAlign
	{
		TopCentre, // 天端中央（横架材。パスは天端中央線）
		Centre,	   // 中央（柱。パスは断面中心を通る鉛直線）
	};

	// 構造材 1 本ぶんの描画仕様。path / profile は呼び出し側が用意する（下記の
	// CreateFlatPath / CreateVerticalPath と DrawUtil の CreateRectangleProfileGroup）。
	struct StructuralMemberSpec
	{
		MCObjectHandle path = nil; // パス（水平材＝平面ポリライン／鉛直材＝NURBS 曲線）
		MCObjectHandle profile = nil; // 断面プロファイルのグループ（空は不可）
		std::string memberId;		  // 構造材 ID（OIP の「構造材 ID」）
		std::string drawClass;		  // クラス名（空ならクラスを割り当てない）
		std::string structuralUse; // 構造用途のキー（横架材="1" / 柱="4" / 小屋束="5"）
		double width = 0.0;		   // 断面幅（主幅。mm）
		double depth = 0.0;		   // 断面せい（主せい。mm）
		StructuralAxisAlign axisAlign = StructuralAxisAlign::TopCentre;
		core::StoryBoundCommand startBound; // 始端（柱は下端）の高さ基準
		core::StoryBoundCommand endBound;	// 終端（柱は上端）の高さ基準
	};

	// DrawStructuralMember の結果。**断面が入ったかを呼び出し側へ返す**のは、実描画を
	// ローカルの VectorWorks でしか確認できないため（断面が 0 だとオブジェクトはあるのに
	// 画面へ出ない。各 draw モジュールが件数を完了ダイアログへ載せる）。
	struct StructuralMemberResult
	{
		MCObjectHandle object = nil; // 生成できなければ nil（呼び出し側はフォールバックへ）
		bool sectionOk = false; // 主幅・主せいを読み戻しで確認できたか
	};

	// 水平材のパス＝始端→終端の「開いた 2D ポリライン」。**Z は持たせない**（冒頭参照）。
	// 作れなければ nil。
	MCObjectHandle CreateFlatPath(const core::Vec2& start, const core::Vec2& end);

	// 鉛直材のパス＝下端 → 上端の 2 点の NURBS 曲線（gSDK->CreateNurbsCurve ＋
	// gSDK->Add3DVertex。**Add3DVertex が VS の AddVertex3D にあたる**）。頂点が本当に
	// 2 つになったかを outAppended に返す（診断用。ここが崩れると何も描かれない）。
	// 作れなければ nil。
	MCObjectHandle CreateVerticalPath(const core::Vec2& position, double bottomZ, double topZ,
									  bool& outAppended);

	// 構造材ツールの PIO を 1 つ生成して仕様どおりに設定する。style が 0 ならスタイルを
	// 関連付けずに描く（スタイルの欠落で部材を失わない）。
	StructuralMemberResult DrawStructuralMember(const StructuralMemberSpec& spec, RefNumber style);
} // namespace HomeskzIfcImport::draw
