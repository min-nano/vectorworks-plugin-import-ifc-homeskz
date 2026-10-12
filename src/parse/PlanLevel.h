//
//	parse/PlanLevel.h
//
//	Phase 1（IFC 解析）の伏図レベル（docs/DEV-NOTES.md「横架材の高さごとに伏図を作る」）。
//	**伏図は横架材の高さごとに 1 枚作る**（ご要望）。ホームズ君の IFC は階（1FL / 2FL / RFL）
//	単位で要素を出すが、スキップフロアでは 1 つの階に横架材の天端が複数ある——例えば
//	GL+2699 と GL+3531 がどちらも 2FL に入る。伏図はそれぞれの高さの梁伏図を作成するのが基本で、
//	設計者が取り込みの設定で「前のレベルと同じ伏図にまとめる」と選んだものだけを寄せる
//	（core::ImportOptions::mergedPlanLevels）。
//
//	【SDK 非依存】parse/ は VectorWorks SDK を一切 include しない（CLAUDE.md「Phase 1」）。
//
//	【レベルの決め方】
//	  * 高さは**その階の横架材レイヤ（横架材天端・最上階は軒高）に載る水平な横架材の天端**を
//	    mm に丸めた値。**1mm でも違えば別のレベル**（まとめるかどうかは設計者が決める。
//	    ご要望）。母屋・登り梁の専用レイヤの材は参照しない——高さがもともと材ごとに違い、
//	    母屋伏図が別にある。
//	  * **傾いた材（登り梁・隅木）は水下側で切り分ける**（ご要望）——低い側の端の天端に
//	    いちばん近いレベルへ入れる。レベルを作る材にはしない。登り梁の専用レイヤ
//	    （"n-登り梁"）の材は、柱と同じく映す伏図の範囲の span レイヤ（"2-1_2+1-登り梁"）
//	    へ分ける——高さを跨ぐ柱梁伏図（どれも跨がなければ水下側の柱梁伏図）と、上端が
//	    その階のいちばん上の伏図レベルに届くなら母屋伏図に映す（noboribariSpan。parse/Sheet）。
//	  * 水平な横架材が 1 本も無い階は、標準の天端（parse/Story の beamTopElevation）1 つ。
//	  * まとめたレベルの並び（伏図 1 枚ぶん）を**伏図レベル**と呼ぶ。建物全体で下から
//	    通し番号（ordinal）を振る。柱・登り梁・耐力壁の span（parse/Column・下の
//	    noboribariSpan・parse/ShearWall）と伏図の切断（parse/Sheet）は通し番号で比べる。
//	  * **通し番号は各階のあとに 1 つ空ける**——その階のいちばん上の伏図レベルの次の番号は
//	    その階の**母屋のレベル**（moyaOrdinal。符号はいちばん上の段数 + 1、"R+1"）。屋根面で
//	    止まる小屋束の上端と母屋伏図はこのレベルで表す（ご要望。小屋束の上端を整数の符号に
//	    揃える）。例えば 1FL・2FL（2 段）・RFL なら 1 / (2) / 3, 4 / (5) / 6 / (7)。番号は
//	    伏図の図番には使わない（図番は伏図レベルの並び順。parse/Sheet）。
//
//	【レイヤ名は符号で呼ぶ】伏図レベルは**階の接頭辞と、その階の標準の伏図レベルからの段数**
//	を並べた符号で呼ぶ（"2±0" / "2-1" / "R+1"。parse/Story の levelLabel）。各階で標準の天端に
//	いちばん近い高さを含む伏図レベルを**標準**（段数 0）とし、その下は −1, −2 …、上は +1, +2 …。
//	伏図レベルで分けるレイヤ（横架材・軒桁・床・耐力壁）は "{符号}-{名前}"（"2±0-横架材" /
//	"2-1-FL"）、柱のレイヤは立つ伏図レベルと届く伏図レベルの符号を並べた "2-1_3±0-柱"
//	（ご要望）。**柱のレイヤの符号と横架材のレイヤの符号は同じレベルを指す**。
//	  * 通し番号ではなく階＋段数で呼ぶのは、スキップフロアで伏図を分けると通し番号が階の
//	    番号とかけ離れて読めなくなるから（ご要望）。
//	  * 高さの数値（"FL-872"）で呼ばないのは、利用者がレベルの高さを変えたときにレイヤ名が
//	    食い違うから（ご要望）。高さは伏図のタイトル（planLevelTitleSuffix）にだけ書く。
//	  * 通し番号と符号は伏図レベルを介して行き来する（ordinalLabel / labelOrdinal）。
//
//	【標準でないレベル】標準でない伏図レベルは、元のレベル種別の後ろに段数の印（core::
//	planLevelTag。"横架材天端(-1)"）を付けた別のレベルを持つ。レベルの高さは元のレベルを
//	伏図レベルの高さのぶんずらしたもの（planLevelShift）。材の高さ基準（StoryBoundCommand）は
//	従来どおり元のレベルを指す——レイヤが違っても同じ階のレベルは指せる（柱が span レイヤに
//	居ながら横架材天端へバインドしているのと同じ）。
//

