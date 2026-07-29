//
//	draw/Floor.cpp
//
//	床板描画の実装。Python 版 vw/floor.py に対応する。命令セット（FloorCommand）を
//	床ツール（Floor オブジェクト）として配置する。【SDK 依存】PluginPrefix.h
//	（VectorWorks SDK）を include するため、この翻訳単位はプラグインビルド（SDK あり）
//	でのみコンパイルされ、無 SDK の core/parse ライブラリには入れない
//	（CLAUDE.md「依存の向きは厳守する」）。
//
//	床ツールの作り方（Python 版 vw/floor.py と同じ手順。BeginFloor の VW 公式
//	ドキュメント「2D オブジェクト作成手続きでテンプレートを定義し、EndGroup で完了する」）:
//	  1. BeginFloor(厚み)                 … 床の作成を開始する
//	  2. ClosePoly → BeginPoly → MoveTo/LineTo → EndPoly
//	                                       … 平面外形を閉じたポリゴンとして描く
//	  3. EndGroup                         … 床オブジェクトを確定する
//	  4. LNewObj                          … いま作られた床のハンドルを取る
//	  5. Move3D(0,0,elevation)            … 床下端を IFC の床位置（絶対 Z）へ移動する
//	                                       （床ツールは作成した層平面 Z=0 に床を置くため。
//	                                        構造材の Move3D と同じ規約）
//	  6. SetObjectClass / 属性を by-class  … クラス分けと描画属性のクラス従属
//	  7. SetObjectStoryBound(h,0,2,…)     … 高さ基準を「横架材天端」レベルへバインドする
//	                                       （bound.offset ＝基準高さからの高低差。段差＝
//	                                        スキップフロアはここに表れる）
//	  8. ResetObject                      … 反映
//	床が作れない（LNewObj が nil）場合は外形ポリゴンにフォールバックする（1 枚の失敗で
//	全体を止めない。Python 版の寛容さ）。
//
//	実描画（Move3D の絶対 Z・厚みの伸びる向き・SetObjectStoryBound のアンカー）は
//	ローカルの VectorWorks で目視確認する方針（ROADMAP.md M5「ローカル確認」）。
//	床下端＝IFC の床位置になること、段差床が offset ぶんずれることを実機で確かめる。
//

#include "PluginPrefix.h"
#include "draw/Floor.h"
#include "core/Document.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// SetObjectStoryBound の引数（Python 版 vw/floor.py の (floor, 0, 2, …) と同値）。
		// 第 2 引数 0 = 下端（床は下端だけをバインドする）、第 3 引数 2 = ストーリレベル基準。
		constexpr short kBoundBottom = 0;
		constexpr short kBoundTypeStory = 2;

		// オブジェクトのクラスを名前で設定する（AddClass は既存なら索引を返し、無ければ作る。
		// draw/Grid.cpp の同名ヘルパーと同じ規約）。
		void SetClassByName(MCObjectHandle object, const std::string& className)
		{
			if (className.empty())
				return;
			const InternalIndex classID = gSDK->AddClass(TXString(className.c_str()));
			gSDK->SetObjectClass(object, classID);
		}

		// 描画属性（太さ・色・パターン・透明度等）をすべてクラス属性に従わせる
		// （Python 版 _set_all_attributes_by_class）。SetObjectClass はクラスを割り当てる
		// だけで各属性は by-instance の既定値のまま残るため、属性ごとに by-class を指定する。
		void SetAllAttributesByClass(MCObjectHandle object)
		{
			gSDK->SetPenColorByClass(object);
			gSDK->SetFillColorByClass(object);
			gSDK->SetLWByClass(object);
			gSDK->SetLSByClass(object);
			gSDK->SetFPatByClass(object);
			gSDK->SetMarkerByClass(object);
			gSDK->SetOpacityByClass(object);
		}

		// 床の平面外形を「閉じたポリゴン」として描く（Python 版の ClosePoly → BeginPoly →
		// MoveTo/LineTo → EndPoly）。boundary は 3 点以上（validateDocument が保証）。
		void DrawBoundaryPolygon(const std::vector<core::Vec2>& boundary)
		{
			gSDK->ClosePoly(); // 以降の BeginPoly を「閉じた」ポリゴンにする
			gSDK->BeginPoly();
			gSDK->MoveTo(boundary.front().x, boundary.front().y);
			for (std::size_t i = 1; i < boundary.size(); ++i)
				gSDK->LineTo(boundary[i].x, boundary[i].y);
			gSDK->EndPoly();
		}

		// 床 1 枚を描く（Python 版 draw_floor）。床ツールで作れなかったときは外形
		// ポリゴンにフォールバックする。配置できたら true。
		bool DrawOne(const core::FloorCommand& floor)
		{
			gSDK->BeginFloor(floor.thickness);
			DrawBoundaryPolygon(floor.boundary);
			gSDK->EndGroup();

			MCObjectHandle object = gSDK->LNewObj();
			if (object == nil)
			{
				// フォールバック: 外形ポリゴンだけを残してクラス分けする。
				DrawBoundaryPolygon(floor.boundary);
				MCObjectHandle polygon = gSDK->LNewObj();
				if (polygon == nil)
					return false;
				SetClassByName(polygon, floor.drawClass);
				SetAllAttributesByClass(polygon);
				return true;
			}

			// 床下端を IFC の床位置（絶対 Z）へ。床ツールは床を作成した層平面（Z=0）に
			// 置くため、Move3D で実際の高さへ移動する（構造材の Move3D と同じ規約）。
			gSDK->Move3D(0.0, 0.0, floor.elevation);
			SetClassByName(object, floor.drawClass);
			SetAllAttributesByClass(object);

			// 高さ基準を標準の床高＝「横架材天端」レベルへバインドする。offset は床下端と
			// 横架材天端の差分（段差＝スキップフロアはここに表れる）。これをしないと編集時に
			// 高さがレイヤ基準へリセットされて Move3D で与えた実高さと矛盾する。
			TXString levelType(floor.bound.level.c_str());
			gSDK->SetObjectStoryBound(object, kBoundBottom, kBoundTypeStory,
									  static_cast<short>(floor.bound.storyOffset), levelType,
									  floor.bound.offset);
			gSDK->ResetObject(object);
			return true;
		}
	} // namespace

	std::size_t drawFloors(const core::Document& document)
	{
		std::size_t drawn = 0;
		for (const core::FloorCommand& floor : document.floors)
		{
			// 配置先レイヤ（"n-FL"）が無い命令はスキップする。レイヤは story 命令が作る
			// ので、無い＝そのストーリの生成がスキップされたということ。床のために勝手に
			// レイヤを作らない（Python 版 execute_floors と同じ規約）。
			MCObjectHandle layer = gSDK->GetNamedLayer(TXString(floor.layer.c_str()));
			if (layer == nil)
				continue;
			gSDK->SetCurrentLayer(layer);

			if (DrawOne(floor))
				++drawn;
		}
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
