//
//	draw/ShearWall.cpp
//
//	耐力壁の設置の実装。意図は draw/ShearWall.h と Extensions/ExtShearWall.h を参照。
//	【SDK 依存】PluginPrefix.h を include するため、この翻訳単位はプラグインビルド
//	（SDK あり）でのみコンパイルされる（CLAUDE.md「依存の向きは厳守する」）。
//
//	手順: 配置先レイヤを用意 → CreateCustomObject で PIO を作る → **両端を柱芯へ置く**
//	（VWParametricObj::SetLinearObjectPos）→ 本体のクラスを設定 → パラメータを書く →
//	ResetObject。リセットで PIO 本体（Extensions/ExtShearWall）が柱を探して絵を描く。
//
//	**パラメータは PIO 本体と同じ名前**でなければ黙って無視される（M6 の垂木で実証済み。
//	draw/DrawUtil の ResolveParamName の doc コメント）。名前の定義は 1 か所に集めたいので、
//	Extensions/ExtShearWall.h の kParamShear* を include して共有する。
//

#include "PluginPrefix.h"
#include "draw/ShearWall.h"
#include "draw/DrawUtil.h"
#include "Extensions/ExtShearWall.h"
#include "core/Document.h"
#include "core/Progress.h"
#include "core/Trace.h"

#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWSymbolDefObj.h"

