//
//	draw/Roof.cpp
//
//	野地板描画の実装。命令セット（RoofCommand）を**屋根面オブジェクト（Roof Face）**として
//	配置する。Python 版 vw/roof.py に対応する。【SDK 依存】PluginPrefix.h（VectorWorks SDK）を
//	include するため、この翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の
//	core/parse ライブラリには入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	【Python 版（BeginRoof）との差異・意図的】Python 版 vw/roof.py は VectorScript の
//	`BeginRoof` … `EndGroup` で屋根面を作る（作図状態に依存する手続きで、レイヤ Z/ΔZ の
//	退避・復元、外形ポリゴンをテンプレートに使う、確定後の後付け操作でクラッシュしないよう
//	細心の順序を守る…といった作法が必要だった。#113）。ISDK にその一連の呼び出しは無く、
//	代わりに**屋根面オブジェクトを外形・高さ・勾配から直接組み立てられる**（VWFC の
//	VWRoofFaceObj。外形ポリゴンから生成し、屋根軸・棟側・勾配・軸の Z はオブジェクト変数で
//	与え、厚みは SetThickness）。
//	作図状態に依存せず、確定後の後付け操作も要らないので、Python 版が #113 で踏んだ落とし穴
//	（テンプレートのポリゴンを屋根と誤認する／未定義動作でのクラッシュ）が構造的に起きない。
//	**仕様の意図（屋根版 1 面＝単勾配の野地板 1 枚）は同じで、実現手段を SDK の作法へ寄せた**
//	（CLAUDE.md「移植の基本方針」）。
//
//	描画手順:
//	  1. 命令の平面外形を閉じた 2D ポリゴンにする（屋根面の水平投影）。
//	  2. 屋根面オブジェクトを外形から生成する。
//	  3. **屋根面を決めるオブジェクト変数を実測値で上書きする**（下記「屋根軸は
//	     オブジェクト変数で与える」）。屋根軸（ovSlabRoofPt1/Pt2）・棟側の点
//	     （ovSlabRoofUpslopePt）・勾配（ovSlabRoofRise/Run）・軸の Z（ovSlabHeight）。
//	  4. **図面（レイヤ）へ挿入する**（下記「生成しただけでは図面に入らない」）。
//	  5. ハンドルから屋根面オブジェクトを作り直して形状を再構築し（VWRoofFaceObj の
//	     ハンドル版コンストラクタが InitGeometry を呼ぶ）、厚み（野地板 12mm）を設定する。
//	  6. クラス（耐力面材-屋根）を割り当て、描画属性をすべてクラス属性に従わせて再計算する。
//	屋根面を作れない場合は外形ポリゴンにフォールバックする（1 枚の失敗で全体を止めない）。
//
//	【生成しただけでは図面に入らない】VWRoofFaceObj は gSDK->CreateBasicSlab でオブジェクトを
//	作るだけで、どのコンテナにも入れない（外形ポリゴンを自分の中へ入れるのみ）。VWPolygon2DObj
//	のように図面へ入る wrapper とは違うので、**明示的にレイヤへ AddObjectToContainer しないと
//	オブジェクトはできているのに図面に現れない**（ローカル確認で、完了ダイアログは枚数を数えて
//	いるのに 1 枚も見えないという形で判明した）。二重登録を避けるため親が無いときだけ入れる。
//
//	【屋根軸はオブジェクト変数で与える（重要）】VWFC の
//	`VWRoofFaceObj(type, poly, z, upSlopeDir, rise, run)` は一見これだけで屋根面を組めそうだが、
//	SDK のソース（SDKLib/Source/VWSDK/VWFC/VWObjects/VWRoofFaceObj.cpp）を読むと
//
//	  * 引数 z を**一度も使っていない**（軸の Z は 0 のまま）
//	  * 屋根軸を `±upSlopeDir.Perp() * dim`（dim＝外形バウンズの短辺の半分）＝**原点を通る線**
//	    として設定する。外形の位置を見ておらず、外形が原点から離れているほど軸は外形の外に出る
//
//	という実装で、任意の位置・高さの片流れ面を作る用途には使えない。屋根面の形状を実際に
//	決めているのは InitGeometry が読む
//	オブジェクト変数（ovSlabRoofPt1/Pt2/UpslopePt/Rise/Run・ovSlabHeight・ovSlabThickness）
//	なので、生成後にそれらを命令の値で上書きし、ハンドル版コンストラクタで InitGeometry を
//	もう一度走らせて形状を作り直す。**ovSlabRoofUpslopePt は「方向」ではなく「棟側にある点」**
//	（ObjectVariables.h のコメント "a point on the upslope side of the roof"）なので、命令の
//	upslope（軸から棟側へ進んだ点）をそのまま渡す。
//
//	【高さは絶対 Z で与える】屋根軸の Z（ovSlabHeight）には命令の elevation（軒の絶対 Z）を
//	そのまま渡す。垂木の配置で「絶対 Z を渡すのが正しい」ことが確認できた
//	（draw/Rafter.cpp「高さは配置行列の絶対 Z で与える」）ので、同じ規約に揃えてある。
//	Z の計算は 1 か所（DrawOne の ovSlabHeight 設定）に集約してある。
//

