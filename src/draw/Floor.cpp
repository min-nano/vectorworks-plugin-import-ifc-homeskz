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
//	  4. **階ごとのスラブスタイル**（"1F-床スタイル" / "屋根-床スタイル" 等）を用意して
//	     適用する。スタイルの構成層は命令どおりに作り直す（上から 床仕上げ → 床下地。
//	     命令の層を先頭に挿入し、元の層は**削除**する。厚み 0 の層は作れないため）。
//	     スタイルを用意できないときだけ、スラブ本体のコンポーネントを直接組む
//	  5. 高さの基準面（データム）を命令に合わせる（一般階＝床仕上げ上端／ロフト＝床下地
//	     下端）。SetSlabHeight にはその基準面の絶対 Z を渡す（SetSlabHeight は厚みではなく
//	     高さを設定する関数。Python 版 #70 の不具合と同じ落とし穴）
//	  6. SetObjectStoryBound で基準面の高さ基準をストーリレベルへバインドする
//	     （一般階＝FL レベル／ロフト＝軒高レベル。offset はそこからの高低差で一般部は 0）
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

		// 既存のコンポーネント（層）数。取得できなければ 0（＝層を持たない）とみなす。
		short CountComponents(MCObjectHandle object)
		{
			short count = 0;
			if (!gSDK->GetNumberOfComponents(object, count))
				return 0;
			return count;
		}

		// オブジェクト（スラブ本体／スラブスタイル）の構成層を命令どおり（上から
		// 床仕上げ → 床下地）に作り直す。
		//
		// 手順: **先頭に命令の層を順に挿入し、その後ろに残った元の層を削除する**。
		//   * 厚み 0 の層は VW が受け付けない（＝「潰す」では構成を確定できない）ので、
		//     余った層は必ず削除する。
		//   * 先に挿入してから削除するので、途中で層が 0 枚になる瞬間が無い
		//     （層が 1 枚も無いスラブ／スタイルは作れない）。
		//   * 削除に失敗したら（想定外の API 挙動）そこで打ち切り、元の層が残ったままでも
		//     床自体は残す（1 枚の失敗で全体を止めない）。
		void SetComponents(MCObjectHandle object,
						   const std::vector<core::SlabComponentCommand>& components)
		{
			const short original = CountComponents(object);
			const auto wanted = static_cast<short>(components.size());

			// 1. 命令の層を先頭から順に挿入する（index の層の「手前」に入るので、
			//    1,2,… の順に入れると命令どおりの並びが先頭にできる）。fill / ペン太さ /
			//    線種は文書の既定に任せ（0）、描画属性はクラスに従わせる。
			for (short index = 1; index <= wanted; ++index)
			{
				const core::SlabComponentCommand& component =
					components[static_cast<std::size_t>(index - 1)];
				gSDK->InsertNewComponentN(object, index, component.thickness, 0, 0, 0, 0, 0);
				gSDK->SetComponentWidth(object, index, component.thickness);
				gSDK->SetComponentName(object, index, TXString(component.name.c_str()));
			}

			// 2. 挿入した層の直後に並んでいる元の層を、前から順に削除する。
			for (short removed = 0; removed < original; ++removed)
			{
				if (!gSDK->DeleteComponent(object, static_cast<short>(wanted + 1)))
					break;
			}
		}

		// 階ごとのスラブスタイル（"1F-床スタイル" 等）を用意して索引を返す。既にあれば
		// それを使い、無ければ作る。構成層は毎回命令どおりに更新する（再インポートで階の
		// 構成が変わっても追従する）。用意できなければ 0（＝スタイル無し）を返す。
		InternalIndex ResolveSlabStyle(const std::string& styleName,
									   const std::vector<core::SlabComponentCommand>& components)
		{
			if (styleName.empty())
				return 0;

			const TXString name(styleName.c_str());
			MCObjectHandle style = gSDK->GetNamedObject(name);
			if (style == nil)
				style = gSDK->CreateSlabStyle(name);
			if (style == nil)
				return 0;

			SetComponents(style, components);
			return gSDK->GetObjectInternalIndex(style);
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

			// 構成は階ごとのスラブスタイルで与える（階により構成が異なることが多いため、
			// スタイルは階ごとに 1 つ）。スタイルを用意できない場合だけ、スタイルを外して
			// スラブ本体のコンポーネントを直接組む（構成の欠落で床を失わないための保険）。
			const InternalIndex style = ResolveSlabStyle(floor.styleName, floor.components);
			if (style != 0)
			{
				gSDK->SetSlabStyle(slab, style);
			}
			else
			{
				gSDK->ConvertToUnstyledSlab(slab);
				SetComponents(slab, floor.components);
			}

			// 高さの基準面（データム）を命令に合わせる。スラブの高さはこの面を指す。
			//   Top    … 床仕上げ＝最上層（コンポーネント 1）を基準にする＝スラブ天端
			//   Bottom … 床下地＝最下層（コンポーネント N）を基準にする＝スラブ底面
			// SetDatumSlabComponent はスラブの高さがどのコンポーネントを指すかを決める。
			// 実際にどの面（層の上端／下端）を指すかは VW 上で目視確認する（ROADMAP.md M5）。
			const auto componentCount = static_cast<short>(floor.components.size());
			if (componentCount > 0)
			{
				const short datumComponent =
					(floor.datum == core::SlabDatum::Bottom) ? componentCount : 1;
				gSDK->SetDatumSlabComponent(slab, datumComponent);
			}

			// SetSlabHeight は厚みではなく**基準面の高さ**（絶対 Z）を設定する（Python 版 #70 と
			// 同じ落とし穴）。命令の elevation は基準面の絶対 Z なのでそのまま渡す。
			gSDK->SetSlabHeight(slab, floor.elevation);

			// 高さ基準（床仕上げ上端）を配置先ストーリの「FL」レベルへバインドする。offset は
			// FL からの高低差（一般部 0・床レベル指定時は ±差分）。これをしないと編集時に
			// 高さがレイヤ基準へリセットされて実形状と矛盾する。
			VectorWorks::SStoryObjectData bound;
			bound.fBound = VectorWorks::eStoryObjectBound_Story;
			bound.fBoundStory = static_cast<Sint8>(floor.bound.storyOffset);
			bound.fLayerLevelType = TXString(floor.bound.level.c_str());
			bound.fOffset = floor.bound.offset;
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
