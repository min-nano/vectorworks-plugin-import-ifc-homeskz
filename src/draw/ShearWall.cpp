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

#include <array>
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
		// 伏図記号のシンボル定義を用意する（Extensions/ExtShearWall.h の kShearMark*Symbol）。
		//
		// 【なぜプラグインがシンボルを作るのか】記号をシンボルにしておくと、図面の側で
		// 1 か所（シンボル定義）を編集するだけで**すべての耐力壁の記号を一括で差し替え・
		// 調整**できる（ご要望）。CLAUDE.md「既存の図面リソースを作らない」の唯一の例外で、
		// 作るのは**耐力壁を 1 枚でも描くときだけ**・名前が既にあれば**触らない**。
		//
		// 【★中身を入れたら ResetObject を呼ぶ】ここが M19 で 3 周かけたところ。
		// `CreateSymbolDefinition` で定義を作り `AddObjectToContainer` で図形を入れる——
		// これは**最初から正しく効いていた**（定義を辿ると多角形が実際に居る）。抜けて
		// いたのは**入れた後の `ResetObject(定義)`** で、これが無いと**定義の外接が計算
		// されない**。外接の無い定義は絵として成立せず、実機では
		//   * シンボルの 2D 編集で「すべて選択」しても何も選べない、
		//   * 配置したインスタンスの外接が無効値（大きさが無い）、
		//   * 図には何も出ない、
		// という「中身はあるのに空に見える」状態になる（docs/DEV-NOTES.md M19）。
		//
		// 【★空の定義が残っていたら作り直す】`CreateSymbolDefinition` は**名前が既に
		// 使われていれば nil を返す**ので、上の不具合で壊れた（空の）定義を抱えた図面は
		// 消してからでないと直せない。「中身があるか」は**型番号が 0 でないメンバが
		// あるか**で見る——`FirstMemberObj` は空の定義でも非 nil（type 0 のレコード 1 つ）
		// を返すので、非 nil を中身の有無に使ってはいけない。
		constexpr short kSymbolDefinitionNodeType = 16; // kSymDefNode（Objs.TDType.h）
		constexpr short kInternalRecordNodeType = 0; // 空の定義が 1 つだけ持つレコード

		// 定義が絵を持っているか（レコード以外のメンバが 1 つでもあるか）。
		bool DefinitionHasContent(MCObjectHandle definition)
		{
			for (MCObjectHandle h = gSDK->FirstMemberObj(definition); h != nil;
				 h = gSDK->NextObject(h))
				if (gSDK->GetObjectTypeN(h) != kInternalRecordNodeType)
					return true;
			return false;
		}

		// 図面のシンボルライブラリから名前で定義を探す（無ければ nil）。**作らない**——
		// `VWSymbolDefObj` の名前の構築子は見つからなければ作ってしまうので使えない。
		MCObjectHandle FindSymbolDefinition(const TXString& name)
		{
			for (MCObjectHandle h = gSDK->FirstMemberObj(gSDK->GetSymbolLibraryHeader()); h != nil;
				 h = gSDK->NextObject(h))
			{
				if (gSDK->GetObjectTypeN(h) != kSymbolDefinitionNodeType)
					continue; // フォルダ等は飛ばす
				try
				{
					if (VWSymbolDefObj(h).GetObjectName() == name)
						return h;
				}
				catch (...)
				{
					continue; // 名前を読めないものは対象外
				}
			}
			return nil;
		}

		// 筋かいの三角（**直角三角形**）。原点は壁と平行な脚の中央で、直角は
		// risesToEnd なら +X 側（＝終端側）に立つ。頂点は +Y へ伸びる。
		MCObjectHandle MakeBraceTriangle(bool risesToEnd)
		{
			const double half = kShearMarkTriangleLength / 2.0;
			const double foot = risesToEnd ? -half : half;
			const double head = risesToEnd ? half : -half;
			return CreateClosedPolygon({core::Vec2{foot, 0.0}, core::Vec2{head, 0.0},
										core::Vec2{head, kShearMarkTriangleHeight}});
		}

		// 面材の丸印。原点が中心。
		MCObjectHandle MakePanelCircle()
		{
			const double radius = kShearMarkCircleDiameter / 2.0;
			WorldRect bounds;
			bounds.left = -radius;
			bounds.right = radius;
			bounds.bottom = -radius;
			bounds.top = radius;
			return gSDK->CreateOval(bounds);
		}

		// 定義を 1 つ用意する。使える定義が図面にある（か、作れた）なら true。
		bool EnsureMarkSymbol(const char* name, const std::function<MCObjectHandle()>& makeShape)
		{
			const TXString wanted(name);
			if (const MCObjectHandle existing = FindSymbolDefinition(wanted); existing != nil)
			{
				if (DefinitionHasContent(existing))
					return true; // 図面のものを尊重してそのまま使う
				// 空＝上記の不具合で壊れた定義。名前を空けないと作り直せない。
				gSDK->DeleteSymbolDefinition(existing, true, false);
			}

			TXString created(name);
			const MCObjectHandle definition = gSDK->CreateSymbolDefinition(created);
			if (definition == nil)
				return false;
			if (created != wanted)
				return false; // 名前を採番し直された＝別名の定義。PIO は名前で置くので使えない

			const MCObjectHandle shape = makeShape();
			if (shape == nil)
				return false;
			// ★**スクリーン平面の 2D 図形にする。** レイヤ平面のまま入れると定義が 3D
			// 扱い（GetSymbolDefinitionType が k3DSym）になり、伏図に出ない恐れがある。
			gSDK->SetPlanarRefID(shape, kPlanarRefID_ScreenPlane);
			SetClassByName(shape, kShearMarkClass);
			SetAllAttributesByClass(shape);
			if (!gSDK->AddObjectToContainer(shape, definition))
				return false;

			gSDK->ResetObject(definition); // ★これが無いと外接が付かず「空のシンボル」になる
			return true;
		}

		// 3 つまとめて。用意できなかった名前をログに残す（記号が出ない原因になるので、
		// 黙って諦めない）。
		void EnsureMarkSymbols()
		{
			struct Wanted
			{
				const char* name;
				std::function<MCObjectHandle()> makeShape;
			};
			const std::array<Wanted, 3> wanted{
				Wanted{kShearMarkBraceRightSymbol, [] { return MakeBraceTriangle(true); }},
				Wanted{kShearMarkBraceLeftSymbol, [] { return MakeBraceTriangle(false); }},
				Wanted{kShearMarkPanelSymbol, [] { return MakePanelCircle(); }}};

			for (const Wanted& item : wanted)
			{
				const bool ready = EnsureMarkSymbol(item.name, item.makeShape);
				if (core::trace::isOpen())
					core::trace::log(std::string("  shearwall: 記号シンボル ") + item.name + " = " +
									 (ready ? "用意できた" : "**用意できない**"));
			}
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
				// ★**見た目の既定値も毎回書く。** PIO のパラメータ既定値は**図面に記録
				// される**ので、コード側で既定を変えても**その PIO を一度使った図面では
				// 古い値のまま**になる（実機で MarkOffset が 4mm のままになり、記号が
				// 横架材の下に潜って見えなかった。M19）。書き手が値を持つ経路を用意して
				// おけば、既定値の食い違いに悩まされない。
				putReal(kParamShearMarkOffset, kShearMarkOffsetDefault);
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

		// 伏図記号のシンボル定義を用意する（上記 EnsureMarkSymbols）。PIO は名前で置くので、
		// 1 枚目を作る前に揃っていなければならない。
		if (!document.shearWalls.empty())
			EnsureMarkSymbols();

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
