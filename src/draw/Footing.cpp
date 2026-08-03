//
//	draw/Footing.cpp
//
//	基礎描画の実装。命令セットの立上り（WallCommand）を壁オブジェクトへ、底盤
//	（SlabCommand）をスラブオブジェクトへ配置する。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには
//	入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	【立上りの描画手順】
//	  1. CreateWall(始点, 終点, 壁厚) で壁芯から壁を作る。**Python 版の
//	     DoubLines(壁厚) → Wall(壁芯) という 2 手順は要らない**——ISDK の CreateWall は
//	     壁厚を引数に取る（ci-debug の sdk-grep で確認）。
//	  2. クラス分けと描画属性の by-class 設定。
//	  3. SetWallOverallHeights で下端・上端をストーリレベルへバインドする。
//	     **壁だけは汎用の SetObjectStoryBound では高さ基準が確定せず**、デザインレイヤの
//	     「壁の高さ（レイヤ設定）」に従ってしまう（構造材・スラブでは SetObjectStoryBound が
//	     効くが、壁は専用関数が要る。Python 版 CLAUDE.md「基礎」節）。命令の
//	     bottomBound / topBound の storyOffset（0=自階・1=上階）がそのまま story 引数になる。
//	  4. 壁スタイル（kWallStyle）を適用する。
//	  5. ResetObject で反映。
//	壁を作れない場合は壁芯の直線にフォールバックする（1 本の失敗で全体を止めない）。
//
//	【壁厚の再設定】壁スタイルは 150mm 厚の複合壁なので、**当てると壁厚がスタイル側の値に
//	上書きされうる**（Python 版はスタイルを当てっぱなしで、この点は VW 上で確認していない）。
//	命令の壁厚（120 / 150 / 300mm と実データでも混在する）を保つため、スタイル適用の直後に
//	SetWallWidth で命令の壁厚を入れ直す。挙動の切り替えは kReassertWallWidth の 1 か所で
//	行える（ローカル確認で「スタイルを当てても壁厚が命令どおり」なら false にしてよい）。
//
//	【底盤の描画手順】床板（draw/Floor）と同じ作法で、共通部分は draw/DrawUtil にある
//	（SetSlabComponents / SetSlabDatum / ResolveSlabStyle）。違うのはスタイル名
//	（"基礎スラブ - コンクリート …"）と構成層（コンクリート／捨てコン／砕石）だけ。
//	SetSlabHeight は厚みではなく**基準面の高さ**（絶対 Z）を設定する関数である点に注意
//	（Python 版 #70 と同じ落とし穴）。基礎ストーリは GL=0 なので絶対 Z がそのまま渡せる。
//
//	実描画（壁の高さ基準・壁スタイル・底盤の天端とスラブスタイル）はローカルの
//	VectorWorks で目視確認する方針（ROADMAP.md M9「ローカル確認」）。
//

#include "PluginPrefix.h"
#include "draw/Footing.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"
#include "core/Progress.h"

#include "VWFC/VWObjects/VWPolygon2DObj.h"

