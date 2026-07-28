//
//	draw/Story.cpp
//
//	ストーリ描画の実装。Python 版 vw/story.py に対応する。命令セット（StoryCommand）から
//	VectorWorks のストーリ・ストーリレベル・デザインレイヤを生成し、希望スタック順へ
//	並べ替える。【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この
//	翻訳単位はプラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse
//	ライブラリには入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	使用する SDK API は ISDK（gSDK）の VectorScript 相当メソッドに合わせている
//	（既存 draw/Grid.cpp と同じく gSDK 経由。Vectorworks 2026 SDK の ISDK.h）:
//	  * gSDK->GetObject(name)                         … 名前でオブジェクト（ストーリ）取得
//	  * gSDK->CreateStory(name, suffix)               … ストーリ生成
//	  * gSDK->SetStoryElevationN(story, elevation)    … ストーリ高さ設定（レベル追加前に）
//	  * gSDK->CreateLayerLevelType(type)              … レベル種別の事前登録
//	  * gSDK->CreateLevelTemplateN(name,scale,type,elev,wallH, idx) … レベルテンプレート生成
//	  * gSDK->AddLevelFromTemplate(story, idx)        … テンプレートからレベル＋レイヤ生成
//	  * gSDK->GetLayerForStory(story, type)           … 生成レイヤのハンドル取得
//	  * gSDK->SetName(layer, name)                    … レイヤ名を意図した名前へリネーム
//	  * gSDK->GetNamedLayer(name)                     … 名前でレイヤ取得（並べ替え用）
//	  * gSDK->FLayer / NextLayer                      … レイヤ走査（下→上）
//	  * gSDK->HMoveForward(layer, toFront)            … レイヤを 1 段前方（上）へ移動
//
//	Python 版 vw/story.py に忠実に写している。VW 2026 では AddStoryLevelN +
//	AssociateLayerWithStory ではレイヤ→レベルの紐付けが UI 上「なし」になるため、
//	バインドが保証される CreateLevelTemplateN + AddLevelFromTemplate を使う。
//	AddLevelFromTemplate は CreateStory の suffix を末尾に付けた名前でレイヤを作る
//	（例 "1-FL-1"）ため、GetLayerForStory で取り直して SetName で意図した名前へ直す。
//
//	希望スタック順の**計算**は core::desiredStoryLayerOrder（SDK 非依存・テスト済み）に
//	あり、ここはその順に HMoveForward で実レイヤを並べ替えるだけ。HMoveForward は
//	toFront=true でレイヤが**削除される**ため必ず false で 1 段ずつ送り、位置が変わら
//	なくなったら（端に到達）即座に打ち切る（Python 版 move_layer_directly_above の注記）。
//
//	実描画（ストーリ高さ・レベルのバインド・レイヤのスタック順）はローカルの
//	VectorWorks で目視確認する（ROADMAP.md M3「ローカル確認」）。ストーリ系 PIO の
//	パラメータ・単位は SDK ビルドと VW 実機でのみ最終確認できる。
//