#include <cmath>
#include <cstddef>
#include <functional>
#include <numbers>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// ------------------------------------------------------------------
		// 伏図記号のシンボル定義（PIO はこれを名前で置くだけ。Extensions/ExtShearWall.h）
		//
		// 【なぜプラグインが図面リソースを作るのか】記号は**利用者が描き直せる形**で
		// 図面に持たせたい、というのがご要望（M19 の実機確認）。シンボルにしておけば
		// 1 か所直せば全部の耐力壁に効き、用紙基準（ovSymDefPageBased）にできるので
		// **縮尺非追従**にもなる。CLAUDE.md「既存の図面リソースを作らない」の唯一の例外で、
		// 　* **無いときだけ作る**（在れば利用者が描き直したものとして触らない）、
		// 　* 作るのは伏図記号のシンボル 3 つだけ、
		// という 2 つの縛りを守る。

		// 記号の寸法（**用紙 mm**）。用紙基準シンボルなので、この数字がそのまま紙の上の
		// 大きさになる（1/50 でも 1/100 でも同じ）。1/50 の伏図で読める最小限として、
		// 三角は 6×3mm・丸は直径 3mm にしてある。
		constexpr double kMarkTriangleLength = 6.0; // 壁と平行な脚（＝斜辺の水平投影）
		constexpr double kMarkTriangleHeight = 3.0; // 壁に直交する脚（＝直角を立てる側）
		constexpr double kMarkCircleDiameter = 3.0;

		// 図形 1 つをシンボル定義の中へ入れ、記号の作図クラスを与える。入ったら true。
		// **VWFC で作った図形はどのコンテナにも入らない**ので AddObject（中身は
		// AddObjectToContainer）で入れ直す（draw/Symbol.cpp の 1 番目の作法）。
		// gSDK->Create* で作ったものはアクティブレイヤに入っているので、同じ呼び出しで
		// 定義へ移る。**入ったかどうかを必ず確かめる**——空のシンボルが図面に残ると、
		// 次からは「在る」と見なして直せなくなる（下記 EnsureSymbol）。
		bool AddToSymbol(MCObjectHandle shape, VWSymbolDefObj& definition)
		{
			if (shape == nil)
				return false;
			definition.AddObject(shape);
			SetClassByName(shape, kShearMarkClass);
			SetAllAttributesByClass(shape);
			return definition.GetFirstMemberObject() != nil;
		}

		// 筋かい記号（直角三角形）1 つ分の頂点。挿入点は**壁と平行な脚の中央**で、
		// 三角は +Y 側（記号を寄せた側のさらに外）へ立つ。直角は「材が高くなる側」の端に
		// 置くので、斜辺の傾きがそのまま筋かいの向き（足元→頂部）を表す。
		std::vector<core::Vec2> BraceTrianglePoints(bool rise)
		{
			const double half = kMarkTriangleLength / 2.0;
			const double foot = rise ? -half : half;
			const double head = rise ? half : -half;
			return {core::Vec2{foot, 0.0}, core::Vec2{head, 0.0},
					core::Vec2{head, kMarkTriangleHeight}};
		}

		// シンボル定義を 1 つ用意する。中身のあるシンボルが在れば true（何もしない）。
		//
		// 【触る／触らないの境目は「中身が在るか」】名前で在る／無いだけを見て打ち切ると、
		// **空のまま登録されたシンボルを二度と直せない**——実機で実際にそうなった（M19。
		// 名前だけ作って中身を入れ損ねた 3 つが図面に残り、次の取り込みでは「在る」と
		// 見なされて空のままだった）。中身が 1 つでも入っていれば利用者が描いたもの
		// （あるいは前回入れたもの）なので触らず、**空なら描き直す**。
		//
		// 名前で拾って無ければ作るのは VWSymbolDefObj の構築子がやってくれる
		// （GS_GetNamedObject → 無ければ GS_CreateSymbolDefinition。SDK のソースで確認）。
		// **作れたかどうかを名前の一致で判定しない**——VW が返す名前は正規化などで元の
		// 綴りと一致しないことがあり、そこで打ち切ると「名前だけ在って空」を招く。
		bool EnsureSymbol(const char* name, const std::function<MCObjectHandle()>& makeShape)
		{
			try
			{
				VWSymbolDefObj definition{TXString(name)};
				if (definition.GetThisObject() == nil)
				{
					core::trace::log(std::string("shearwall: シンボル定義を作れない（") + name +
									 "）");
					return false;
				}
				if (definition.GetFirstMemberObject() != nil)
					return true; // 中身が在る＝そのまま使う

				if (!AddToSymbol(makeShape(), definition))
				{
					core::trace::log(std::string("shearwall: シンボルへ図形を入れられない（") +
									 name + "）");
					return false;
				}
				// **用紙基準＝縮尺非追従**（この 1 行がご要望の本体）。
				definition.SetPageBased(true);
				return true;
			}
			catch (...)
			{
				core::trace::log(std::string("shearwall: シンボル定義で例外（") + name + "）");
				return false;
			}
		}

		// 伏図記号のシンボル 3 つを用意する。用意できなかった数を返す（診断へ出す）。
		std::size_t EnsureMarkSymbols()
		{
			std::size_t failed = 0;
			const auto ensure =
				[&failed](const char* name, const std::function<MCObjectHandle()>& makeShape)
			{
				if (!EnsureSymbol(name, makeShape))
					++failed;
			};

			ensure(kShearSymbolBraceRise,
				   [] { return CreateClosedPolygon(BraceTrianglePoints(true)); });
			ensure(kShearSymbolBraceFall,
				   [] { return CreateClosedPolygon(BraceTrianglePoints(false)); });
			ensure(kShearSymbolPanel,
				   []
				   {
					   const double radius = kMarkCircleDiameter / 2.0;
					   WorldRect bounds;
					   bounds.left = -radius;
					   bounds.right = radius;
					   bounds.bottom = -radius;
					   bounds.top = radius;
					   return gSDK->CreateOval(bounds);
				   });
			return failed;
		}

		// ------------------------------------------------------------------
		// 種別・掛け方・面の値をパラメータの綴りへ落とす（Extensions/ExtShearWall.h の
		// kShear* が唯一の定義）。
		const char* KindValue(core::ShearWallKind kind)
		{
			return kind == core::ShearWallKind::Panel ? kShearKindPanel : kShearKindBrace;
		}

		const char* BraceStyleValue(core::ShearWallBraceStyle style)
		{
			return style == core::ShearWallBraceStyle::Double ? kShearBraceDouble
															  : kShearBraceSingle;
		}

		const char* PanelSideValue(core::ShearWallPanelSide side)
		{
			switch (side)
			{
			case core::ShearWallPanelSide::Back:
				return kShearSideBack;
			case core::ShearWallPanelSide::Both:
				return kShearSideBoth;
			case core::ShearWallPanelSide::Front:
				break;
			}
			return kShearSideFront;
		}

		// 耐力壁 1 枚を置く。PIO を作って両端とパラメータを書き、リセットまでできたら true。
		// 書けなかったパラメータの数を outUnwritten に足す。
		//
		// ★**パラメータは 1 つずつ独立に書き、1 つ書けなくても残りとリセットを諦めない。**
		// VWFC の setter は名前が通らないと例外を投げるので、まとめて 1 つの try に入れると
		// **最初の 1 つで残り全部と ResetObject までが飛ぶ**——PIO は図面に残るのに絵が
		// 1 つも描かれない、という「命令はあるのに見えない」最悪の形になる（M19 のローカル
		// 確認で実際にこうなった。docs/DEV-NOTES.md M19「パラメータが 1 つ通らないと…」）。
		bool PlaceOne(const core::ShearWallCommand& wall, std::size_t& outUnwritten)
		{
			// 挿入点は始端（柱芯）。第 4 引数 bInsert=true でアクティブレイヤへ入れる。
			// 線分 PIO なので、この後 SetLinearObjectPos で両端を与え直す。
			//
			// ★**角度もここで与える**（始端→終端の向き）。線分 PIO として置けていれば
			// 両端がそのまま向きを決めるので角度は要らないが、**万一 1 点のオブジェクトと
			// して置かれても、ローカル +X が壁の向きに揃う**——PIO 側は絵をローカル座標で
			// 描くので、この 1 つで「向きだけ違う」という直しにくい壊れ方を塞げる。
			const double angle = std::atan2(wall.end.y - wall.start.y, wall.end.x - wall.start.x) *
								 180.0 / std::numbers::pi;
			const MCObjectHandle object =
				gSDK->CreateCustomObject(TXString(kShearWallUniversalName),
										 WorldPt(wall.start.x, wall.start.y), angle, true);
			if (object == nil)
				return false;

			// PIO 本体のクラス（筋かい／耐力面材）。PIO が描く帯・面はこのクラスの属性で
			// 描かれる（面材の表裏と伏図の記号だけは PIO 側でクラスを分ける）。
			SetClassByName(object, wall.drawClass);

			bool placed = false;
			std::size_t unwritten = 0;
			try
			{
				VWParametricObj pio(object);

				// 1 つ書くたびに握る。失敗は数えるだけで、次のパラメータへ進む。
				const auto write = [&unwritten](const std::function<void()>& put)
				{
					try
					{
						put();
					}
					catch (...)
					{
						++unwritten;
					}
				};

				// **両端＝柱芯**。ここが耐力壁の「どの柱とどの柱の間か」を表す。
				write(
					[&]
					{
						pio.SetLinearObjectPos(VWPoint2D(wall.start.x, wall.start.y),
											   VWPoint2D(wall.end.x, wall.end.y));
						placed = true;
					});

				const auto putString = [&](const char* name, const TXString& value)
				{ write([&] { pio.SetParamString(name, value); }); };
				const auto putReal = [&](const char* name, double value)
				{ write([&] { SetParamRealChecked(pio, TXString(name), value); }); };

				putString(kParamShearTargetLayers, TXString(wall.targetLayers.c_str()));
				putString(kParamShearKind, TXString(KindValue(wall.kind)));
				putString(kParamShearBraceStyle, TXString(BraceStyleValue(wall.braceStyle)));
				putString(kParamShearBraceRise,
						  TXString(wall.braceRisesToEnd ? kShearRiseEnd : kShearRiseStart));
				putString(kParamShearPanelSide, TXString(PanelSideValue(wall.panelSide)));
				putReal(kParamShearWidth, wall.width);
				putReal(kParamShearClearSpan, wall.clearSpan);
				putReal(kParamShearBottom, wall.bottomHeight);
				putReal(kParamShearTop, wall.topHeight);
			}
			catch (...)
			{
				// PIO のラッパーそのものを作れなかった（＝パラメトリックでない）。
				return false;
			}

			outUnwritten += unwritten;

			// **リセットは必ず呼ぶ。** ここが本体の Recalculate を呼び、耐力壁が描かれる。
			// パラメータを取りこぼしていても、描けるところまでは描かせる。
			gSDK->ResetObject(object);
			return placed;
		}
	} // namespace

	std::size_t drawShearWalls(const core::Document& document, core::ProgressReporter& progress,
							   std::string* outNote)
	{
		std::size_t drawn = 0;
		std::size_t missingLayers = 0;
		std::size_t failed = 0;
		std::size_t unwritten = 0;
		std::size_t missingSymbols = 0;

		// **1 枚も作る前に、PIO の定義を「設定ダイアログを出さない」で作っておく。**
		// CreateCustomObject は定義が無ければ既定（kCustomObjectPrefAlways）で作るので、
		// 最初の 1 個だけダイアログが出てインポートが止まる（柱記号で実機確認済み。
		// draw/ColumnMark.cpp）。
		if (!document.shearWalls.empty())
			gSDK->DefineCustomObject(TXString(kShearWallUniversalName), kCustomObjectPrefNever);

		// **伏図記号のシンボルも 1 枚も置く前に用意する**（無ければ登録・在れば触らない。
		// 上記「伏図記号のシンボル定義」）。作れなくても耐力壁自体は置く——記号が出ない
		// だけで、内法も軸組図も読めるほうがまし。
		if (!document.shearWalls.empty())
			missingSymbols = EnsureMarkSymbols();

		for (const core::ShearWallCommand& wall : document.shearWalls)
		{
			if (progress.cancelled())
				break;
			progress.step();

			// "n-耐力壁" はストーリが作るレイヤ。無い＝その階の生成がスキップされたと
			// いうことなので、耐力壁も置かない（要素のために勝手にレイヤを作らない）。
			if (ActivateExistingLayer(wall.layer) == nil)
			{
				++missingLayers;
				continue;
			}

			if (PlaceOne(wall, unwritten))
				++drawn;
			else
				++failed;
		}

		if (outNote != nullptr &&
			(missingLayers > 0 || failed > 0 || unwritten > 0 || missingSymbols > 0))
		{
			std::string text = "耐力壁の診断: ";
			if (missingLayers > 0)
				text += "配置先レイヤを用意できない命令 " + std::to_string(missingLayers) + " 件。";
			if (failed > 0)
				text += "オブジェクトを作れなかった命令 " + std::to_string(failed) + " 件。";
			// **書けなかったパラメータは黙って捨てない。** PIO は図面に在るのに絵が痩せる
			// （最悪は何も描かれない）という、いちばん切り分けにくい症状の唯一の手掛かり。
			if (unwritten > 0)
				text += "PIO に書けなかったパラメータ " + std::to_string(unwritten) + " 個。";
			// 記号のシンボルを用意できないと、伏図に三角も丸も出ない（耐力壁自体は在る）。
			if (missingSymbols > 0)
				text += "登録できなかった伏図記号のシンボル " + std::to_string(missingSymbols) +
						" 個。";
			*outNote = std::move(text);
		}

		return drawn;
	}
} // namespace HomeskzIfcImport::draw
