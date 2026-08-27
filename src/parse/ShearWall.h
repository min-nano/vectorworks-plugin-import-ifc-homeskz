//
//	parse/ShearWall.h
//
//	Phase 1（IFC 解析）の耐力壁モジュール（docs/DEV-NOTES.md M19「耐力壁」）。筋かい
//	（IfcMember "筋かい…"）と面材（IfcWall "面材…"）を、**両端を柱に紐付けた線分 PIO
//	1 つ**（core::ShearWallCommand）へ変換する。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。STEP エンティティ
//	グラフ（parse/Step）・幾何（parse/IfcGeometry）・ストーリ（parse/Story）・柱の命令
//	（parse/Column）だけで完結する（CLAUDE.md「Phase 1」）。
//
//	解析の要点:
//	  * **壁の向きは押し出し方向から決まる。** 筋かいも面材も「壁面内の断面を壁面に
//	    直交する向きへ押し出した」ソリッドなので、押し出し方向 extrudeDir が
//	    そのまま**平面の法線**になり、それに直交する向きが壁の軸になる。平面 footprint
//	    の外接矩形から向きを推し量る（＝対角線を軸と取り違える）必要が無い。
//	    押し出しが水平でない要素は耐力壁として解釈できないので捨てる。
//	  * **筋かいの見付け幅は壁面内で測る。** 押し出しが法線方向なので、壁面の座標系
//	    （軸方向 s ・高さ z）へ落とした断面外形は IFC のプロファイルを回転しただけの形に
//	    なる。筋かいは長い帯なので、その外形を回転キャリパで測った**最小の幅**が
//	    見付け幅（45×90 の 90）で、法線方向の広がりがそのまま材厚（45）になる。
//	  * **たすき掛けは同名 2 要素**。ホームズ君は "筋かいダブル:1FL_18" を**同じ Name の
//	    IfcMember 2 本**（互いに逆向きに傾く）として出す。Name でまとめて 1 枚の耐力壁にする。
//	  * **表裏の面材は同じ位置の 2 枚**。大壁の両面に面材を張ると、同じ軸・同じ区間の
//	    IfcWall が法線の正負に 1 枚ずつ出る。これも 1 枚の耐力壁（両面）にまとめる。
//	  * **両端の柱は柱の命令から引く**（IFC を引き直さない）。すでに組み立て終えた
//	    core::ColumnCommand のうち、その階を base とする span レイヤ（"{i+1}to…-柱"）に
//	    載るものから、軸の両端に最も近い柱を選んで**柱芯**を命令の start / end にする。
//	    柱が見つからない端は要素自身の端をそのまま使う。
//	  * 配置先は階ごとの専用レイヤ "n-耐力壁"（レイヤ平面＝その階の横架材天端。最上階は軒高）。
//	    高さは**レイヤ平面からの相対**で持つ（core::ShearWallCommand「高さの持ち方」）。
//
//	【表と裏】面材をどちらの面に張っているかは、**軸（start→end）を見て左手側が表**という
//	幾何の約束で決める。IFC には内外の区別が無いので実世界の意味は持たせられないが、
//	両面に張った 2 枚を**ハッチングの向きで見分ける**にはどちら側かを決定的に言えれば足りる。
//	そのため start / end は「(x, y) の辞書順で小さい方が start」に固定する——列挙順で軸が
//	反転すると、同じ面材が取り込むたびに表になったり裏になったりする。
//

#pragma once

