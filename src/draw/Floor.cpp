//
//	draw/Floor.cpp
//
//	床板描画の実装。命令セット（FloorCommand）を**スラブオブジェクト**として配置する。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには
//	入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	【Python 版（床ツール）との差異・意図的】Python 版 vw/floor.py は床ツール
//	（Floor オブジェクト）で描くが、本移植では**スラブツール（Slab オブジェクト）**を使う。
//	床ツールは実体としては押し出しの派生でオブジェクト構造がほぼ押し出しと変わらないのに対し、
//	スラブは BIM オブジェクトとして機能（コンポーネント構成・スタイル・データ連携）が
//	強化されており、今後の発展性が高い。ISDK にも Floor の生成 API は無く Slab には
//	一式（CreateSlab / SetSlabHeight / スタイル・コンポーネント）が揃っているため、
//	SDK の作法にも素直に乗る（CLAUDE.md「移植の基本方針」: 仕様の意図を再現し、実現手段は
//	C++ SDK の作法に合わせる）。2D 表現は床ツールと異なる点に注意（ローカル確認項目）。
//
//	描画手順（Python 版 vw/footing.py の draw_slab と同じ作法。底盤＝M9 と共通）:
//	  1. 外形を閉じた 2D ポリゴンにする
//	  2. CreateSlab(プロファイル) でスラブを生成する
//	  3. クラス分けと描画属性の by-class 設定
//	  4. 厚みを 24mm にする（スラブの厚みは**コンポーネント**が決める。文書のスラブ設定に
//	     依存しないよう、スタイルを外して（ConvertToUnstyledSlab）先頭コンポーネントを
//	     24mm に、残りは 0 に潰す）
//	  5. SetSlabHeight にスラブ**天端**の絶対 Z を渡す（＝床下端 + 厚み。SetSlabHeight は
//	     厚みではなく天端高さを設定する関数。Python 版 #70 の不具合と同じ落とし穴）
//	  6. SetObjectStoryBound で天端の高さ基準を「横架材天端」レベルへバインドする
//	     （offset も天端基準なので命令の offset に厚みを足す）
//	  7. ResetObject で反映
//	スラブが作れない場合は外形ポリゴンにフォールバックする（1 枚の失敗で全体を止めない）。
//
//	実描画（天端の与え方・厚み・バインドのアンカー・2D 表現）はローカルの VectorWorks で
//	目視確認する方針（ROADMAP.md M5「ローカル確認」）。
//

#include "PluginPrefix.h"
#include "draw/Floor.h"
#include "core/Document.h"

