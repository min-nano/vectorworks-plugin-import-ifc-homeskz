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

#include "VWFC/VWObjects/VWParametricObj.h"

#include <cstddef>
#include <functional>
#include <string>

namespace HomeskzIfcImport::draw
{
	namespace
	{
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
			const MCObjectHandle object = gSDK->CreateCustomObject(
				TXString(kShearWallUniversalName), WorldPt(wall.start.x, wall.start.y), 0.0, true);
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
				putReal(kParamShearPanelOffset, wall.panelOffset);
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

		// **1 枚も作る前に、PIO の定義を「設定ダイアログを出さない」で作っておく。**
		// CreateCustomObject は定義が無ければ既定（kCustomObjectPrefAlways）で作るので、
		// 最初の 1 個だけダイアログが出てインポートが止まる（柱記号で実機確認済み。
		// draw/ColumnMark.cpp）。
		if (!document.shearWalls.empty())
			gSDK->DefineCustomObject(TXString(kShearWallUniversalName), kCustomObjectPrefNever);

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

		if (outNote != nullptr && (missingLayers > 0 || failed > 0 || unwritten > 0))
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
			*outNote = std::move(text);
		}

		return drawn;
	}
} // namespace HomeskzIfcImport::draw