#pragma once

#include "core/Document.h"
#include "core/ImportOptions.h"
#include "parse/Story.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::parse
{
	class Context;

	// 伏図レベル 1 つ（＝伏図 1 枚ぶんの横架材の高さのまとまり）。
	struct PlanLevel
	{
		std::size_t story = 0; // 属する FL 階（0 起点。Context::stories の添字）
		std::vector<long long> heights; // 含む天端の高さ（GL からの mm。昇順・1 つ以上）
		int ordinal = 0;				// 建物全体の通し番号（1 始まり。下から）
		bool standard = false; // その階の標準の横架材天端を含むか（段数 0）
		int step = 0; // その階の標準の伏図レベルからの段数（下が負。レイヤ名の符号）
		long long datum = 0; // 高さの基準＝その階の FL（最上階は軒高）の GL からの高さ（mm）
		bool top = false; // 最上階か（基準の名前が「軒高」になる）
		std::string tag;  // レベル種別の印（標準は空。core::planLevelTag）

		// 代表の高さ（いちばん低い天端）。レベルの高さ・レイヤの印はこれで決める。
		double height() const
		{
			return static_cast<double>(heights.front());
		}
	};

	// 各階の横架材の天端の高さ（昇順・重複なし）。添字は stories と同じ。members は
	// **レイヤを振り分ける前**の横架材命令（parse/Member の buildMemberCommands）。
	std::vector<std::vector<long long>>
	collectBeamHeights(const std::vector<StoryInfo>& stories,
					   const std::vector<core::MemberCommand>& members);

	// 伏図レベルを組み立てる（階の昇順・階の中は高さの昇順＝通し番号の順）。heights は
	// collectBeamHeights の結果で、options の「前のレベルとまとめる」に挙がった高さは直前の
	// 伏図レベルへ寄せる（階の最初の高さは寄せようがないので無視する）。
	std::vector<PlanLevel> buildPlanLevels(const std::vector<StoryInfo>& stories,
										   const std::vector<std::vector<long long>>& heights,
										   const core::ImportOptions& options);

	// 各階の**標準の横架材の高さ**（添字は stories と同じ。GL からの mm）。その階の横架材の
	// 天端の高さのうち、標準の横架材天端（beamTopElevation。柱・床版の配置から推す値）に
	// いちばん近いもの——標準の伏図レベル（名前を変えない方）が含む高さで、データタグに高さの
	// 注記を添えるかの基準（parse/Tag）。推した値そのものではなく**実在する横架材の高さ**を
	// 採るのは、推した値と横架材がずれるモデルがあるため（グレー本モデルプラン1 は横架材が
	// FL ちょうど・推した値は FL−100。推した値で比べると全部の梁に注記が付く）。伏図レベルが
	// 無い階は推した値。
	std::vector<long long> standardBeamHeights(const std::vector<StoryInfo>& stories,
											   const std::vector<PlanLevel>& levels);

	// 階 story の伏図レベル（通し番号の順）。
	std::vector<const PlanLevel*> storyPlanLevels(const std::vector<PlanLevel>& levels,
												  std::size_t story);

	// 階 story の伏図レベルのうち、高さ z（GL からの mm）にいちばん近い天端を含むもの。
	// 同じ近さなら低い方（決定的）。その階に伏図レベルが無ければ nullptr。
	const PlanLevel* nearestPlanLevel(const std::vector<PlanLevel>& levels, std::size_t story,
									  double z);

	// 階 story の伏図レベルのうち、天端が z − 1mm 以上にある最も低いもの（柱の上端が届く
	// 伏図レベル＝その柱の上に載る横架材の高さ）。どれも z より低ければ最も高いもの。
	const PlanLevel* planLevelAbove(const std::vector<PlanLevel>& levels, std::size_t story,
									double z);

	// 通し番号の比較の許容（番号は整数か、伏図の切断の + 0.25 のような小数）。
	inline constexpr double kOrdinalTol = 1e-6;

	// 伏図レベルが無い（単体テストで階だけを扱う）ときの、階 story の標準の通し番号。
	// 各階に伏図レベル 1 つと母屋のレベル 1 つを置いた番号（2s+1。母屋は 2s+2）。
	inline double standardOrdinalWithoutLevels(std::size_t story)
	{
		return (2.0 * static_cast<double>(story)) + 1.0;
	}

	// 通し番号 ordinal が属する階＝その番号以下で最も高い伏図レベルの階（母屋のレベルは
	// その階になる）。伏図レベルが無ければ standardOrdinalWithoutLevels の逆。
	std::size_t storyOfOrdinal(const std::vector<PlanLevel>& levels, double ordinal);

	// 階 story の母屋のレベルの通し番号（いちばん上の伏図レベルの次。ヘッダ冒頭）。
	double moyaOrdinal(const std::vector<PlanLevel>& levels, std::size_t story);

	// 通し番号 value を、階 story の符号で書く（"2-1" / "R+1"。段数はその階の標準の
	// 伏図レベルの通し番号との差）。span の端を書くのに使う。階に伏図レベルが無ければ
	// standardOrdinalWithoutLevels とみなして段数を出す。
	std::string ordinalLabel(const std::vector<PlanLevel>& levels, std::size_t story, double value);

	// 通し番号 value を、その番号が属する階（storyOfOrdinal）の符号で書く。柱・耐力壁の
	// span の端はこれで書く——立つ伏図レベル・届く伏図レベルの符号がそのまま横架材の
	// レイヤの符号になる。屋根束の上端（届いた階の母屋のレベル）は "R+1" のような符号になる。
	std::string spanLevelLabel(const std::vector<PlanLevel>& levels, double value);

	// 符号（"2-1" / "R+1"）を通し番号へ引き直す（ordinalLabel の逆）。符号として読めない・
	// 接頭辞の階に伏図レベルが無い（数字の接頭辞は階の番号とみなす。ordinalLabel と同じ）
	// なら false。
	bool labelOrdinal(const std::vector<PlanLevel>& levels, const std::string& label,
					  double& outOrdinal);

	// 接尾辞 suffix の span レイヤ名（"2-1_3±0-柱"）を通し番号の (from, to) へ引き直す。
	// span レイヤでない・符号を引き直せなければ false。
	bool parseSpanOrdinals(const std::vector<PlanLevel>& levels, const std::string& layer,
						   const std::string& suffix, double& outFrom, double& outTo);

	// 伏図レベルのレベル種別（"横架材天端" / "横架材天端(-1)"）。
	std::string planLevelType(const PlanLevel& level, const std::string& levelType);

	// 伏図レベルの符号（"2±0" / "2-1"。parse/Story の levelLabel）。
	std::string planLevelLabel(const PlanLevel& level, const StoryInfo& story);

	// 伏図レベルのレイヤ名（"2±0-横架材" / "2-1-FL"。parse/Story の levelLayerName）。
	std::string planLevelLayer(const PlanLevel& level, const StoryInfo& story,
							   const std::string& levelType);

	// 伏図レベルの横架材レイヤ名（"2±0-横架材"。最上階も "R±0-横架材"）。
	std::string planLevelBeamLayer(const PlanLevel& level, const StoryInfo& story);

	// 伏図レベルのレベルを、元のレベルからどれだけ上へずらすか（mm）。標準は 0、それ以外は
	// 代表の高さ − 標準の横架材天端。
	double planLevelShift(const PlanLevel& level, const StoryInfo& story);

	// 階に伏図レベルが 2 つ以上あるとき、伏図のタイトルへ添える高さ（"（FL-872）"。まとめた
	// ものは "（FL-872・FL-771）"。高さは FL、最上階は軒高から）。1 つだけなら空（タイトルは従来のまま）。
	std::string planLevelTitleSuffix(const std::vector<PlanLevel>& levels, const PlanLevel& level);

	// 登り梁が基準高さ（伏図レベルの高さ）を跨ぐとみなす許容（mm）。端がちょうど基準高さに
	// ある材は跨ぐものとする。
	inline constexpr double kNoboribariLevelTol = 1.0;

	// 登り梁を映す伏図の範囲（ご要望）。**映す伏図のレベルの範囲**を通し番号で表す
	// （柱梁伏図＝その伏図レベル、母屋伏図＝その階の母屋のレベル moyaOrdinal）。伏図は
	// 自分のレベルを範囲に含む登り梁を映す（parse/Sheet。両端を含む）。
	//   * 柱梁伏図: その階の伏図レベルのうち、高さを跨ぐ（下端 ≤ 高さ ≤ 上端）もの。
	//   * 母屋伏図: 上端がその階のいちばん上の伏図レベルの高さ以上のもの。
	//   * どちらにも該当しなければ、低い側の端に近い伏図レベルの柱梁伏図。
	// 範囲は、柱梁伏図 o だけなら [o, o]、跨ぐものは [いちばん下, いちばん上]、母屋伏図にも
	// 映すなら to = 母屋のレベル、母屋伏図だけなら [母屋, 母屋]。符号はどれも整数の段数
	// （"2-1_2-1-登り梁" / "2-1_2+1-登り梁"）。その階に伏図レベルが無ければ false。
	bool noboribariSpan(const core::MemberCommand& member, const std::vector<PlanLevel>& levels,
						std::size_t story, double& from, double& to);

	// 横架材命令の配置先を伏図レベルのレイヤへ振り分ける。対象は横架材レイヤ
	// （beamTopLayerName）と登り梁の専用レイヤ（"n-登り梁"）に載る材で、水平な材は天端、
	// 傾いた材は低い側の端の天端にいちばん近い伏図レベルへ入れる（ヘッダ冒頭）。母屋の
	// 専用レイヤは振り分けない（母屋伏図にだけ映る）。登り梁は noboribariSpan の範囲の
	// span レイヤ（"2-1_2+1-登り梁"。端の符号は材の階で書く）へ置き、高さ基準もその
	// レベルへ付け替える。横架材レイヤの**軒桁**（クラスで判別）は軒桁の専用レイヤ
	// （"2±0-軒桁" / "2-1-軒桁"）へ分ける（母屋伏図に薄く重ねるため。取り合いを判定する
	// ときは parse/Story の beamGroupLayer で横架材レイヤへ読み替える）。
	void assignMemberPlanLevels(std::vector<core::MemberCommand>& members,
								const std::vector<StoryInfo>& stories,
								const std::vector<PlanLevel>& levels);

	// 伏図レベルの候補（まとめる前の高さ 1 つずつ。階の昇順・高さの昇順）。設定ダイアログの
	// 「前のレベルと同じ伏図にまとめる」の行になる。IFC を読んで候補を返す入口は
	// parse/BuildDocument の scanPlanLevelChoices（draw/ が STEP の型を参照せずに呼べるように）。
	std::vector<core::PlanLevelChoice> collectPlanLevelChoices(Context& context);
} // namespace HomeskzIfcImport::parse
