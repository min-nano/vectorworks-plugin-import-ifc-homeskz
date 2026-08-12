//
//	parse/Story.h
//
//	Phase 1（IFC 解析）のストーリモジュール。Python 版 ifc/story.py に対応する。
//	IfcBuildingStorey を辿ってストーリ（階）・ストーリレベル・デザインレイヤの生成
//	命令（core::StoryCommand）を組み立てる。以降の要素（横架材・柱・床…）はここで
//	作られたレベルに高さをバインドして配置されるため、ストーリは幾何の土台（M2）に
//	続く共有基盤になる（ROADMAP.md M3）。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。STEP エンティティ
//	グラフ（parse/Step の Model）だけで完結し、通常の C++ ツールチェインでコンパイル・
//	単体テストできる（CLAUDE.md「Phase 1」）。
//
//	ホームズ君 IFC の高さ表現ルール（Python 版 ifc/story.py と一致させる）:
//	  * ストーリは名前が "FL" で終わる IfcBuildingStorey だけを対象にする（"設計GL" 等の
//	    参照高は VW のストーリにしない。残すと既定高さ 0 のストーリが複数できて衝突する）。
//	  * Elevation がストーリ高さ。Elevation 昇順に並べ、最上階を「屋根」とみなす。
//	  * 横架材天端オフセットは、その階に属する IfcColumn / IfcSlab のローカル配置 Z の
//	    「0 以下の最大値」（＝床に最も近い負のオフセット）。列挙順に依存しない決定値。
//
//	【現在のスコープ】基本レベル（一般階=FL＋横架材天端、最上階=軒高）に、要素を導入した
//	マイルストーンのレベルを足していく:
//	  * M5 床板 … 屋根階に床版（ロフト）があるときだけ FL レベル（軒高 + 36mm）
//	  * M6 屋根組 … 屋根版を含む階に 垂木・野地板 レベル（横架材天端／軒高の直上）
//	  * M7 横架材 … 母屋・登り梁の命令がある階に 母屋・登り梁 レベル（同じく直上。判定は
//	    parse/Member が組み立てた命令の配置先レイヤ。Story.cpp の「Python 版との差異」参照）
//	柱の span レベルは M8 で柱を導入するときに足す（Python 版はこれらを常時組み込むが、
//	本移植は要素ごとの縦切りで積み上げるため、描画対象の無いレベルは先に作らない＝
//	空レイヤを作らない。ROADMAP.md M3「階・レベル・レイヤ（基本レベルのみ）」）。
//

#pragma once

