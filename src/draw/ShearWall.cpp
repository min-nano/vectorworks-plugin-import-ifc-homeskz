//
//	draw/ShearWall.cpp
//
//	耐力壁の設置の実装。意図は draw/ShearWall.h と Extensions/ExtShearWall.h を参照。
//	【SDK 依存】PluginPrefix.h を include するため、この翻訳単位はプラグインビルド
//	（SDK あり）でのみコンパイルされる（CLAUDE.md「依存の向きは厳守する」）。
//
//	手順: 配置先レイヤを用意 → CreateCustomObject で PIO を生成する → **両端を柱芯へ置く**
//	（VWParametricObj::SetLinearObjectPos）→ 本体のクラスを設定 → パラメータを書く →
//	ResetObject。リセットで PIO 本体（Extensions/ExtShearWall）が柱を検索して描画する。
//
//	**パラメータは PIO 本体と同じ名前**でなければ通知なしに無視される（M6 の垂木で実証済み。
//	draw/DrawUtil の ResolveParamName の doc コメント）。名前の定義は 1 か所に集めたいので、
//	Extensions/ExtShearWall.h の kParamShear* を include して共有する。
//

#include "PluginPrefix.h"
#include "draw/ShearWall.h"
#include "draw/DrawUtil.h"
#include "draw/ShearWallPio.h"
#include "Extensions/ExtShearWall.h"
#include "core/Document.h"
#include "core/Progress.h"