#include <cstddef>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 立上りへ適用する壁スタイル名（Python 版 vw/footing.py の
		// '基礎 - 木造ベタ基礎150mm'。「壁 基礎立ち上がり」フォルダ内のウォールスタイル）。
		// 見つからなければスタイル無しで描く（クラス属性だけが効く）。
		constexpr const char* kWallStyle = "基礎 - 木造ベタ基礎150mm";

		// SetWallStyle に渡す壁芯からのオフセット（選択側／置換側とも 0＝壁芯に揃える）。
		constexpr double kWallStyleOffset = 0.0;

		// 壁スタイル適用後に命令の壁厚を入れ直すか（ヘッダ冒頭「壁厚の再設定」）。
		constexpr bool kReassertWallWidth = true;

		// SetObjectStoryBound に渡すバウンド ID。スラブは高さ基準を 1 つだけ持つ
		// （draw/Floor と同じ）。型は SDK の TObjectBoundID（= Sint32）だが、その別名は
		// SDK の名前空間の中にあるため、ここでは実体の Sint32 で持つ（暗黙変換で同じ）。
		constexpr Sint32 kSlabBoundID = 0;

		// 命令の高さ基準（StoryBoundCommand）を SDK の SStoryObjectData へ写す。
		VectorWorks::SStoryObjectData StoryBound(const core::StoryBoundCommand& bound)
		{
			VectorWorks::SStoryObjectData data;
			data.fBound = VectorWorks::eStoryObjectBound_Story;
			data.fBoundStory = static_cast<Sint8>(bound.storyOffset);
			data.fLayerLevelType = TXString(bound.level.c_str());
			data.fOffset = bound.offset;
			return data;
		}

		// 壁スタイルの索引を返す（無ければ 0）。ISDK には名前から引く呼び出しが無いので、
		// 名前付きオブジェクトを GetNamedObject で引いて内部索引に直す（draw/Member の
		// プラグインスタイル解決と同じ作法）。
		InternalIndex ResolveWallStyle()
		{
			MCObjectHandle style = gSDK->GetNamedObject(TXString(kWallStyle));
			if (style == nil)
				return 0;
			return gSDK->GetObjectInternalIndex(style);
		}

		// 立上り 1 本を壁として描く。壁を作れなければ壁芯の直線にフォールバックする。
		// 配置できたら true。
		bool DrawOneWall(const core::WallCommand& wall, InternalIndex style)
		{
			const WorldPt start(wall.start.x, wall.start.y);
			const WorldPt end(wall.end.x, wall.end.y);

			MCObjectHandle object = gSDK->CreateWall(start, end, wall.thickness);
			if (object == nil)
			{
				// フォールバック: 壁芯の直線（クラス付き）を残す（命令の位置は図面に残る）。
				// 2D ポリラインで作るのは他の要素のフォールバックと同じ作法（draw/Grid・
				// draw/Member）。
				VWPolygon2DObj line(
					{VWPoint2D(wall.start.x, wall.start.y), VWPoint2D(wall.end.x, wall.end.y)});
				line.SetClosed(false);
				const MCObjectHandle lineHandle = line.GetThisObject();
				if (lineHandle == nil)
					return false;
				SetClassByName(lineHandle, wall.drawClass);
				SetAllAttributesByClass(lineHandle);
				return true;
			}

			SetClassByName(object, wall.drawClass);
			SetAllAttributesByClass(object);

			// 高さは壁専用の SetWallOverallHeights でストーリレベルへバインドする
			// （ヘッダ冒頭「立上りの描画手順」3）。
			gSDK->SetWallOverallHeights(object, StoryBound(wall.bottomBound),
										StoryBound(wall.topBound));

			if (style != 0)
			{
				gSDK->SetWallStyle(object, style, kWallStyleOffset, kWallStyleOffset);
				if (kReassertWallWidth)
					gSDK->SetWallWidth(object, wall.thickness);
			}

			gSDK->ResetObject(object);
			return true;
		}

		// 底盤 1 枚をスラブとして描く。スラブを作れなければ外形ポリゴンにフォールバックする。
		// 配置できたら true。手順は draw/Floor の DrawOne と同じ（共通部分は draw/DrawUtil）。
		bool DrawOneSlab(const core::SlabCommand& slab)
		{
			const MCObjectHandle profile = CreateClosedPolygon(slab.boundary);
			if (profile == nil)
				return false;

			MCObjectHandle object = gSDK->CreateSlab(profile);
			if (object == nil)
			{
				// フォールバック: 外形ポリゴンをクラス付きで残す。
				SetClassByName(profile, slab.drawClass);
				SetAllAttributesByClass(profile);
				return true;
			}

			SetClassByName(object, slab.drawClass);
			SetAllAttributesByClass(object);

			// 構成はコンクリート厚ごとのスラブスタイルで与える。用意できない場合だけ、
			// スタイルを外してスラブ本体のコンポーネントを直接組む（構成の欠落で底盤を
			// 失わないための保険。draw/Floor と同じ）。
			const InternalIndex style =
				ResolveSlabStyle(slab.styleName, slab.components, slab.datum);
			if (style != 0)
			{
				gSDK->SetSlabStyle(object, style);
			}
			else
			{
				gSDK->ConvertToUnstyledSlab(object);
				SetSlabComponents(object, slab.components);
				SetSlabDatum(object, slab.datum, static_cast<short>(slab.components.size()));
			}

			// SetSlabHeight は厚みではなく**基準面の高さ**（絶対 Z）を設定する。命令の
			// elevation はコンクリート天端の絶対 Z なのでそのまま渡す。
			gSDK->SetSlabHeight(object, slab.elevation);

			// 天端を底盤天端レベルへバインドする（offset はそのレベルからの差。主たる底盤は 0）。
			gSDK->SetObjectStoryBound(object, kSlabBoundID, StoryBound(slab.bound));

			gSDK->ResetObject(object);
			return true;
		}
	} // namespace

	std::size_t drawWalls(const core::Document& document, core::ProgressReporter& progress)
	{
		// 壁スタイルの解決は 1 回で足りる（全ての立上りが同じスタイルを使う）。
		const InternalIndex style = ResolveWallStyle();

		std::size_t drawn = 0;
		for (const core::WallCommand& wall : document.walls)
		{
			// 中止（進捗ダイアログのキャンセル）は残りを描かずに抜ける。進捗は本数で報告し、
			// 描画の前に 1 件進める（＝「いま何本目を描いているか」が見える）。
			if (progress.cancelled())
				break;
			progress.step();

			// 配置先レイヤ（"F-立上り"）が無い命令はスキップする（規約は ActivateExistingLayer）。
			if (ActivateExistingLayer(wall.layer) == nil)
				continue;

			if (DrawOneWall(wall, style))
				++drawn;
		}
		return drawn;
	}

	std::size_t drawSlabs(const core::Document& document, core::ProgressReporter& progress)
	{
		std::size_t drawn = 0;
		for (const core::SlabCommand& slab : document.slabs)
		{
			if (progress.cancelled())
				break;
			progress.step();

			// 配置先レイヤ（"F-底盤"）が無い命令はスキップする。
			if (ActivateExistingLayer(slab.layer) == nil)
				continue;

			if (DrawOneSlab(slab))
				++drawn;
		}
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
