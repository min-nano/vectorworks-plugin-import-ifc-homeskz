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
//	VWRoofFaceObj。外形ポリゴン＋軸の Z＋upslope 方向＋rise/run を渡し、厚みは SetThickness）。
//	作図状態に依存せず、確定後の後付け操作も要らないので、Python 版が #113 で踏んだ落とし穴
//	（テンプレートのポリゴンを屋根と誤認する／未定義動作でのクラッシュ）が構造的に起きない。
//	**仕様の意図（屋根版 1 面＝単勾配の野地板 1 枚）は同じで、実現手段を SDK の作法へ寄せた**
//	（CLAUDE.md「移植の基本方針」）。
//
//	描画手順:
//	  1. 命令の平面外形を閉じた 2D ポリゴンにする（屋根面の水平投影）。
//	  2. 屋根面オブジェクトを「外形・軸の Z（レイヤ相対）・upslope 方向・勾配（rise/run）」で
//	     生成し、厚み（野地板 12mm）を設定する。upslope 方向は軒（屋根軸）から棟へ向かう
//	     単位方向で、命令の axisStart → upslope から求める。
//	  3. クラス（耐力面材-屋根）を割り当て、描画属性をすべてクラス属性に従わせて再計算する。
//	屋根面を作れない場合は外形ポリゴンにフォールバックする（1 枚の失敗で全体を止めない）。
//
//	【高さの与え方（ローカル確認項目）】屋根面には命令の elevation（軒の絶対 Z）をそのまま渡す。
//	Python 版は BeginRoof 経由では「レイヤ基準で扱わないとレイヤ高さぶん二重に持ち上がる」ことを
//	確認している（#113）が、それは SetZVals ＋ Move3D の手続きに固有の挙動で、屋根面オブジェクトを
//	直接組み立てる本実装では当てはまらない可能性がある。VW 実機で高さがずれていたら、ここで
//	レイヤの高さを引く（＝レイヤ相対へ直す）ことで直せるよう、Z の計算は 1 か所（DrawOne の
//	VWRoofFaceObj 生成）に集約してある。
//
//	実描画（高さ・勾配・厚みの最終挙動、厚みが軸のどちら側へ伸びるか）はローカルの
//	VectorWorks で目視確認する（ROADMAP.md M6「ローカル確認」）。
//

#include "PluginPrefix.h"
#include "draw/Roof.h"
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
		// オブジェクトのクラスを名前で設定する（draw/Grid.cpp・draw/Floor.cpp と同じヘルパー）。
		void SetClassByName(MCObjectHandle object, const std::string& className)
		{
			if (className.empty())
				return;
			const InternalIndex classID = gSDK->AddClass(TXString(className.c_str()));
			gSDK->SetObjectClass(object, classID);
		}

		// 描画属性（線幅・色・パターン・矢印・透明度）をすべてクラス属性に従わせる
		// （draw/Floor.cpp と同じ規約）。
		void SetAllAttributesByClass(MCObjectHandle object)
		{
			gSDK->SetPColorsByClass(object);
			gSDK->SetFColorsByClass(object);
			gSDK->SetLWByClass(object);
			gSDK->SetPPatByClass(object);
			gSDK->SetFPatByClass(object);
			gSDK->SetArrowByClass(object);
			gSDK->SetOpacityByClass(object);
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

		// 野地板 1 枚を屋根面オブジェクトとして描く。作れなければ外形ポリゴンへフォールバック
		// する。何か 1 つでも配置できたら true。
		bool DrawOne(const core::RoofCommand& roof)
		{
			if (roof.run <= 0.0 || roof.boundary.size() < 3)
			{
				// 勾配が定まらない（鉛直面等の退化した）命令は屋根面を作らずフォールバック。
				DrawFallbackPolygon(roof);
				return true;
			}

			// 軒（屋根軸）から棟へ向かう単位方向。命令は軸始点と upslope 定義点で向きを表す。
			const double upX = roof.upslope.x - roof.axisStart.x;
			const double upY = roof.upslope.y - roof.axisStart.y;
			const double upLength = std::hypot(upX, upY);
			if (upLength <= 0.0)
			{
				DrawFallbackPolygon(roof);
				return true;
			}
			const VWPoint2D upSlopeDir(upX / upLength, upY / upLength);

			// 屋根面: 水平投影外形（閉じたポリゴン）＋軒の天端 Z＋upslope 方向＋勾配
			// （rise/run。比がそのまま勾配になるので命令の値をそのまま渡す）。
			const VWPolygon2D outline(BoundaryPoints(roof.boundary), true);
			VWRoofFaceObj face(kRoofFaceType_Roof, outline, roof.elevation, upSlopeDir, roof.rise,
							   roof.run);
			const MCObjectHandle handle = face.GetThisObject();
			if (handle == nil)
			{
				DrawFallbackPolygon(roof);
				return true;
			}

			// 厚み（野地板 12mm 固定）。屋根面は厚みを自分で持つ（スラブのような構成層ではない）。
			face.SetThickness(roof.thickness);

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
			// 配置先レイヤ（"n-野地板"）が無い命令はスキップする（レイヤは story 命令が作る）。
			MCObjectHandle layer = gSDK->GetNamedLayer(TXString(roof.layer.c_str()));
			if (layer == nil)
				continue;
			gSDK->SetCurrentLayer(layer);

			if (DrawOne(roof))
				++drawn;
		}
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
