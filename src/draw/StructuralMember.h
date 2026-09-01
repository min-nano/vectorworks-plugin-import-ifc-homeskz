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
//	【パスも共通】水平材も鉛直材も本質は**3 次元空間の直線 1 本**なので、パスの作り方も分けな
//	い。どちらも 2 点の NURBS 曲線（`CreateNurbsCurve` ＋ `Add3DVertex`＝VS の`AddVertex3D`）
//	で作る（CreatePath）。M7 の横架材が 2D ポリラインだったのは「ISDK には NURBS
//	曲線へ頂点を足す呼び出しが無い」と誤認していたためで、M8 の柱で `Add3DVertex`
//	が見つかりその前提は消えた。要素ごとに違うのは**2 点の Z をどう置くか**だけで、
//	それは下記のとおり部材の仕様そのものである。
//
//	【Z の置き方（＝呼び出し側が決める唯一の差）】
//	  * 水平材（横架材）… 両端とも**同じ Z**（天端 Z）を渡す。傾斜梁の勾配は始端／終端の
//	    ストーリバウンドの offset 差だけで表し、**パスには持たせない**——構造材ツールはバウンド
//	    の高さ差をパス由来の部材長へ加算しうるので、パスにも傾斜を持たせると二重に適用される
//	    （実機で確認済み。draw/Member.cpp 冒頭）。
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
//	【端部オフセット】パスの端点は**接合相手の芯線上**にあり（横架材の芯線は天端中央を通る
//	ので、柱の上端なら受ける梁の天端、負け梁の端なら勝ち梁の天端中央線）、材が実際に止まる
//	位置は PIO の端部オフセット（OIP の「始端オフセット」「終端オフセット」。負値が材を
//	短くする）で戻す。命令が持つ値の決まり方は core/Document.h「端部オフセット」にある。
//	**このパラメータの universal 名は SDK ヘッダのどこにも無い**（ci-debug の sdk-grep で
//	確認）ので、ありうる名前とローカライズ名を並べて DrawUtil の ResolveParamNameAmong で
//	引き、解決できなければ件数と手掛かりを診断へ持ち帰る。
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
	// 構造材ツールのフィールド名のうち、**描いた構造材を後から読む側とも共有する**もの。
	// 記号 PIO（Extensions/ExtColumnMark）は対象レイヤの構造材を走査し、構造用途で柱／
	// 小屋束を見分け、断面寸法から記号の大きさを決める——つまり**ここで書いた名前で
	// 読み直す**ので、書き手と読み手が同じ定義を見る必要がある（CLAUDE.md「構造材ツールの
	// フィールド名・値・生成手順は draw/StructuralMember」）。残りのフィールド名は書き手
	// しか使わないので .cpp のまま。
	// 構造材ツールの universal 名（プラグイン名）。**描く側だけのものではない**——断面寸法
	// データタグの式（draw/Tag の TagFieldFormula）が「どのオブジェクトのどのフィールドを
	// 読むか」をこの名前で指すので、書き手と式が同じ定義を見る必要がある。
	inline constexpr const char* kStructuralMemberPlugin = "StructuralMember";

	inline constexpr const char* kFieldStructuralUse = "StructuralUse"; // 構造用途
	inline constexpr const char* kFieldMajorBreadth = "MajorBreadth";	// 断面幅
	inline constexpr const char* kFieldMajorDepth = "MajorDepth";		// 断面せい

	// universal 名で引けなかったときに使う OIP のローカライズ名（DrawUtil の
	// ResolveParamName へ渡す第 2 候補）。**書きも読みも同じ解決を通す**必要がある——
	// 日本語環境では universal 名で引けないことがあり、書き手だけがローカライズ名へ
	// 落ちると、読み手は同じ値を見つけられない。
	inline constexpr const char* kLocalizedBreadth = "幅";
	inline constexpr const char* kLocalizedDepth = "せい";

	// 断面基準点（PIO の AxisAlign）。パスが断面のどこを通るかを決めるので、**呼び出し側が
	// 渡す断面矩形の置き方と必ず一致させる**（天端中央なら原点が上辺中央、中央なら原点が
	// 断面中心、中下なら原点が下辺中央）。値は構造材ツールのポップアップのキー
	// （DrawStructuralMember 内で変換）。
	enum class StructuralAxisAlign
	{
		TopCentre,	  // 天端中央（横架材。パスは天端中央線）
		Centre,		  // 中央（柱。パスは断面中心を通る鉛直線）
		BottomCentre, // 中下（垂木。パスは下面中央線＝屋根面が通る線）
	};

	// 構造材 1 本ぶんの描画仕様。path / profile は呼び出し側が用意する（下記の CreatePath と
	// DrawUtil の CreateRectangleProfileGroup）。
	struct StructuralMemberSpec
	{
		MCObjectHandle path = nil;	  // パス（部材の芯線。2 点の NURBS 曲線）
		MCObjectHandle profile = nil; // 断面プロファイルのグループ（空は不可）
		std::string memberId;		  // 構造材 ID（OIP の「構造材 ID」）
		std::string drawClass;		  // クラス名（空ならクラスを割り当てない）
		// 構造用途のキー。値の定義は core/Document.h（kStructuralUseBeam ほか）で、
		// 横架材・柱・小屋束・垂木がある。
		std::string structuralUse;
		double width = 0.0; // 断面幅（主幅。mm）
		double depth = 0.0; // 断面せい（主せい。mm）
		StructuralAxisAlign axisAlign = StructuralAxisAlign::TopCentre;
		core::StoryBoundCommand startBound; // 始端（柱は下端）の高さ基準
		core::StoryBoundCommand endBound;	// 終端（柱は上端）の高さ基準
		// 端部オフセット（mm。負値＝材を短くする）。パスの端点は接合相手の芯線上にあり、
		// 材が実際に止まる位置はここで戻す（core/Document.h「端部オフセット」）。
		// 0 なら端点がそのまま材の端（垂木・自由端の横架材）。
		double startOffset = 0.0;
		double endOffset = 0.0;
	};

	// DrawStructuralMember の結果。**断面が入ったかを呼び出し側へ返す**のは、実描画を
	// ローカルの VectorWorks でしか確認できないため（断面が 0 だとオブジェクトはあるのに
	// 画面へ出ない。各 draw モジュールが件数を完了ダイアログへ載せる）。
	struct StructuralMemberResult
	{
		MCObjectHandle object = nil; // 生成できなければ nil（呼び出し側はフォールバックへ）
		bool sectionOk = false; // 主幅・主せいを読み戻しで確認できたか
		// 端部オフセットを書けたか（要らない＝両端 0 のときは true）。false なら材が接合
		// 相手の芯線まで伸びたまま描かれる（＝梁せい／半幅ぶん長い）ので、呼び出し側は
		// 件数を診断へ載せる。
		bool endOffsetOk = true;
		// 端部オフセットのパラメータ名を解決できなかったときだけ、PIO が持つ「オフセット」を
		// 含むパラメータ名の一覧（DescribeParamsContaining）。実機でしか読めない情報を
		// 1 周で持ち帰るための手掛かりで、解決できていれば空。
		std::string offsetParamHint;
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