#include "PluginPrefix.h"
#include "draw/Story.h"
#include "core/Document.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// レベルテンプレートの既定値（Python 版 vw/story.py と同値）。スケール 1.0、
		// 壁高 2400mm。実描画の高さは要素側のストーリバウンドで決まるためここは器の既定。
		constexpr double kTemplateScale = 1.0;
		constexpr double kTemplateWallHeight = 2400.0;

		// 1 つのストーリレベルをレベルテンプレート経由で生成し、紐づくレイヤを意図した
		// 名前へリネームする（Python 版 create_story_level_via_template）。生成に失敗した
		// ら静かに戻る（1 レベルの欠損で全体を止めない）。
		void CreateStoryLevelViaTemplate(MCObjectHandle story, const std::string& levelType,
										 double elevation, const std::string& desiredLayerName)
		{
			Sint32 templateIndex = -1;
			const Boolean ok = gSDK->CreateLevelTemplateN(
				TXString(desiredLayerName.c_str()), kTemplateScale, TXString(levelType.c_str()),
				elevation, kTemplateWallHeight, templateIndex);
			if (!ok || templateIndex < 0)
				return;
			if (!gSDK->AddLevelFromTemplate(story, templateIndex))
				return;
			// AddLevelFromTemplate はレイヤ名に suffix を付ける（"1-FL-1"）。取り直して直す。
			MCObjectHandle layer = gSDK->GetLayerForStory(story, TXString(levelType.c_str()));
			if (layer != nil)
				gSDK->SetName(layer, TXString(desiredLayerName.c_str()));
		}

		// ドキュメント内のデザインレイヤ数を返す（並べ替えの反復上限。Python 版 count_layers）。
		std::size_t CountLayers()
		{
			std::size_t n = 0;
			MCObjectHandle layer = gSDK->FLayer();
			while (layer != nil)
			{
				++n;
				layer = gSDK->NextLayer(layer);
			}
			return n;
		}

		// レイヤ走査（FLayer→NextLayer は下→上）での target の位置を返す（無ければ -1。
		// Python 版 layer_index）。
		long LayerIndex(MCObjectHandle target)
		{
			long idx = 0;
			MCObjectHandle layer = gSDK->FLayer();
			while (layer != nil)
			{
				if (layer == target)
					return idx;
				++idx;
				layer = gSDK->NextLayer(layer);
			}
			return -1;
		}

		// target レイヤを anchor レイヤの直上へ移動する（Python 版 move_layer_directly_above）。
		// HMoveForward(h, false) で 1 段ずつ前方（上）へ送り、anchor の次が target になったら
		// 止める。toFront=true はレイヤ削除の副作用があるため使わない。1 段送っても位置が
		// 変わらなくなったら（端に到達）打ち切り、無限ループを避けるため回数も頭打ちにする。
		void MoveLayerDirectlyAbove(MCObjectHandle target, MCObjectHandle anchor,
									std::size_t maxSteps)
		{
			if (target == nil || anchor == nil || target == anchor)
				return;
			long prevIndex = LayerIndex(target);
			for (std::size_t step = 0; step < maxSteps; ++step)
			{
				if (gSDK->NextLayer(anchor) == target)
					return; // 既に anchor の直上
				gSDK->HMoveForward(target, false);
				const long curIndex = LayerIndex(target);
				if (curIndex == prevIndex)
					return; // これ以上前方へ動かない（端に到達）。送り続けない。
				prevIndex = curIndex;
			}
		}
	} // namespace

	std::size_t drawStories(const core::Document& document)
	{
		const std::vector<core::StoryCommand>& commands = document.stories;
		if (commands.empty())
			return 0;

		// 命令セットに登場するレベル種別を登場順に事前登録する（Python 版 execute_stories）。
		std::vector<std::string> levelTypes;
		for (const core::StoryCommand& command : commands)
		{
			for (const core::LevelCommand& level : command.levels)
			{
				bool seen = false;
				for (const std::string& t : levelTypes)
				{
					if (t == level.type)
					{
						seen = true;
						break;
					}
				}
				if (!seen)
					levelTypes.push_back(level.type);
			}
		}
		for (const std::string& levelType : levelTypes)
			gSDK->CreateLayerLevelType(TXString(levelType.c_str()));

		std::size_t count = 0;
		for (const core::StoryCommand& command : commands)
		{
			const TXString storyName(command.name.c_str());

			MCObjectHandle story = gSDK->GetObject(storyName);
			if (story == nil)
			{
				gSDK->CreateStory(storyName, TXString(command.suffix.c_str()));
				story = gSDK->GetObject(storyName);
			}
			if (story == nil)
				continue;

			// ストーリ高さは CreateStory 直後・レベル追加前に設定する。直後に設定しないと
			// 「既定高さ 0 のストーリが複数」となり次の CreateStory が衝突して失敗し得る。
			gSDK->SetStoryElevationN(story, command.elevation);

			for (const core::LevelCommand& level : command.levels)
				CreateStoryLevelViaTemplate(story, level.type, level.offset, level.layer);

			++count;
		}

		// スタック順の並べ替えは通り芯レイヤ "共通" 生成後に行う必要があるため、ここでは
		// 行わず executeDocument が全描画後に reorderStoryLayers を呼ぶ（Python 版と同じ）。
		return count;
	}

	void reorderStoryLayers(const core::Document& document)
	{
		if (document.stories.empty())
			return;
		const std::size_t maxSteps = CountLayers();
		if (maxSteps == 0)
			return;

		// 希望スタック順（SDK 非依存・テスト済みの計算）。topLayers は M12 以降で渡す。
		const std::vector<std::string> order = core::desiredStoryLayerOrder(document.stories);

		// 希望順の全レイヤを 1 本の並びとみなし、隣接ペアごとに上側を下側の直上へ移動する。
		// 下のペアから順（末尾→先頭）に処理して、上のペアを直す際に確定済みの下を崩さない。
		// 未生成のレイヤ（通り芯描画前の "共通" 等）は GetNamedLayer が nil を返しスキップ。
		for (std::size_t i = order.size(); i-- > 1;)
		{
			MCObjectHandle upper = gSDK->GetNamedLayer(TXString(order[i - 1].c_str()));
			MCObjectHandle lower = gSDK->GetNamedLayer(TXString(order[i].c_str()));
			MoveLayerDirectlyAbove(upper, lower, maxSteps);
		}
	}
} // namespace HomeskzIfcImport::draw
