//
//	parse/Story.h
//
//	Phase 1（IFC 解析）のストーリモジュール。IfcBuildingStorey を辿ってストーリ（階）
//	・ストーリレベル・デザインレイヤの生成命令（core::StoryCommand）を組み立てる。
//	以降の要素（横架材・柱・床…）はここで作られたレベルに高さをバインドして配置されるため、
//	ストーリは幾何の土台（M2）に続く共有基盤になる（docs/DEV-NOTES.md M3）。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない。STEP エンティティ
//	グラフ（parse/Step の Model）だけで完結し、通常の C++ ツールチェインでコンパイル・
//	単体テストできる（CLAUDE.md「Phase 1」）。
//
//	ホームズ君 IFC の高さ表現ルール:
//	  * ストーリは名前が "FL" で終わる IfcBuildingStorey だけを対象にする（"設計GL" 等の
//	    参照高は VW のストーリにしない。残すと既定高さ 0 のストーリが複数できて衝突する）。
//	  * Elevation がストーリ高さ。Elevation 昇順に並べ、最上階を「屋根」とみなす。
//	  * 横架材天端オフセットは、その階に属する IfcColumn / IfcSlab のローカル配置 Z の
//	    「負の値のうち最大のもの」（＝床に最も近い負のオフセット）。列挙順に依存しない決定値。
//
//	【作るレベル】基本レベル（一般階=FL＋横架材天端、最上階=軒高）（M3）に、要素を導入した
//	マイルストーンのレベルを追加している（いずれも描画するものがあるときだけ。Story.cpp の
//	「レベルを足す条件」参照）:
//	  * M5 床板 … 屋根階に床版（ロフト）があるときだけ FL レベル（軒高 + 36mm）
//	  * M6 屋根組 … 屋根版を含む階に 垂木・野地板 レベル（横架材天端／軒高の直上）
//	  * M7 横架材 … 母屋・登り梁の命令がある階に 母屋・登り梁 レベル（同じく直上。判定は
//	    parse/Member が組み立てた命令の配置先レイヤ）
//	  * M8 柱 … 実在する span 柱レイヤ（"2-1_3±0-柱"）ごとのレベル
//	  * M19 耐力壁 … 実在する耐力壁の span レイヤ（"1±0_2±0-耐力壁"）ごとのレベル
//	  * 伏図の分け方（parse/PlanLevel）… 軒桁の専用レベル、登り梁の span レベル、標準で
//	    ない伏図レベル（横架材の高さごとの伏図）の横架材・FL レベル
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

	// レベル種別名。CreateLayerLevelType へ登録し GetLayerForStory でレイヤを取り直す鍵で、
	// 伏図レベルで分けないレイヤ（"R-母屋"）は名前の接尾辞もこれ（"母屋"）。伏図レベルで
	// 分けるレイヤの名前は levelLayerSuffix（横架材天端・軒高は "横架材"）。
	//
	// **文字列の定義は core/Document.h（命令セットの語彙）にあり、ここはその再公開**。
	// 床（parse/Floor）は高さ基準のレベル名としてこれを参照する（かつては Story.cpp と
	// Floor.cpp が各々ローカルに持っており、片方だけ直すと SetObjectStoryBound が解決
	// できないレベルを指す形になっていた）。屋根組のレベル名は同じ流儀で parse/Rafter の
	// kLevelTaruki・parse/Roof の kLevelNojiita が再公開する。
	inline constexpr const char* kLevelFL = core::kLevelFL;
	inline constexpr const char* kLevelBeamTop = core::kLevelBeamTop;
	inline constexpr const char* kLevelEaves = core::kLevelEaves;
	inline constexpr const char* kLevelNokigeta = core::kLevelNokigeta;

	// 横架材のレイヤ名の名前（"2±0-横架材" の "横架材"）。**最上階も同じ名前**にする
	// （レベル種別は軒高のまま。beamTopLevelType）——柱のレイヤ（"2±0_R±0-柱"）の符号と
	// 横架材のレイヤの符号を突き合わせて読めるよう、レイヤ名は符号だけでレベルを表す（ご要望）。
	inline constexpr const char* kBeamLayerSuffix = "横架材";

	// 柱・小屋束を配置する span（またぐレベル区間）レイヤの接尾辞。レイヤ名は
	// "{from}_{to}-柱"（"2-1_3±0-柱"）で、from は柱が立つ伏図レベルの符号、to は上端が届く
	// 伏図レベルの符号（屋根面で止まる屋根束は母屋のレベル "R+1"。parse/Column）。伏図が
	// 切断レベルで表示レイヤを絞れるようにするための分け方。
	inline constexpr const char* kColumnLayerSuffix = "柱";

	// 伏図レベルの符号 "{階の接頭辞}{段数}"（"2±0" / "2-1" / "R+1"）。接頭辞は
	// storyLayerPrefix、段数はその階の標準の伏図レベルからの段数（core::planLevelStepText）。
	// **符号の規約はここが唯一**で、伏図レベルのレイヤ（levelLayerName）・span レイヤ
	// （spanLayerName）・伏図記号レイヤ（parse/ColumnMark）の頭がどれもこれになる——柱の
	// レイヤと横架材のレイヤが同じ符号で同じレベルを指す（ご要望）。
	std::string levelLabel(const std::string& prefix, double step);
	std::string levelLabel(std::size_t index, bool isTop, double step);

	// 符号を階の接頭辞と段数へ分解する。text 全体が符号（接頭辞は数字の並びか "R"）なら true。
	bool parseLevelLabel(const std::string& text, std::string& outPrefix, double& outStep);

	// 伏図レベルに属するレイヤ名 "{符号}-{名前}"（"2±0-横架材" / "2-1-FL" / "R±0-軒桁"）。
	// 名前はレベル種別から levelLayerSuffix で決める。伏図レベルで分けるレイヤ（横架材・
	// 軒桁・床）は**標準の伏図レベルでも "±0" を付ける**（ご要望）。耐力壁は柱と同じ span
	// レイヤ（"1±0_2±0-耐力壁"。spanLayerName）。伏図レベルで
	// 分けないレイヤ（母屋・垂木・野地板）は従来どおり storyLayerName（"R-母屋"）。
	std::string levelLayerName(std::size_t index, bool isTop, double step,
							   const std::string& levelType);

	// レベル種別からレイヤ名の名前を決める（横架材天端・軒高 → "横架材"。それ以外は種別の
	// まま。伏図レベルの印 "(-1)" は付いていてもいなくてもよい）。
	std::string levelLayerSuffix(const std::string& levelType);

	// span レイヤ名 "{from}_{to}-{suffix}"（"2-1_3±0-柱" / "2-1_2+1-登り梁"）。from / to は
	// 伏図レベルの符号（levelLabel）。**span レイヤ名の規約はここが唯一**で、柱・登り梁が
	// レイヤを名乗るときと、ストーリがそのレベル（＝レイヤ）を作るときの両方がこれを通る。
	std::string spanLayerName(const std::string& fromLabel, const std::string& toLabel,
							  const std::string& suffix);

	// span レイヤ名を from / to の符号へ分解する。接尾辞 suffix の span レイヤでなければ
	// false（接尾辞が違う・"_" が 1 つでない・符号として読めない）。符号を通し番号へ引き直す
	// のは parse/PlanLevel の parseSpanOrdinals（伏図レベルが要る）。
	bool parseSpanLayer(const std::string& name, const std::string& suffix, std::string& outFrom,
						std::string& outTo);

	// 柱の span レイヤ（接尾辞 "柱"）を分解する。
	bool parseSpanLayer(const std::string& name, std::string& outFrom, std::string& outTo);

	// レイヤ名の頭にある階の接頭辞（"2±0-横架材" → "2"、"2-1_3±0-柱" → "2"、"R-母屋" →
	// "R"）。span レイヤは from の符号の階で、登り梁・柱のレベルもその階に作られる
	// （parse/Story）。頭が接頭辞でなければ空文字（"共通"）。
	std::string layerStoryPrefix(const std::string& layer);

	// 伏図レベルのレイヤ（levelLayerName）の段数を "±0" に読み替えた名前（"2-1-横架材" →
	// "2±0-横架材"）。同じ階の別の伏図レベルの材を同じ群として扱うところ（仕口・継手・寸法・
	// 柱の上端の判定）がこれで比べる。伏図レベルのレイヤでなければそのまま返す。
	std::string stripLevelStep(const std::string& layer);

	// IfcProduct（要素）のローカル配置 Z 座標を取り出す。取得できれば outZ に入れて true、
	// ObjectPlacement が無い／IfcLocalPlacement でない／座標が足りない等で取れなければ false。
	// 親 PlacementRelTo は辿らず RelativePlacement の Location.Z だけを参照する（M2 の
	// resolveObjectPlacement と同じく、階高は描画フェーズのストーリで反映するため親配置を合成
	// しない）。
	bool getLocalPlacementZ(const Model& model, const Entity& element, double& outZ);

	// デザインレイヤ名の接頭辞（＝ CreateStory の接尾辞）を返す。一般階は "{index+1}"、
	// 最上階は "R"（屋根）。床・部材の配置先レイヤ名（"1±0-FL" / "R-母屋" …）と伏図レベルの
	// 符号（levelLabel）を組み立てるのに使う。
	std::string storyLayerPrefix(std::size_t index, bool isTop);

	// 伏図レベルで分けない配置先デザインレイヤ名 "{接頭辞}-{レベル種別}" を組み立てる
	// （"R-母屋" / "2-垂木" / "2-登り梁" …。伏図レベルで分けるものは levelLayerName）。レイヤ名の規約を 1 か所に固定するためのヘルパーで、ストーリがレベルを
	// 作るときと、各要素が配置先を引くときの**両方**がこれを通る（規約が食い違うと要素の
	// レイヤ探索が黙って失敗し、命令はあるのに 1 つも描画されない形になる）。
	std::string storyLayerName(std::size_t index, bool isTop, const std::string& levelType);

	// 横架材の取り合い・継手・柱の上端を判定するときの「その階の横架材レイヤ」。軒桁の専用
	// レイヤ（"2±0-軒桁" / "2-1-軒桁"。母屋伏図に薄く重ねるために分けた。parse/PlanLevel）を
	// 同じ伏図レベルの横架材レイヤ（"2±0-横架材" / "2-1-横架材"。段数は残す）へ読み替え、
	// それ以外はそのまま返す。**レイヤ一致で相手を絞るところは、どれもこれを通して比べる**
	// （通さないと軒桁と小屋梁の仕口・継手が黙って消える）。
	std::string beamGroupLayer(const std::string& layer);

	// 階（#storeyId）に属する要素の #id を返す。IfcRelContainedInSpatialStructure
	// を逆参照から辿り、RelatingStructure が当該階のものだけを採る。並びは rel の #id 昇順・
	// rel 内は RelatedElements の記述順で、エンティティ列挙順に依存しない決定的な結果になる。
	std::vector<int> collectStoryElements(const Model& model, int storeyId);

	// 階（#storeyId）に、要素判別述語 pred を満たす要素が 1 つでもあるか。屋根版の有無
	// （parse/Rafter の storyHasRoofSlab）と床版の有無（parse/Floor の storyHasFloorSlab）が
	// 同じ走査（要素一覧 → null チェック → 述語）を共有する。
	bool storyHasElement(Context& context, int storeyId, bool (*pred)(const Entity&));

	// 階（#storeyId）に属する IfcColumn / IfcSlab から横架材天端の相対オフセット（FL
	// からの負値）を求める。ローカル配置 Z が負の要素のうち最大値（床に最も近接した負の
	// オフセット）を返す。最初に見つかった値ではなく最大値を採るため、エンティティ列挙順に依
	// 存しない決定的な結果になる。候補が無ければ 0.0。
	double resolveBeamTopOffset(const Model& model, int storeyId);

	// 同上。共有コンテキストの要素一覧を使う（parse/Context.h）。
	double resolveBeamTopOffset(Context& context, int storeyId);

	// 収集したストーリ 1 件。
	struct StoryInfo
	{
		int id = 0; // IfcBuildingStorey の #id（要素探索・決定的 tie-break 用）
		double elevation = 0.0; // Elevation（ストーリ高さ。mm）
		double beamOffset = 0.0; // 横架材天端オフセット（負値。最上階は未使用で 0）
		bool isTop = false; // 最上階（Elevation 最大）＝「屋根」か
		std::string name; // IfcBuildingStorey の Name（"1FL" / "2FL" / "RFL"。高さの注記の基準名）
	};

	// その階の横架材レベルの種別名。一般階は横架材天端、最上階は軒高（最上階に横架材天端
	// レベルは無い）。**この分岐はここに 1 つだけ置く**——横架材・柱・火打・伏図の表示レイヤ
	// が同じ分岐を要り、かつて各所が三項演算子を各々書いていた（片方だけ直すと配置先と
	// バインド先のレベルが食い違う）。
	inline const char* beamTopLevelType(bool isTop)
	{
		return isTop ? kLevelEaves : kLevelBeamTop;
	}

	// その階の横架材天端（最上階は軒高＝ストーリ原点）の絶対 Z。バインド offset の基準や
	// 床下地の下端の算出に使う。最上階の beamOffset は未使用（StoryInfo の doc コメント）
	// なので加算しない。
	inline double beamTopElevation(const StoryInfo& story)
	{
		return story.isTop ? story.elevation : story.elevation + story.beamOffset;
	}

	// その階の**標準の伏図レベルの**横架材レイヤ名（"1±0-横架材" / "R±0-横架材"）。横架材・
	// 火打はまずここへ置き、伏図レベルへ振り分ける（parse/PlanLevel）。同じ階の横架材を
	// 段数によらず集めるところは、stripLevelStep を通したレイヤ名とこれを比べる。
	inline std::string beamTopLayerName(std::size_t index, const StoryInfo& story)
	{
		return levelLayerName(index, story.isTop, 0.0, beamTopLevelType(story.isTop));
	}

	// IFC からストーリ情報を Elevation 昇順で集める。名前が "FL" で終わる IfcBuildingStorey
	// だけを対象にし、末尾を最上階（isTop）とする。Elevation が同値の階は #id 昇順で安定に並
	// べる（列挙順に依存しない決定性）。
	std::vector<StoryInfo> collectStories(const Model& model);

	// 同上。共有コンテキストの要素一覧を使う（parse/Context.h）。
	std::vector<StoryInfo> collectStories(Context& context);

	// STEP Model から story 命令を組み立てる（作るレベルはヘッダ冒頭「作るレベル」参照）。
	// ストーリを一つも検出できなければ空を返す（1 要素の欠損で全体を止めない。CLAUDE.md
	// 「エラーハンドリング」）。
	std::vector<core::StoryCommand> buildStoryCommands(const Model& model);

	// 同上。共有コンテキストのストーリ一覧・要素一覧・ロフト床を使う（parse/Context.h）。
	std::vector<core::StoryCommand> buildStoryCommands(Context& context);
} // namespace HomeskzIfcImport::parse