#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWSymbolDefObj.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <numbers>
#include <ranges>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// ------------------------------------------------------------------
		// 伏図記号のシンボル定義を用意する（Extensions/ExtShearWall.h の kShearMark*Symbol）。
		//
		// 【なぜプラグインがシンボルを生成するのか】記号をシンボルにしておくと、図面の側で
		// 1 か所（シンボル定義）を編集するだけで**すべての耐力壁の記号を一括で差し替え・
		// 調整**できる（ご要望）。CLAUDE.md「既存の図面リソースを作らない」の唯一の例外で、
		// 生成するのは**耐力壁を 1 枚でも描画するときだけ**・名前が既にあれば**変更しない**。
		//
		// 【★中身を入れたら ResetObject を呼ぶ】ここが M19 で 3 周を要したところ。
		// `CreateSymbolDefinition` で定義を生成し `AddObjectToContainer` で図形を入れる——
		// これは**最初から正しく機能していた**（定義を辿ると多角形が実際にある）。欠けて
		// いたのは**入れた後の `ResetObject(定義)`** で、これが無いと**定義の外接が計算
		// されない**。外接の無い定義は図形として成立せず、実機では
		//   * シンボルの 2D 編集で「すべて選択」しても何も選べない、
		//   * 配置したインスタンスの外接が無効値（大きさが無い）、
		//   * 図には何も表示されない、
		// という「中身はあるのに空に見える」状態になる（SDK 側の挙動は SDK リファレンス
		// の Findings/Symbols.md。方針は docs/DEV-NOTES.md「耐力壁は…」）。
		//
		// 【★空の定義が残っていたら再生成する】`CreateSymbolDefinition` は**名前が既に
		// 使われていれば nil を返す**ので、上の不具合で壊れた（空の）定義を含む図面は
		// 削除してからでないと直せない。「中身があるか」は**型番号が 0 でないメンバが
		// あるか**で判定する——`FirstMemberObj` は空の定義でも非 nil（type 0 のレコード 1 つ）
		// を返すので、非 nil を中身の有無に使ってはいけない。
		// 定義が図形を持っているか（レコード以外のメンバが 1 つでもあるか）。
		bool DefinitionHasContent(MCObjectHandle definition)
		{
			for (MCObjectHandle h = gSDK->FirstMemberObj(definition); h != nil;
				 h = gSDK->NextObject(h))
				if (!IsObjectType(h, ObjectNodeType::InternalRecord))
					return true;
			return false;
		}

		// 図面のシンボルライブラリから名前で定義を探す（無ければ nil）。**生成しない**——
		// `VWSymbolDefObj` の名前のコンストラクタは見つからなければ生成してしまうので使えない。
		MCObjectHandle FindSymbolDefinition(const TXString& name)
		{
			for (MCObjectHandle h = gSDK->FirstMemberObj(gSDK->GetSymbolLibraryHeader()); h != nil;
				 h = gSDK->NextObject(h))
			{
				if (!IsObjectType(h, ObjectNodeType::SymbolDefinition))
					continue; // フォルダ等はスキップする
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

		// 筋かいの三角（**直角三角形**）。原点は壁と平行な脚の中央、直角は +X 側、頂点は
		// +Y 側——「終端側へ上がる・表へ寄せる」形 1 つだけを生成する。残る 3 通りは置くときに
		// 軸ごと反転させる（Extensions/ExtShearWall.h の対応表）。
		MCObjectHandle MakeBraceTriangle()
		{
			const double half = kShearMarkTriangleLength / 2.0;
			return CreateClosedPolygon({core::Vec2{-half, 0.0}, core::Vec2{half, 0.0},
										core::Vec2{half, kShearMarkTriangleHeight}});
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

		// 定義を「使える状態」にする（用紙基準にし、外接を再計算する）。用紙基準にできたら
		// true（できなくても外接は再計算する）。
		//
		// ★**中身がある定義にも ResetObject を呼ぶ。** 外接は中身と別に持たれていて、
		// 中身があっても外接が無い定義は**図に何も表示されない**（実機: 面材の丸は定義に
		// 入っているのに図面へ表示されなかった。壊れた定義を生成した版で図面に残ったものが、
		// 「中身がある」判定でそのまま使われていた）。ResetObject は図形を書き換えないので、
		// 「既にある定義は変更しない」という約束（CLAUDE.md 4）とも矛盾しない。
		bool PrepareDefinition(MCObjectHandle definition)
		{
			// **用紙基準（縮尺無視）にする。** 記号は表記なので、伏図の縮尺が変わっても
			// 紙の上の大きさは変えない（ご要望）。大きさは「定義の図形（用紙 mm）×
			// レイヤの縮尺」で決まるので、耐力壁レイヤの縮尺を伏図の縮尺へ揃える
			// （draw/Sheet の applyPlanLayerScale）ところまでが 1 組。
			bool pageBased = true;
			try
			{
				VWSymbolDefObj(definition).SetPageBased(true);
			}
			catch (...)
			{
				// 用紙基準にできなくても記号自体は表示される（縮尺に追従するだけ）ので止めない。
				// ただし通知せずに破棄しない——「縮尺無視になっていない」の唯一の手掛かりなので、
				// 呼び出し側が記録へ残す（EnsureMarkSymbols）。
				pageBased = false;
			}
			gSDK->ResetObject(definition);
			return pageBased;
		}

		// 定義を 1 つ用意する。使える定義が図面にある（か、生成できた）なら true。用紙基準に
		// できたかを pageBased に返す。
		bool EnsureMarkSymbol(const char* name, const std::function<MCObjectHandle()>& makeShape,
							  bool& pageBased)
		{
			pageBased = false;
			const TXString wanted(name);
			if (const MCObjectHandle existing = FindSymbolDefinition(wanted); existing != nil)
			{
				if (DefinitionHasContent(existing))
				{
					// 図形は図面のものを尊重してそのまま使い、外接と用紙基準だけ整える。
					pageBased = PrepareDefinition(existing);
					return true;
				}
				// 空＝上記の不具合で壊れた定義。名前を空けないと再生成できない。
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
			// 扱い（GetSymbolDefinitionType が k3DSym）になり、伏図に表示されない恐れがある。
			gSDK->SetPlanarRefID(shape, kPlanarRefID_ScreenPlane);
			SetClassWithAttributes(shape, kShearMarkClass);
			if (!gSDK->AddObjectToContainer(shape, definition))
				return false;

			// ★外接が付くのはここ（無いと空のシンボルに見える）
			pageBased = PrepareDefinition(definition);
			return true;
		}

		// まとめて。用意できたか・用紙基準にできたかを 1 行にして outInfo へ返す（記号が
		// 表示されない・縮尺に追従してしまう原因になるので、通知せずに断念しない）。
		// 平常でも出力される記録なので完了ダイアログの診断ではなくログの記録へ
		// （core::DrawCounts）。診断ログへは要素から直接書かない（CLAUDE.md「重複を
		// 作らない置き場所」の診断ログへの書き出し口）。
		void EnsureMarkSymbols(std::string* outInfo)
		{
			struct Wanted
			{
				const char* name;
				std::function<MCObjectHandle()> makeShape;
			};
			const std::array<Wanted, 2> wanted{
				Wanted{kShearMarkBraceSymbol, [] { return MakeBraceTriangle(); }},
				Wanted{kShearMarkPanelSymbol, [] { return MakePanelCircle(); }}};

			std::string text = "耐力壁の記号シンボル:";
			for (const Wanted& item : wanted)
			{
				bool pageBased = false;
				const bool ready = EnsureMarkSymbol(item.name, item.makeShape, pageBased);
				text += std::string(" ") + item.name + " = ";
				if (!ready)
					text += "**用意できない**";
				else if (!pageBased)
					text += "用意できた（**用紙基準にできない**）";
				else
					text += "用意できた";
			}
			AppendLine(outInfo, text);
		}

		// ------------------------------------------------------------------
		// 種別・掛け方・面の値をパラメータの綴りへ変換する（Extensions/ExtShearWall.h の
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

		// 耐力壁 1 枚を置く。PIO を生成して両端とパラメータを書き、リセットまでできたら true。
		// 書けなかったパラメータの数を outUnwritten に加算する。
		//
		// ★**パラメータは 1 つずつ独立に書き、1 つ書けなくても残りとリセットを中断しない。**
		// VWFC の setter は名前が通らないと例外を投げるので、まとめて 1 つの try に入れると
		// **最初の 1 つで残り全部と ResetObject までが実行されない**——PIO は図面に残るのに図形が
		// 1 つも描画されない、という「命令はあるのに表示されない」最悪の形になる（M19 のローカル
		// 確認で実際にこうなった。docs/DEV-NOTES.md M19）。
		bool PlaceOne(const core::ShearWallCommand& wall, std::size_t& outUnwritten,
					  MCObjectHandle& outObject)
		{
			// 挿入点は始端（柱芯）。第 4 引数 bInsert=true でアクティブレイヤへ入れる。
			// 線分 PIO なので、この後 SetLinearObjectPos で両端を与え直す。
			//
			// ★**角度もここで与える**（始端→終端の向き）。線分 PIO として置けていれば
			// 両端がそのまま向きを決めるので角度は要らないが、**万一 1 点のオブジェクトと
			// して置かれても、ローカル +X が壁の向きに揃う**——PIO 側は図形をローカル座標で
			// 描画するので、この 1 つで「向きだけ違う」という直しにくい不具合を防げる。
			const double angle = std::atan2(wall.end.y - wall.start.y, wall.end.x - wall.start.x) *
								 180.0 / std::numbers::pi;
			const MCObjectHandle object =
				gSDK->CreateCustomObject(TXString(kShearWallUniversalName),
										 WorldPt(wall.start.x, wall.start.y), angle, true);
			if (object == nil)
				return false;
			outObject = object;

			// PIO 本体のクラス（筋かい／耐力面材）。PIO が描画する帯・面はこのクラスの属性で
			// 描画される（面材の表裏と伏図の記号だけは PIO 側でクラスを分ける）。
			SetClassByName(object, wall.drawClass);

			bool placed = false;
			std::size_t unwritten = 0;
			try
			{
				VWParametricObj pio(object);

				// 1 つ書くたびに例外を捕捉する。失敗は数えるだけで、次のパラメータへ進む。
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
				// 上端は内法の両端で別々に書く。解析が上の横架材に合わせてあるので、登り梁の
				// 下では左右で違う（parse/ShearWall の fitShearWallsToMembers）。
				putReal(kParamShearTopEnd, wall.topHeightEnd);
				// ★**表示に関わる既定値も毎回書く。** PIO のパラメータ既定値は**図面に記録
				// される**ので、コード側で既定を変えても**その PIO を一度使った図面では
				// 古い値のまま**になる（実機で MarkOffset が 4mm のままになり、記号が
				// 横架材の下に隠れて表示されなかった。M19）。書き込む側が値を持つ経路を用意して
				// おけば、既定値の食い違いが問題にならない。
				putReal(kParamShearMarkOffset, kShearMarkOffsetDefault);
			}
			catch (...)
			{
				// PIO のラッパーそのものを生成できなかった（＝パラメトリックでない）。
				return false;
			}

			outUnwritten += unwritten;

			// **リセットは必ず呼ぶ。** ここが本体の Recalculate を呼び、耐力壁が描画される。
			// 書けなかったパラメータがあっても、描画できるところまでは描画させる。
			gSDK->ResetObject(object);
			return placed;
		}
	} // namespace

	std::size_t drawShearWalls(const core::Document& document, core::ProgressReporter& progress,
							   std::string* outNote, ObjectHandles* outHandles,
							   std::string* outInfo)
	{
		std::size_t drawn = 0;
		std::size_t missingLayers = 0;
		std::size_t failed = 0;
		std::size_t unwritten = 0;

		// 1 枚目を生成する前に PIO の定義（理由は DrawUtil の PrepareCustomObjectDefinition）と
		// 伏図記号のシンボル定義（上記 EnsureMarkSymbols。PIO は名前で置く）を用意する。
		if (!document.shearWalls.empty())
		{
			PrepareCustomObjectDefinition(kShearWallUniversalName);
			EnsureMarkSymbols(outInfo);
		}

		for (std::size_t index = 0; index < document.shearWalls.size(); ++index)
		{
			const core::ShearWallCommand& wall = document.shearWalls[index];
			if (!AdvanceProgress(progress))
				break;

			// "n-耐力壁" はストーリが作るレイヤ。無い＝その階の生成がスキップされたと
			// いうことなので、耐力壁も置かない（要素のために独自にレイヤを作らない）。
			if (ActivateExistingLayer(wall.layer) == nil)
			{
				++missingLayers;
				continue;
			}

			MCObjectHandle object = nil;
			if (PlaceOne(wall, unwritten, object))
				++drawn;
			else
				++failed;
			if (outHandles != nullptr && object != nil)
				outHandles->table().handles[index] = object;
		}

		if (outNote != nullptr && (missingLayers > 0 || failed > 0 || unwritten > 0))
		{
			std::string text = "耐力壁の診断: ";
			AppendCount(text, "配置先レイヤを用意できない命令", missingLayers, "件");
			AppendCount(text, "オブジェクトを作れなかった命令", failed, "件");
			// **書けなかったパラメータは通知せずに破棄しない。** PIO は図面にあるのに
			// 描画結果が欠ける（最悪は何も描画されない）という、いちばん切り分けにくい症状の
			// 唯一の手掛かり。
			AppendCount(text, "PIO に書けなかったパラメータ", unwritten, "個");
			*outNote = std::move(text);
		}

		return drawn;
	}

#if VW_DRAW_VERIFY
	void recheckShearWalls(const ObjectHandles& handles, std::string* outNotes)
	{
		if (handles.table().handles.empty())
			return;

		// 柱から求められなかった壁の経過は先頭の数枚だけ載せる（全数だと読めない）。
		constexpr std::size_t kShownFallbacks = 3;
		std::size_t fromColumns = 0;
		std::size_t fallbacks = 0;
		std::size_t undecided = 0;
		std::string shown;
		for (const auto& [index, object] : handles.table().handles)
		{
			const ShearWallProbe probe = probeShearWall(object);
			switch (probe.kind)
			{
			case ShearWallProbe::Kind::FromColumns:
				++fromColumns;
				continue;
			case ShearWallProbe::Kind::Fallback:
				++fallbacks;
				break;
			case ShearWallProbe::Kind::Undecided:
				++undecided;
				break;
			}
			if (fallbacks + undecided <= kShownFallbacks)
				shown += "\n    #" + std::to_string(index) + ": " + probe.text;
		}

		// 取り込み中のリセットの行（`shearwall: 内法 柱から／控え`）と枚数を照合する。
		// ここで控えが増えていれば「取り込みの後段で柱が見つからなくなった」と読める。
		std::string text = "耐力壁の測り直し（取り込み後・描かない）: 柱から " +
						   std::to_string(fromColumns) + " 枚 / 控え " + std::to_string(fallbacks) +
						   " 枚";
		if (undecided > 0)
			text += " / 内法が決まらない " + std::to_string(undecided) + " 枚";
		text += shown;
		// 行き先は呼び出し側の記録（draw/ExecuteDocument の addNotes）だけ。診断ログへ
		// 直接書くと、結果の「記録:」と同じ行が二度出力される。
		if (outNotes != nullptr)
			*outNotes = text;
	}
#endif
} // namespace HomeskzIfcImport::draw