#include "VWFC/VWObjects/VWPolygon2DObj.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// SetObjectStoryBound に渡すバウンド ID。スラブは高さ基準を 1 つだけ持ち、
		// Python 版 vw/footing.py の draw_slab も 0 を渡している。型は SDK の
		// TObjectBoundID（= Sint32）だが、その別名は SDK の名前空間の中にあるため、
		// ここでは実体の Sint32 で持つ（暗黙変換で同じ）。
		constexpr Sint32 kSlabBoundID = 0;

		// スラブのコンポーネント（層）を走査する上限。ホームズ君の床は 1 層で足りるが、
		// 文書のスラブ設定が多層でも取りこぼさないよう十分大きい値で打ち切る。
		constexpr short kMaxSlabComponents = 32;

		// オブジェクトのクラスを名前で設定する（AddClass は既存なら索引を返し、無ければ作る。
		// draw/Grid.cpp の同名ヘルパーと同じ規約）。
		void SetClassByName(MCObjectHandle object, const std::string& className)
		{
			if (className.empty())
				return;
			const InternalIndex classID = gSDK->AddClass(TXString(className.c_str()));
			gSDK->SetObjectClass(object, classID);
		}

		// 描画属性（線幅・色・パターン・矢印・透明度）をすべてクラス属性に従わせる。
		// SetObjectClass はクラスを割り当てるだけで各属性は by-instance の既定値のまま
		// 残るため、属性ごとに by-class を指定する（Python 版 _set_all_attributes_by_class
		// と同じ意図。ISDK の関数名は VS と異なる: PColors=ペン色 / FColors=面色 /
		// PPat=線種 / FPat=面パターン / Arrow=マーカー）。
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

		// スラブの厚みを thickness にする。スラブの実厚はコンポーネント（層）の合計なので、
		// まずスタイルを外して（文書のスラブ設定＝スタイルに厚みを支配されないように）、
		// 先頭コンポーネントを thickness に、以降のコンポーネントを 0 に潰す。
		// コンポーネントが 1 つも無ければ 1 層を挿入する。
		void SetSlabThickness(MCObjectHandle slab, double thickness)
		{
			// スタイル付きのままではコンポーネント幅を変えられない（スタイルが厚みを持つ）。
			gSDK->ConvertToUnstyledSlab(slab);

			short components = 0;
			for (short index = 1; index <= kMaxSlabComponents; ++index)
			{
				WorldCoord width = 0;
				if (!gSDK->GetComponentWidth(slab, index, width))
					break;
				components = index;
			}

			if (components == 0)
			{
				// コンポーネントが無いスラブ（スタイルを外した直後の既定）には 1 層作る。
				// fill / ペン太さ / 線種は文書の既定に任せ（0 = 既定）、描画属性はクラスに従う。
				gSDK->InsertNewComponentN(slab, 1, thickness, 0, 0, 0, 0, 0);
				return;
			}

			gSDK->SetComponentWidth(slab, 1, thickness);
			for (short index = 2; index <= components; ++index)
				gSDK->SetComponentWidth(slab, index, 0);
		}

		// 床の平面外形を閉じた 2D ポリゴンとして作る（スラブのプロファイル）。
		MCObjectHandle CreateBoundaryPolygon(const std::vector<core::Vec2>& boundary)
		{
			std::vector<VWPoint2D> vertices;
			vertices.reserve(boundary.size());
			for (const core::Vec2& point : boundary)
				vertices.emplace_back(point.x, point.y);

			VWPolygon2DObj polygon(vertices);
			polygon.SetClosed(true); // スラブのプロファイルは閉じた外形
			return polygon.GetThisObject();
		}

		// 床 1 枚をスラブとして描く。スラブを作れなければ外形ポリゴンにフォールバックする。
		// 配置できたら true。
		bool DrawOne(const core::FloorCommand& floor)
		{
			const MCObjectHandle profile = CreateBoundaryPolygon(floor.boundary);
			if (profile == nil)
				return false;

			MCObjectHandle slab = gSDK->CreateSlab(profile);
			if (slab == nil)
			{
				// フォールバック: 外形ポリゴンをクラス付きで残す。
				SetClassByName(profile, floor.drawClass);
				SetAllAttributesByClass(profile);
				return true;
			}

			SetClassByName(slab, floor.drawClass);
			SetAllAttributesByClass(slab);
			SetSlabThickness(slab, floor.thickness);

			// SetSlabHeight は厚みではなく**天端高さ**（絶対 Z）を設定する。命令の
			// elevation は床下端なので厚みを足す（Python 版 #70 と同じ落とし穴）。
			const double topElevation = floor.elevation + floor.thickness;
			gSDK->SetSlabHeight(slab, topElevation);

			// 高さ基準（天端）を標準の床高＝「横架材天端」レベルへバインドする。命令の
			// bound.offset は床下端と横架材天端の差分なので、天端基準へ厚みぶん寄せる。
			// これをしないと編集時に高さがレイヤ基準へリセットされて実形状と矛盾する。
			VectorWorks::SStoryObjectData bound;
			bound.fBound = VectorWorks::eStoryObjectBound_Story;
			bound.fBoundStory = static_cast<Sint8>(floor.bound.storyOffset);
			bound.fLayerLevelType = TXString(floor.bound.level.c_str());
			bound.fOffset = floor.bound.offset + floor.thickness;
			gSDK->SetObjectStoryBound(slab, kSlabBoundID, bound);

			gSDK->ResetObject(slab);
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