#include "core/Document.h"
#include "parse/Step.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// レベル種別名。Python 版 ifc/story.py の LEVEL_FL / LEVEL_BEAM_TOP / LEVEL_EAVES と
	// 一致させる。CreateLayerLevelType へ登録し GetLayerForStory でレイヤを取り直す鍵で、
	// デザインレイヤ名の接尾辞（"1-FL" の "FL"）も同じ名前になる。
	//
	// **文字列の定義は core/Document.h（命令セットの語彙）にあり、ここはその再公開**。
	// 床（parse/Floor）は高さ基準のレベル名としてこれを参照する（かつては Story.cpp と
	// Floor.cpp が各々ローカルに持っており、片方だけ直すと SetObjectStoryBound が解決
	// できないレベルを指す形になっていた）。屋根組のレベル名は同じ流儀で parse/Rafter の
	// kLevelTaruki・parse/Roof の kLevelNojiita が再公開する。
	inline constexpr const char* kLevelFL = core::kLevelFL;
	inline constexpr const char* kLevelBeamTop = core::kLevelBeamTop;
	inline constexpr const char* kLevelEaves = core::kLevelEaves;

	// 柱・小屋束を配置する span（またぐレベル区間）レイヤの接尾辞（Python 版
	// LAYER_COLUMN_SUFFIX）。レイヤ名は "{from}to{to}-柱" で、from は柱が立つ床レベル
	// （1 始まり・GL=0）、to は上端が届く床／屋根面レベル（parse/Column の
	// resolveColumnToLevel）。伏図が切断レベルで表示レイヤを絞れるようにするための分け方。
	inline constexpr const char* kColumnLayerSuffix = "柱";

	// span レベルから柱のデザインレイヤ名 "{from}to{to}-柱" を組み立てる（Python 版
	// span_layer_name）。整数レベルは小数点なし・半整数は ".5" 付き（"1to2-柱" /
	// "2to2.5-柱"）。**レイヤ名の規約はここが唯一**で、柱がレイヤを名乗るときと、
	// ストーリがそのレベル（＝レイヤ）を作るときの両方がこれを通る。
	std::string spanLayerName(double fromLevel, double toLevel);

	// span レベル 1 つの表記（Python 版 _fmt_span_level）。整数は小数点なし・半整数は
	// ".5" 付き（1 → "1"、2.5 → "2.5"）。**この表記の定義はここが唯一**で、span 柱レイヤ
	// （spanLayerName）と伏図記号レイヤ（parse/ColumnMark の planMarkLayerName）の両方が
	// これを通る（別々に書くと "2.5-柱伏図記号" と "2.5to…-柱" の桁がズレたときに、
	// 伏図の表示レイヤ探索が黙って失敗する形になる）。
	std::string formatSpanLevel(double value);

	// "{from}to{to}-柱" レイヤ名を (from, to) へ分解する（Python 版 parse_span_layer）。
	// span 柱レイヤでなければ false（接尾辞が違う・"to" が無い・数値でない）。
	bool parseSpanLayer(const std::string& name, double& outFrom, double& outTo);

	// IfcProduct（要素）のローカル配置 Z 座標を取り出す。取得できれば outZ に入れて
	// true、ObjectPlacement が無い／IfcLocalPlacement でない／座標が足りない等で
	// 取れなければ false（Python 版 get_local_placement_z 相当）。親 PlacementRelTo は
	// 辿らず RelativePlacement の Location.Z だけを見る（M2 の resolveObjectPlacement と
	// 同じく、階高は描画フェーズのストーリで反映するため親配置を合成しない）。
	bool getLocalPlacementZ(const Model& model, const Entity& element, double& outZ);

	// デザインレイヤ名の接頭辞（＝ CreateStory の接尾辞）を返す（Python 版 layer_prefix_for /
	// story_suffix_for）。一般階は "{index+1}"、最上階は "R"（屋根）。床・部材の配置先
	// レイヤ名（"1-FL" / "R-軒高" …）を組み立てるのに使う。
	std::string storyLayerPrefix(std::size_t index, bool isTop);

	// 配置先デザインレイヤ名 "{接頭辞}-{レベル種別}" を組み立てる（"1-FL" / "R-軒高" /
	// "2-垂木" …）。レイヤ名の規約を 1 か所に固定するためのヘルパーで、ストーリがレベルを
	// 作るときと、各要素が配置先を引くときの**両方**がこれを通る（規約がズレると要素の
	// レイヤ探索が黙って失敗し、命令はあるのに 1 つも描かれない形になる）。
	std::string storyLayerName(std::size_t index, bool isTop, const std::string& levelType);

	// 階（#storeyId）に属する要素の #id を返す（Python 版 storey.ContainsElements →
	// RelatedElements に相当）。IfcRelContainedInSpatialStructure を逆参照から辿り、
	// RelatingStructure が当該階のものだけを採る。並びは rel の #id 昇順・rel 内は
	// RelatedElements の記述順で、エンティティ列挙順に依存しない決定的な結果になる。
	std::vector<int> collectStoryElements(const Model& model, int storeyId);

	// 階（#storeyId）に属する IfcColumn / IfcSlab から横架材天端の相対オフセット
	// （FL からの負値）を求める（Python 版 resolve_beam_top_offset 相当）。ローカル
	// 配置 Z が負の要素のうち最大値（床に最も近接した負のオフセット）を返す。最初に
	// 見つかった値ではなく最大値を採るため、エンティティ列挙順に依存しない決定的な
	// 結果になる。候補が無ければ 0.0。
	double resolveBeamTopOffset(const Model& model, int storeyId);

	// 同上。共有コンテキストの要素一覧を使う（parse/Context.h）。
	double resolveBeamTopOffset(Context& context, int storeyId);

	// 収集したストーリ 1 件（Python 版 collect_stories の要素 (elevation, offset) に対応）。
	struct StoryInfo
	{
		int id = 0; // IfcBuildingStorey の #id（要素探索・決定的 tie-break 用）
		double elevation = 0.0; // Elevation（ストーリ高さ。mm）
		double beamOffset = 0.0; // 横架材天端オフセット（負値。最上階は未使用で 0）
		bool isTop = false; // 最上階（Elevation 最大）＝「屋根」か
	};

	// IFC からストーリ情報を Elevation 昇順で集める（Python 版 collect_stories 相当）。
	// 名前が "FL" で終わる IfcBuildingStorey だけを対象にし、末尾を最上階（isTop）と
	// する。Elevation が同値の階は #id 昇順で安定に並べる（列挙順に依存しない決定性）。
	std::vector<StoryInfo> collectStories(const Model& model);

	// 同上。共有コンテキストの要素一覧を使う（parse/Context.h）。
	std::vector<StoryInfo> collectStories(Context& context);

	// STEP Model から story 命令を組み立てる（Python 版 build_story_commands 相当。
	// ただし M3 は基本レベルのみ。ヘッダ冒頭「M3 のスコープ」参照）。ストーリを一つも
	// 検出できなければ空を返す（1 要素の欠損で全体を止めない。CLAUDE.md「エラーハンドリング」）。
	std::vector<core::StoryCommand> buildStoryCommands(const Model& model);

	// 同上。共有コンテキストのストーリ一覧・要素一覧・ロフト床を使う（parse/Context.h）。
	std::vector<core::StoryCommand> buildStoryCommands(Context& context);
} // namespace HomeskzIfcImport::parse
