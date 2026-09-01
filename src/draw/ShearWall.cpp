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
#include "VWFC/VWObjects/VWGroupObj.h"

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
		// 【一時的な調査】シンボル定義へ図形を入れられない件を実機で切り分ける（M19）。
		//
		// 分かっていること: 定義（GS_CreateSymbolDefinition）は作れて図面のリソースにも
		// 並ぶのに、AddObjectToContainer で図形を入れても**実機のシンボル 2D 編集では
		// 何も選べない**。一方 GetFirstMemberObject() は非 nil を返すので、
		// 「入った／入っていない」がこちらから判断できていない。
		//
		// そこで**事実だけを採る**: 既存の（テンプレート由来の）シンボル定義と、
		// こちらが作った定義を同じ API で覗いて並べ、入れ方を 3 通り試して結果を比べる。
		// 切り分けが済んだらこの節ごと消す。
		constexpr short kSymbolDefinitionNodeType = 16; // kSymDefNode（Objs.TDType.h）

		// テスト用のシンボル名（実機で消せるよう "_" で始める）。
		constexpr const char* kProbeSymbolA = "_耐力壁記号テストA";
		constexpr const char* kProbeSymbolB = "_耐力壁記号テストB";
		constexpr const char* kProbeSymbolC = "_耐力壁記号テストC";

		// 記号の下描き（三角）。調査でも本番と同じ形を使う。
		std::vector<core::Vec2> ProbeTriangle()
		{
			return {core::Vec2{-150.0, 0.0}, core::Vec2{150.0, 0.0}, core::Vec2{150.0, 150.0}};
		}

		// コンテナの中身を「件数と型番号」で書き出す。型番号は Objs.TDType.h
		// （4=楕円 / 5=多角形 / 11=グループ / 16=シンボル定義）。
		std::string DescribeMembers(MCObjectHandle container)
		{
			std::size_t count = 0;
			std::string types;
			for (MCObjectHandle h = gSDK->FirstMemberObj(container); h != nil && count < 8;
				 h = gSDK->NextObject(h))
			{
				++count;
				types += " " + std::to_string(gSDK->GetObjectTypeN(h));
			}
			return std::to_string(count) + " 件 [" + types + " ]";
		}

		void DumpSymbolDefinition(const std::string& label, MCObjectHandle definition)
		{
			if (definition == nil)
			{
				core::trace::log("symprobe: " + label + " = nil");
				return;
			}
			core::trace::log(
				"symprobe: " + label + " type=" + std::to_string(gSDK->GetObjectTypeN(definition)) +
				" defType=" + std::to_string(gSDK->GetSymbolDefinitionType(definition)) +
				" 中身=" + DescribeMembers(definition));
		}

		// 図面に既にあるシンボル定義（テンプレート由来のもの）を数件ダンプする。
		// **これが「正しく中身が入っているシンボル」の見え方**で、比較の基準になる。
		void DumpExistingSymbols()
		{
			const MCObjectHandle header = gSDK->GetSymbolLibraryHeader();
			if (header == nil)
			{
				core::trace::log("symprobe: シンボルライブラリを取れない");
				return;
			}
			std::size_t seen = 0;
			for (MCObjectHandle h = gSDK->FirstMemberObj(header); h != nil && seen < 6;
				 h = gSDK->NextObject(h))
			{
				if (gSDK->GetObjectTypeN(h) != kSymbolDefinitionNodeType)
					continue; // フォルダ等は飛ばす
				++seen;
				DumpSymbolDefinition("既存#" + std::to_string(seen), h);
			}
			if (seen == 0)
				core::trace::log("symprobe: 既存のシンボル定義が 1 つも見つからない");
		}

		// 入れ方を 3 通り試す。どれで中身が増えるかを見る。
		void ProbeSymbolCreation()
		{
			// 方法 A: 図形を作ってから AddObjectToContainer（いままでのやり方）
			{
				TXString name(kProbeSymbolA);
				const MCObjectHandle definition = gSDK->CreateSymbolDefinition(name);
				core::trace::log(std::string("symprobe: A 作成 name=") + name.GetStdString() +
								 " nil=" + (definition == nil ? "yes" : "no"));
				const MCObjectHandle shape = CreateClosedPolygon(ProbeTriangle());
				core::trace::log(
					"symprobe: A 図形 nil=" + std::string(shape == nil ? "yes" : "no") +
					" type=" + (shape == nil ? "-" : std::to_string(gSDK->GetObjectTypeN(shape))));
				const bool added = gSDK->AddObjectToContainer(shape, definition);
				core::trace::log(std::string("symprobe: A AddObjectToContainer=") +
								 (added ? "true" : "false"));
				DumpSymbolDefinition("A", definition);
			}

			// 方法 B: グループにまとめてからグループを入れる
			{
				TXString name(kProbeSymbolB);
				const MCObjectHandle definition = gSDK->CreateSymbolDefinition(name);
				VWGroupObj group;
				group.AddObject(CreateClosedPolygon(ProbeTriangle()));
				const MCObjectHandle groupHandle = group.GetThisObject();
				core::trace::log(
					"symprobe: B グループ 中身=" +
					(groupHandle == nil ? std::string("nil") : DescribeMembers(groupHandle)));
				const bool added = gSDK->AddObjectToContainer(groupHandle, definition);
				core::trace::log(std::string("symprobe: B AddObjectToContainer=") +
								 (added ? "true" : "false"));
				DumpSymbolDefinition("B", definition);
			}

			// 方法 C: 定義を「アクティブなシンボル定義」にしてから図形を作る
			// （作った先が定義になるか＝VectorScript の BeginSym/EndSym に当たる挙動か）
			{
				TXString name(kProbeSymbolC);
				const MCObjectHandle definition = gSDK->CreateSymbolDefinition(name);
				gSDK->SetActiveSymbolDef(definition);
				const MCObjectHandle shape = CreateClosedPolygon(ProbeTriangle());
				DumpSymbolDefinition("C（作っただけ）", definition);
				const bool added = gSDK->AddObjectToContainer(shape, definition);
				core::trace::log(std::string("symprobe: C AddObjectToContainer=") +
								 (added ? "true" : "false"));
				DumpSymbolDefinition("C（入れた後）", definition);
			}
		}

		// 調査ひとまとめ。ログが開いているときだけ（dev ビルドと HOMESKZ_IFC_TRACE）。
		void ProbeSymbolDefinitions()
		{
			if (!core::trace::isOpen())
				return;
			core::trace::log("symprobe: シンボル定義の調査（M19。分かったら消す）");
			DumpExistingSymbols();
			ProbeSymbolCreation();
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
