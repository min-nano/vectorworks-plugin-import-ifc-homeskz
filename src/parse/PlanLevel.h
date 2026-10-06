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
//	    （"n-登り梁"）の材は、柱と同じく映す伏図の範囲の span レイヤ（"{from}to{to}-登り梁"）
//	    へ分ける——高さを跨ぐ柱梁伏図（どれも跨がなければ水下側の柱梁伏図）と、上端が
//	    その階のいちばん上の伏図レベルに届くなら母屋伏図に映す（noboribariSpan。parse/Sheet）。
//	  * 水平な横架材が 1 本も無い階は、標準の天端（parse/Story の beamTopElevation）1 つ。
//	  * まとめたレベルの並び（伏図 1 枚ぶん）を**伏図レベル**と呼ぶ。建物全体で下から
//	    1, 2, 3 … と通し番号（ordinal）を振り、柱の span レイヤ（"{from}to{to}-柱"）と
//	    伏図記号レイヤ（"{to}-柱伏図記号"）の番号はこの通し番号になる（伏図をまとめる単位で
//	    柱のレイヤも区切る。ご要望）。**どの階も高さが 1 つなら通し番号＝階の番号**なので、
//	    スキップフロアでない建物のレイヤ名は従来と変わらない。
//
//	【標準のレベルは名前を変えない】各階で標準の天端にいちばん近い高さを含む伏図レベルを
//	**標準**とし、そこは従来どおりのレイヤ（"2-横架材天端" / "R-軒高" / "2-FL" / "2-耐力壁"）
//	へ置く。それ以外の伏図レベルは、元のレベル種別の後ろに高さの印（core::planLevelTag。
//	"(FL-872)"。高さはその階の FL、最上階は軒高から測る）を付けた別のレベル・レイヤ
//	（"2-横架材天端(FL-872)"）へ置く。レベルの高さは
//	元のレベルを伏図レベルの高さのぶんずらしたもの（planLevelShift）。材の高さ基準
//	（StoryBoundCommand）は従来どおり元のレベルを指す——レイヤが違っても同じ階のレベルは
//	指せる（柱が span レイヤに居ながら横架材天端へバインドしているのと同じ）。
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
		bool standard = false; // その階の標準の横架材天端を含むか（＝レイヤ名を変えない）
		long long datum = 0; // 高さの基準＝その階の FL（最上階は軒高）の GL からの高さ（mm）
		bool top = false; // 最上階か（基準の名前が「軒高」になる）
		std::string tag; // レベル種別・レイヤ名の印（標準は空。core::planLevelTag）

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

	// 通し番号 ordinal（span の from。整数部）の伏図レベルが属する階。見つからなければ
	// ordinal − 1 を返す（どの階も高さが 1 つのときの従来の対応）。
	std::size_t storyOfOrdinal(const std::vector<PlanLevel>& levels, double ordinal);

	// 伏図レベルのレベル種別（"横架材天端" / "横架材天端(FL-872)"）。
	std::string planLevelType(const PlanLevel& level, const std::string& levelType);

	// 伏図レベルのレイヤ名（"2-横架材天端" / "2-横架材天端(FL-872)"）。
	std::string planLevelLayer(const PlanLevel& level, const StoryInfo& story,
							   const std::string& levelType);

	// 伏図レベルの横架材レイヤ名（横架材天端・最上階は軒高）。
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

	// 登り梁を映す伏図の範囲（ご要望）。柱の span（"{from}to{to}-柱"）と同じ通し番号の範囲で
	// 表し、柱梁伏図（切断＝通し番号 + 0.25）・母屋伏図（切断＝その階のいちばん上の通し番号
	// + 0.75）は、切断を範囲に含む登り梁を映す（parse/Sheet の spanLayersAtCut）。
	//   * 柱梁伏図: その階の伏図レベルのうち、高さを跨ぐ（下端 ≤ 高さ ≤ 上端）もの。
	//   * 母屋伏図: 上端がその階のいちばん上の伏図レベルの高さ以上のもの。
	//   * どちらにも該当しなければ、低い側の端に近い伏図レベルの柱梁伏図。
	// 範囲は、柱梁伏図 o だけなら [o, o + 0.5]、母屋伏図にも映すなら to = いちばん上 + 1、
	// 母屋伏図だけなら [いちばん上 + 0.5, いちばん上 + 1]。その階に伏図レベルが無ければ false。
	bool noboribariSpan(const core::MemberCommand& member, const std::vector<PlanLevel>& levels,
						std::size_t story, double& from, double& to);

	// 横架材命令の配置先を伏図レベルのレイヤへ振り分ける。対象は横架材レイヤ
	// （beamTopLayerName）と登り梁の専用レイヤ（"n-登り梁"）に載る材で、水平な材は天端、
	// 傾いた材は低い側の端の天端にいちばん近い伏図レベルへ入れる（ヘッダ冒頭）。母屋の
	// 専用レイヤは振り分けない（母屋伏図にだけ映る）。登り梁は noboribariSpan の範囲の
	// span レイヤ（"{from}to{to}-登り梁"）へ置き、高さ基準もそのレベルへ付け替える。横架材レイヤの**軒桁**（クラスで判別）は
	// 軒桁の専用レイヤ（"n-軒桁" / "n-軒桁(FL-872)"）へ分ける（母屋伏図に薄く重ねるため。
	// 取り合いを判定するときは parse/Story の beamGroupLayer で横架材レイヤへ読み替える）。
	void assignMemberPlanLevels(std::vector<core::MemberCommand>& members,
								const std::vector<StoryInfo>& stories,
								const std::vector<PlanLevel>& levels);

	// 伏図レベルの候補（まとめる前の高さ 1 つずつ。階の昇順・高さの昇順）。設定ダイアログの
	// 「前のレベルと同じ伏図にまとめる」の行になる。IFC を読んで候補を返す入口は
	// parse/BuildDocument の scanPlanLevelChoices（draw/ が STEP の型を参照せずに呼べるように）。
	std::vector<core::PlanLevelChoice> collectPlanLevelChoices(Context& context);
} // namespace HomeskzIfcImport::parse
