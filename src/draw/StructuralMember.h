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
//	【パスも共通】水平材も鉛直材も本質は**3 次元空間の直線 1 本**なので、パスの作り方も
//	分けない。どちらも 2 点の NURBS 曲線（`CreateNurbsCurve` ＋ `Add3DVertex`＝VS の
//	`AddVertex3D`）で作る（CreatePath）。これは Python 版 vw/member.py・vw/column.py と
//	同じ作法でもある——M7 の横架材が 2D ポリラインだったのは「ISDK には NURBS 曲線へ頂点を
//	足す呼び出しが無い」と誤認していたためで、M8 の柱で `Add3DVertex` が見つかりその前提は
//	消えた。要素ごとに違うのは**2 点の Z をどう置くか**だけで、それは下記のとおり部材の
//	仕様そのものである。
//
//	【Z の置き方（＝呼び出し側が決める唯一の差）】
//	  * 水平材（横架材）… 両端とも**同じ Z**（天端 Z）を渡す。傾斜梁の勾配は始端／終端の
//	    ストーリバウンドの offset 差だけで表し、**パスには持たせない**——構造材ツールは
//	    バウンドの高さ差をパス由来の部材長へ加算しうるので、パスにも傾斜を持たせると
//	    二重に適用される（Python 版が実機で確認済み。draw/Member.cpp 冒頭）。
//	  * 鉛直材（柱）… 下端 Z → 上端 Z。実体の高さをこのパスが作り、上下端のバウンド差が
//	    その高さを支配する（draw/Column.cpp 冒頭）。
//
//	**Z に 0 を渡してはならない。** 「パスに高さを持たせない＝Z=0」ではない。3D 座標は
//	**絶対 Z** として渡る（M6 の垂木で確定。読み戻しだけがレイヤ相対になる）ので、2 階の梁に
//	Z=0 のパスを渡せばジオメトリは地面に置かれる。しかも VW はバウンドの offset を
//	「レベル Z − オブジェクト Z」で**再計算して上書きする**（M8 の 1 周目で観測）ため、
//	命令どおりの offset も失われる。**パスとバウンドが同じ絶対 Z を指す**ようにするのが
//	正しく、これは柱（M8）で実証済みの形でもある。水平材ではその Z が両端で等しいだけ。
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

	// 構造材 1 本ぶんの描画仕様。path / profile は呼び出し側が用意する（下記の CreatePath と
	// DrawUtil の CreateRectangleProfileGroup）。
	struct StructuralMemberSpec
	{
		MCObjectHandle path = nil;	  // パス（部材の芯線。2 点の NURBS 曲線）
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

	// パス＝部材の芯線（始端 → 終端）を通る 2 点の NURBS 曲線。gSDK->CreateNurbsCurve で
	// 始端 1 点の曲線を作り、gSDK->Add3DVertex（**VS の AddVertex3D にあたる**）で終端を
	// 足す。**水平材・鉛直材ともこれ 1 つ**で、違いは呼び出し側が渡す 2 点の Z だけ
	// （冒頭「Z の置き方」。Z に 0 を渡してはならない）。
	//
	// 頂点が本当に 2 つになったかを outAppended に返す（診断用。ここが崩れると PIO は
	// パスを挿入点としてしか読まず、長さ 0 で何も描かれない）。作れなければ nil。
	MCObjectHandle CreatePath(const core::Vec3& start, const core::Vec3& end, bool& outAppended);

	// 構造材ツールの PIO を 1 つ生成して仕様どおりに設定する。style が 0 ならスタイルを
	// 関連付けずに描く（スタイルの欠落で部材を失わない）。
	StructuralMemberResult DrawStructuralMember(const StructuralMemberSpec& spec, RefNumber style);
} // namespace HomeskzIfcImport::draw