#include "PluginPrefix.h"
#include "draw/Roof.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"

// 屋根面オブジェクト（VWRoofFaceObj）と、その外形に渡す 2D ポリゴン。フォールバックの
// 外形ポリゴンは draw/Floor.cpp と同じ VWPolygon2DObj で描く。
#include "VWFC/Math/VWPolygon.h"
#include "VWFC/VWObjects/VWRoofFaceObj.h"
#include "VWFC/VWObjects/VWPolygon2DObj.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 勾配（rise/run）を渡すときの run の基準値（mm）。命令の rise/run は屋根面の**単位
		// 法線成分**（rise=水平成分 dh・run=鉛直成分 nz）で、比はそのまま勾配だが、値そのものは
		// 1 mm 未満の極小値になる。屋根面は rise/run を**長さ（WorldCoord）**として保持するため、
		// 極小値のまま渡すと勾配計算が丸めに埋もれるおそれがある。Python 版が VW 実機で通して
		// いた VectorScript エクスポートの規約に合わせ、比を保ったまま run＝25.4（1 インチ）へ
		// 正規化して渡す（例: 10 度 → rise≈4.479 / run=25.4）。
		constexpr double kSlopeRunUnit = 25.4;

		// オブジェクト変数へ 2D 点／実数を書き込む小さなヘルパー（呼び出しの定型を 1 か所に）。
		void SetPointVariable(MCObjectHandle object, short variable, const core::Vec2& point)
		{
			gSDK->SetObjectVariable(object, variable, TVariableBlock(WorldPt(point.x, point.y)));
		}

		void SetRealVariable(MCObjectHandle object, short variable, double value)
		{
			gSDK->SetObjectVariable(object, variable, TVariableBlock(value));
		}

		// 命令の平面外形を VWPoint2D の列にする（屋根面の外形・フォールバックの外形に共通）。
		std::vector<VWPoint2D> BoundaryPoints(const std::vector<core::Vec2>& boundary)
		{
			std::vector<VWPoint2D> points;
			points.reserve(boundary.size());
			for (const core::Vec2& point : boundary)
				points.emplace_back(point.x, point.y);
			return points;
		}

		// 外形ポリゴンをクラス付きで残す（屋根面を作れなかったときのフォールバック）。
		void DrawFallbackPolygon(const core::RoofCommand& roof)
		{
			VWPolygon2DObj polygon(BoundaryPoints(roof.boundary));
			polygon.SetClosed(true);
			const MCObjectHandle handle = polygon.GetThisObject();
			if (handle == nil)
				return;
			SetClassByName(handle, roof.drawClass);
			SetAllAttributesByClass(handle);
		}

		// 野地板 1 枚を屋根面オブジェクトとして描く。**屋根面として作れたときだけ true** を返し、
		// 外形ポリゴンへフォールバックした場合は false（完了ダイアログの「描けた数」が
		// 「6/6」ではなく「0/6」になるので、ローカル確認で屋根面生成の失敗が一目で分かる）。
		bool DrawOne(const core::RoofCommand& roof, MCObjectHandle layer)
		{
			if (roof.run <= 0.0 || roof.boundary.size() < 3)
			{
				// 勾配が定まらない（鉛直面等の退化した）命令は屋根面を作らずフォールバック。
				DrawFallbackPolygon(roof);
				return false;
			}

			// 軒（屋根軸）が退化していないこと。軸の向きが無いと屋根面が定まらない。
			if (std::hypot(roof.axisEnd.x - roof.axisStart.x, roof.axisEnd.y - roof.axisStart.y) <=
					0.0 ||
				std::hypot(roof.upslope.x - roof.axisStart.x, roof.upslope.y - roof.axisStart.y) <=
					0.0)
			{
				DrawFallbackPolygon(roof);
				return false;
			}

			// 水平投影外形（閉じたポリゴン）から屋根面オブジェクトを作る。位置・高さ・勾配は
			// このコンストラクタでは与えられない（冒頭「屋根軸はオブジェクト変数で与える」）ので、
			// ここでは外形だけを渡す。
			const VWPolygon2D outline(BoundaryPoints(roof.boundary), true);
			const VWRoofFaceObj face(kRoofFaceType_Roof, outline);
			const MCObjectHandle handle = face.GetThisObject();
			// 屋根面として成立していなければ外形ポリゴンへフォールバックする（nil だけでなく
			// 種別も確かめる。屋根面でないものに屋根専用の設定を続けない）。
			if (handle == nil || !VWRoofFaceObj::IsRoofFaceObjectN(handle))
			{
				DrawFallbackPolygon(roof);
				return false;
			}

			// **図面（レイヤ）へ挿入する。** VWFC の VWRoofFaceObj は gSDK->CreateBasicSlab で
			// オブジェクトを作るだけで、どのコンテナにも入れない（外形ポリゴンを自分の中へ
			// AddObjectToContainer するのみ）。VWPolygon2DObj のように図面へ入る wrapper と
			// 違うため、入れないと**オブジェクトはできているのに図面に現れない**。すでに
			// どこかに入っている場合に二重登録しないよう、親が無いときだけ入れる。
			if (gSDK->ParentObject(handle) == nil)
				gSDK->AddObjectToContainer(handle, layer);

			// 屋根面の形状を決めるオブジェクト変数を命令の値で上書きする。
			//   * 屋根軸（軒に沿う線）… 命令の軸始点・終点。
			//   * 棟側の点 … 命令の upslope（**方向ではなく点**）。
			//   * 勾配 … 比を保ったまま run＝25.4 基準へ正規化する（kSlopeRunUnit 参照）。
			//   * 軸の Z … 命令の elevation（冒頭「高さは絶対 Z で与える」）。
			SetPointVariable(handle, ovSlabRoofPt1, roof.axisStart);
			SetPointVariable(handle, ovSlabRoofPt2, roof.axisEnd);
			SetPointVariable(handle, ovSlabRoofUpslopePt, roof.upslope);
			SetRealVariable(handle, ovSlabRoofRise, roof.rise * kSlopeRunUnit / roof.run);
			SetRealVariable(handle, ovSlabRoofRun, kSlopeRunUnit);
			SetRealVariable(handle, ovSlabHeight, roof.elevation);

			// 上書きした変数から形状を作り直す。ハンドル版コンストラクタが InitGeometry を
			// 呼ぶので、これが「変数 → 屋根面の 3D 形状」の再構築にあたる。
			VWRoofFaceObj placed(handle);
			// 厚み（野地板 12mm 固定）。屋根面は厚みを自分で持つ（スラブのような構成層ではない）。
			placed.SetThickness(roof.thickness);

			SetClassByName(handle, roof.drawClass);
			SetAllAttributesByClass(handle);
			gSDK->ResetObject(handle);
			return true;
		}
	} // namespace

	std::size_t drawRoofs(const core::Document& document)
	{
		std::size_t drawn = 0;
		for (const core::RoofCommand& roof : document.roofs)
		{
			// 配置先レイヤ（"n-野地板"）が無い命令はスキップする（規約は ActivateExistingLayer）。
			const MCObjectHandle layer = ActivateExistingLayer(roof.layer);
			if (layer == nil)
				continue;

			if (DrawOne(roof, layer))
				++drawn;
		}
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
