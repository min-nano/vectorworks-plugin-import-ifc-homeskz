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
#include "VWFC/VWObjects/VWSymbolObj.h"
#include "VWFC/VWObjects/VWSymbolDefObj.h"
#include "VWFC/VWObjects/VWPolygon2DObj.h"

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
		// 【一時的な調査 v3】シンボル定義へ入れた図形が絵にならない件（M19）。
		//
		// v2 で分かった事実:
		//   * 定義（GS_CreateSymbolDefinition）は作れて、図面のリソースにも並ぶ。
		//   * AddObjectToContainer は true を返し、**定義の中身は実際に増える**
		//     （自作 = [5 0]。テンプレート由来の既存シンボルも [5 0] で同じ形）。
		//   * それなのに配置したインスタンスの外接が -2147483648（＝大きさ無し）で、
		//     実機のシンボル 2D 編集では何も選べない。
		//
		// 残る枝は 2 つ。**事実だけ**を採って片方に決める。
		//   ① 入れている多角形そのものが空（頂点 0）。
		//   ② 定義の側が「2D の絵を持つシンボル」になっていない（種別・キャッシュ）。
		// そこで v3 では
		//   * 図形の**頂点数・閉じているか・自分の外接・親**を入れる前後で見る（①）、
		//   * 定義の**種別**（GetSymbolDefinitionType = 2D / 3D / ハイブリッド）と外接を、
		//     **既存（正しく描ける）シンボル**と並べて見る（②）、
		//   * 既存シンボルを 1 つ置いて外接を読む（＝こちらの測り方が正しいかの検算）、
		//   * SetPageBased(true) が実際に効くか（実機で縮尺無視にならなかった件）、
		// を一度に採る。切り分けが済んだらこの節ごと消す。
		constexpr short kSymbolDefinitionNodeType = 16; // kSymDefNode（Objs.TDType.h）
		constexpr short kPolygonNodeType = 5;			// kPolygonNode

		// テスト用のシンボル名（実機で消せるよう "_" で始める）。
		constexpr const char* kProbeSymbolA = "_耐力壁記号テストA";

		// 記号の下描き（三角）。調査でも本番と同じ形を使う。
		std::vector<core::Vec2> ProbeTriangle()
		{
			return {core::Vec2{-150.0, 0.0}, core::Vec2{150.0, 0.0}, core::Vec2{150.0, 150.0}};
		}

		// 外接を "幅x高さ" で。取れない／無効値（INT_MIN が入る）はそう書く。
		std::string Extent(MCObjectHandle object)
		{
			WorldRect bounds;
			if (object == nil || !gSDK->GetObjectBounds(object, bounds))
				return "取れない";
			const double width = bounds.right - bounds.left;
			const double height = bounds.top - bounds.bottom;
			if (!std::isfinite(width) || !std::isfinite(height) || std::fabs(width) > 1.0e9)
				return "無効";
			return std::to_string(static_cast<int>(width)) + "x" +
				   std::to_string(static_cast<int>(height));
		}

		// 図形 1 つの素性。多角形なら頂点数と閉じているかも出す（枝①の判定はここ）。
		std::string DescribeShape(MCObjectHandle object)
		{
			if (object == nil)
				return "nil";
			const short type = gSDK->GetObjectTypeN(object);
			std::string text = "type=" + std::to_string(type) + " 外接=" + Extent(object);
			if (type == kPolygonNodeType)
			{
				try
				{
					const VWPolygon2DObj polygon(object);
					text += " 頂点=" + std::to_string(polygon.GetVertexCount()) +
							" 閉=" + (polygon.IsClosed() ? "yes" : "no");
				}
				catch (...)
				{
					text += " 頂点=読めない";
				}
			}
			const MCObjectHandle parent = gSDK->ParentObject(object);
			text += " 親=";
			text += (parent == nil ? std::string("nil")
								   : "type" + std::to_string(gSDK->GetObjectTypeN(parent)));
			return text;
		}

		// 定義 1 つを中身まで書き出す。**種別（defType）が肝**——2D の絵を持つ
		// シンボルになっていなければ、中身があっても伏図には出ない。
		void DumpDefinition(const std::string& label, MCObjectHandle definition)
		{
			if (definition == nil)
			{
				core::trace::log("symprobe: " + label + " = nil");
				return;
			}
			core::trace::log(
				"symprobe: " + label + " type=" + std::to_string(gSDK->GetObjectTypeN(definition)) +
				" defType=" + std::to_string(gSDK->GetSymbolDefinitionType(definition)) +
				" 外接=" + Extent(definition));
			std::size_t index = 0;
			for (MCObjectHandle h = gSDK->FirstMemberObj(definition); h != nil && index < 4;
				 h = gSDK->NextObject(h), ++index)
				core::trace::log("symprobe: " + label + " 中身[" + std::to_string(index) + "] " +
								 DescribeShape(h));
			if (index == 0)
				core::trace::log("symprobe: " + label + " 中身なし");
		}

		// 図面に既にあるシンボル定義（テンプレート由来＝正しく描けるもの）を基準にする。
		// **中身と種別の見え方が自作とどう違うか**が知りたい唯一のこと。
		// 併せて 1 つ実際に置いてみて、外接の読み方そのものを検算する。
		void DumpExistingSymbols()
		{
			const MCObjectHandle header = gSDK->GetSymbolLibraryHeader();
			if (header == nil)
			{
				core::trace::log("symprobe: シンボルライブラリを取れない");
				return;
			}
			std::size_t seen = 0;
			MCObjectHandle first = nil;
			for (MCObjectHandle h = gSDK->FirstMemberObj(header); h != nil && seen < 3;
				 h = gSDK->NextObject(h))
			{
				if (gSDK->GetObjectTypeN(h) != kSymbolDefinitionNodeType)
					continue; // フォルダ等は飛ばす
				if (first == nil)
					first = h;
				++seen;
				DumpDefinition("既存#" + std::to_string(seen), h);
			}
			if (seen == 0)
			{
				core::trace::log("symprobe: 既存のシンボル定義が 1 つも見つからない");
				return;
			}

			// ★検算: 正しく描けるシンボルを置いたとき、外接がまともに読めるか。
			// ここが「無効」なら、自作シンボルの外接が無効なのは測り方の問題であって
			// 定義の中身の話ではない、と分かる。
			try
			{
				const VWSymbolDefObj definition(first);
				const TXString name(definition.GetObjectName());
				const VWSymbolObj instance(name, VWPoint2D(0.0, 0.0), 0.0);
				core::trace::log("symprobe: 既存#1 を置いた name=" + name.GetStdString() + " " +
								 DescribeShape(instance.GetThisObject()));
			}
			catch (...)
			{
				core::trace::log("symprobe: 既存#1 を置けない（名前が取れない）");
			}
		}

		// 自作の定義に図形を入れ、**入れる前・入れた後・更新を促した後**で見比べる。
		void ProbeSymbolCreation()
		{
			// CreateSymbolDefinition は名前を inout で受ける（重複時に採番して返す）ので
			// 一時オブジェクトは渡せない。
			TXString name(kProbeSymbolA);
			const MCObjectHandle definition = gSDK->CreateSymbolDefinition(name);
			core::trace::log("symprobe: A 作成 name=" + name.GetStdString() +
							 " nil=" + (definition == nil ? "yes" : "no"));
			DumpDefinition("A（作っただけ）", definition);

			const MCObjectHandle shape = CreateClosedPolygon(ProbeTriangle());
			// ★枝①の判定。ここで頂点が 3・外接が 300x150 なら図形そのものは正しい。
			core::trace::log("symprobe: A 入れる前の図形 " + DescribeShape(shape));

			const bool added = gSDK->AddObjectToContainer(shape, definition);
			core::trace::log(std::string("symprobe: A AddObjectToContainer=") +
							 (added ? "true" : "false"));
			core::trace::log("symprobe: A 入れた後の図形 " + DescribeShape(shape));
			DumpDefinition("A（入れた後）", definition);

			// ★実機で「縮尺無視」に入らなかった件。setter が効いているのかを見る。
			try
			{
				VWSymbolDefObj wrapper(definition);
				wrapper.SetPageBased(true);
				core::trace::log(std::string("symprobe: A 縮尺無視 GetPageBased=") +
								 (wrapper.GetPageBased() ? "true" : "false"));
			}
			catch (...)
			{
				core::trace::log("symprobe: A 縮尺無視の設定で例外");
			}

			// ★枝②の判定。定義へ「中身が変わった」と知らせて外接・種別が付くか。
			gSDK->ResetObject(definition);
			DumpDefinition("A（ResetObject 後）", definition);

			// ★枝③の判定。**中身を入れた定義が、図面のリソース一覧に居る同名の定義と
			// 同じものか。** CreateSymbolDefinition は「名前が既に使われていれば nil を
			// 返す」仕様（APIBase.Legacy.Defs.h の GS_CreateSymbolDefinition）なので、
			// 名前の取り合いが起きていれば「作った先」と「一覧に見えるもの」が別物に
			// なり得る——実機で中身が空に見えることの説明になる。
			const MCObjectHandle header = gSDK->GetSymbolLibraryHeader();
			MCObjectHandle listed = nil;
			for (MCObjectHandle h = gSDK->FirstMemberObj(header); h != nil && listed == nil;
				 h = gSDK->NextObject(h))
			{
				if (gSDK->GetObjectTypeN(h) != kSymbolDefinitionNodeType)
					continue;
				try
				{
					if (VWSymbolDefObj(h).GetObjectName() == name)
						listed = h;
				}
				catch (...)
				{
				}
			}
			core::trace::log(
				std::string("symprobe: A 一覧に居るか=") +
				(listed == nil ? "no" : (listed == definition ? "同じ handle" : "別の handle")));
			if (listed != nil && listed != definition)
				DumpDefinition("A（一覧側）", listed);
		}

		// 作った定義を素直に置いてみる（PIO の中ではなくアクティブレイヤへ）。
		void ProbeSymbolPlacement()
		{
			const TXString name(kProbeSymbolA);
			const VWSymbolObj instance(name, VWPoint2D(0.0, 0.0), 0.0);
			const MCObjectHandle handle = instance.GetThisObject();
			core::trace::log("symprobe: A を置いた シンボルか=" +
							 std::string(handle != nil && VWSymbolObj::IsSymbolObject(handle, name)
											 ? "yes"
											 : "no") +
							 " " + DescribeShape(handle));
		}

		// 調査ひとまとめ。ログが開いているときだけ（dev ビルドと HOMESKZ_IFC_TRACE）。
		void ProbeSymbolDefinitions()
		{
			if (!core::trace::isOpen())
				return;
			core::trace::log("symprobe: シンボル定義の調査 v3（M19。分かったら消す）");
			DumpExistingSymbols();
			ProbeSymbolCreation();
			ProbeSymbolPlacement();
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

		// 【一時的】シンボル定義の調査（上記 ProbeSymbolDefinitions。分かったら消す）。
		if (!document.shearWalls.empty())
			ProbeSymbolDefinitions();

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