#include "core/Document.h"
#include "core/Geometry.h"
#include "parse/Step.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// 耐力壁レイヤのレベル種別名。**実体は core/Document.h**（命令セットの語彙）で、ここは
	// 解析側の再公開（parse/Story.h が kLevelFL 等を再公開しているのと同じ形）。
	inline constexpr const char* kLevelShearWall = core::kLevelShearWall;

	// 要素を識別する Name 接頭辞。**"筋かいダブル" は "筋かい" で始まる**ので、判定は
	// 長い方を先に見る。
	inline constexpr const char* kBracePrefix = "筋かい";
	inline constexpr const char* kDoubleBracePrefix = "筋かいダブル";
	inline constexpr const char* kPanelPrefix = "面材";

	// 押し出し方向を**水平**とみなす Z 成分の上限。耐力壁は壁面に直交する向き（＝水平）へ
	// 押し出したソリッドで、そうでないものはここでは解釈できない。
	inline constexpr double kShearWallHorizontalTol = 0.1;

	// 軸の端点と柱芯を対応づける平面距離の上限（mm）。柱芯は要素の端（柱の内側面）から
	// 半柱幅ぶん外にあるので、太めの柱（150 角）でも届くだけの余裕を取る。
	inline constexpr double kShearWallColumnTol = 300.0;

	// 同じ耐力壁とみなす軸方向の許容（mm）。表裏の面材（同じ軸・同じ区間で法線の正負に
	// 1 枚ずつ）を 1 枚へまとめる判定と、面材が軸のどちら側にあるかの判定に使う。
	inline constexpr double kShearWallMergeTol = 5.0;

	// 表裏の面材とみなす**法線方向**の距離の上限（mm）。同じ通りに並ぶ 2 枚の壁は軸も
	// 軸方向の区間も一致しうる（間口の同じ部屋が並ぶだけで起こる）ので、軸方向の一致だけで
	// まとめると離れた 2 枚が 1 枚になってしまう。表裏はせいぜい「柱幅＋板厚」しか離れない。
	inline constexpr double kShearWallPairOffsetTol = 300.0;

	// 要素が筋かい（Name が "筋かい" 始まりの IfcMember）か。
	bool isShearBrace(const Entity& element);

	// 要素がたすき掛けの筋かい（Name が "筋かいダブル" 始まり）か。isShearBrace が真の
	// ものの部分集合。
	bool isDoubleBrace(const Entity& element);

	// 要素が面材（Name が "面材" 始まりの IfcWall）か。
	bool isShearPanel(const Entity& element);

	// 壁面の座標系で表した耐力壁要素 1 つの姿。s は軸方向、n は法線方向の**絶対座標を
	// 軸／法線へ射影した値**なので、平面の点は axis·s + normal·n で復元できる
	// （axis ⊥ normal・どちらも単位ベクトル）。
	struct ShearWallPiece
	{
		core::Vec2 axis;		// 壁の向き（単位。押し出し方向に直交）
		core::Vec2 normal;		// 壁面の法線（単位。押し出し方向）
		double sMin = 0.0;		// 軸方向の範囲
		double sMax = 0.0;		//
		double offset = 0.0;	// 法線方向の中心（材の中心面）
		double zBottom = 0.0;	// ストーリ相対の下端 Z
		double zTop = 0.0;		// 同 上端
		double thickness = 0.0; // 材厚（壁面に直交する厚み）
		double width = 0.0;		// 筋かいの見付け幅（面材では 0）
		bool risesToMax = true; // 筋かいが sMax 側で高いか（面材では未使用）
	};

	// 要素の押し出しソリッドから ShearWallPiece を求める。押し出しを解決できない・
	// 押し出しが水平でない・縮退している要素は false（out は変更しない）。
	// brace が真なら壁面内の見付け幅も求める（面材は幅を使わないので測らない）。
	bool resolveShearWallPiece(const Model& model, const Entity& element, bool brace,
							   ShearWallPiece& out);

	// STEP Model から耐力壁の描画命令を組み立てる。
	//
	// FL ストーリ（parse/Story の collectStories）を Elevation 昇順に走査し、各階に含まれる
	// 筋かい・面材を解析する。並びは 階（Elevation 昇順）→ 階内は要素の出現順（#id 昇順の
	// rel 由来）で、エンティティ列挙順に依存しない決定的な結果になる。解決できない要素は
	// スキップする（1 枚の欠損で全体を止めない。CLAUDE.md「エラーハンドリング」）。
	//
	// columns は端点に対応づける柱の命令（**span レイヤ名から base ストーリを読む**ので、
	// parse/Column が組み立てたものをそのまま渡す）。省略したオーバーロードは内部で組み立てる。
	std::vector<core::ShearWallCommand> buildShearWallCommands(const Model& model);
	std::vector<core::ShearWallCommand>
	buildShearWallCommands(const Model& model, const std::vector<core::ColumnCommand>& columns);

	// 同上。共有コンテキストのストーリ一覧・センタリング中心・階の要素・柱の命令を使う
	// （parse/Context.h）。**Context 自身がこの結果をキャッシュする**（Context::shearWalls）
	// ので、ストーリ（耐力壁レベルを作るか）と Document の shearWalls が同じ 1 回の解析結果を
	// 共有する。
	std::vector<core::ShearWallCommand> buildShearWallCommands(Context& context);
	std::vector<core::ShearWallCommand>
	buildShearWallCommands(Context& context, const std::vector<core::ColumnCommand>& columns);

	// 耐力壁の命令が layer に 1 枚でもあるか。parse/Story が「その階に耐力壁レベルを作るか」を
	// 決めるのに使う（母屋・登り梁と同じく、**命令があるときだけ・命令があれば必ず**レイヤが
	// できる形にするため）。
	bool anyShearWallOnLayer(const std::vector<core::ShearWallCommand>& walls,
							 const std::string& layer);
} // namespace HomeskzIfcImport::parse
