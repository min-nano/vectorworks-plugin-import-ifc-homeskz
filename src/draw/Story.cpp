//
//	draw/Story.cpp
//
//	ストーリ描画の実装。Python 版 vw/story.py に対応する。命令セット（StoryCommand）から
//	VectorWorks のストーリ・ストーリレベル・デザインレイヤを生成する。【SDK 依存】
//	PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位はプラグインビルド
//	（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには入れない
//	（CLAUDE.md「依存の向きは厳守する」）。
//
//	使用する SDK API は ISDK（gSDK）の実在シグネチャに合わせている（Vectorworks 2026 SDK の
//	Interfaces/VectorWorks/ISDK.h。既存 draw/Grid.cpp と同じく gSDK 経由）:
//	  * gSDK->GetNamedObject(name)                        … 名前で名前付きオブジェクト取得
//	                                                        （ストーリ。VS の GetObject 相当）
//	  * gSDK->CreateStory(name, suffix)                   … ストーリ生成（引数は非 const TXString&）
//	  * gSDK->SetStoryElevation(story, elevation)         … ストーリ高さ設定（レベル追加前に）
//	  * gSDK->CreateLayerLevelType(type)                  … レベル種別の事前登録（非 const TXString&）
//	  * gSDK->CreateStoryLevelTemplate(name,scale,type,elevOff,wallH, idx)
//	                                                       … レベルテンプレート生成（idx は short&）
//	  * gSDK->AddStoryLevelFromTemplate(story, idx)       … テンプレートからレベル＋レイヤ生成
//	  * gSDK->GetLayerForStory(story, type)               … 生成レイヤのハンドル取得
//	  * gSDK->SetObjectName(layer, name)                  … レイヤ名を意図した名前へリネーム
//
//	Python 版 vw/story.py に忠実に写している。VW 2026 では AddStoryLevel +
//	AssociateLayerWithStory ではレイヤ→レベルの紐付けが UI 上「なし」になるため、バインドが
//	保証される CreateStoryLevelTemplate + AddStoryLevelFromTemplate を使う。テンプレートは
//	CreateStory の suffix を末尾に付けた名前でレイヤを作る（例 "1-FL-1"）ため、GetLayerForStory
//	で取り直して SetObjectName で意図した名前（"1-FL"）へ直す。
//
//	レイヤのスタック順の並べ替えは行わない（ヘッダ参照: VW 2026 ISDK にデザインレイヤの
//	重ね順変更呼び出しが無く、目的である伏図ビューポートの重ね順制御は per-viewport の
//	SetViewportLayerStackingOverride を使う M13 へ委ねる。希望順の計算は
//	core::desiredStoryLayerOrder に用意済み）。
//
//	実描画（ストーリ高さ・レベルのバインド・単位）はローカルの VectorWorks で目視確認する
//	（ROADMAP.md M3「ローカル確認」）。
//

#include "PluginPrefix.h"
#include "draw/Story.h"
#include "core/Document.h"
#include "core/Progress.h"

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
										 double offset, const std::string& desiredLayerName)
		{
			// CreateStoryLevelTemplate / CreateLayerLevelType は引数を非 const TXString& で
			// 取るため、一時オブジェクトではなく名前付き lvalue を渡す。
			TXString templateName(desiredLayerName.c_str());
			TXString levelTypeName(levelType.c_str());

			short templateIndex = -1;
			const bool ok =
				gSDK->CreateStoryLevelTemplate(templateName, kTemplateScale, levelTypeName, offset,
											   kTemplateWallHeight, templateIndex);
			if (!ok || templateIndex < 0)
				return;
			if (!gSDK->AddStoryLevelFromTemplate(story, templateIndex))
				return;
			// AddStoryLevelFromTemplate はレイヤ名に suffix を付ける（"1-FL-1"）。取り直して直す。
			MCObjectHandle layer = gSDK->GetLayerForStory(story, levelTypeName);
			if (layer != nil)
				gSDK->SetObjectName(layer, TXString(desiredLayerName.c_str()));
		}
	} // namespace

	std::size_t drawStories(const core::Document& document, core::ProgressReporter& progress)
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
		{
			TXString levelTypeName(levelType.c_str()); // CreateLayerLevelType は非 const TXString&
			gSDK->CreateLayerLevelType(levelTypeName);
		}

		std::size_t count = 0;
		for (const core::StoryCommand& command : commands)
		{
			// 中止（進捗ダイアログのキャンセル）は残りを作らずに抜ける。進捗は階数で報告し、
			// 生成の前に 1 件進める（＝「いま何階目を作っているか」が見える）。
			if (progress.cancelled())
				break;
			progress.step();

			// CreateStory は名前・接尾辞を非 const TXString& で取るため名前付き lvalue を渡す。
			TXString storyName(command.name.c_str());

			MCObjectHandle story = gSDK->GetNamedObject(storyName);
			if (story == nil)
			{
				TXString suffix(command.suffix.c_str());
				gSDK->CreateStory(storyName, suffix);
				story = gSDK->GetNamedObject(storyName);
			}
			if (story == nil)
				continue;

			// ストーリ高さは CreateStory 直後・レベル追加前に設定する。直後に設定しないと
			// 「既定高さ 0 のストーリが複数」となり次の CreateStory が衝突して失敗し得る。
			gSDK->SetStoryElevation(story, command.elevation);

			for (const core::LevelCommand& level : command.levels)
				CreateStoryLevelViaTemplate(story, level.type, level.offset, level.layer);

			++count;
		}
		return count;
	}
} // namespace HomeskzIfcImport::draw
