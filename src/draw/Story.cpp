//
//	draw/Story.cpp
//
//	ストーリ描画の実装。命令セット（StoryCommand）から VectorWorks の
//	ストーリ・ストーリレベル・デザインレイヤを生成する。【SDK 依存】PluginPrefix.h
//	（VectorWorks SDK）を include するため、この翻訳単位はプラグインビルド（SDK あり）
//	でのみコンパイルされ、無 SDK の core/parse ライブラリには入れない（CLAUDE.md「依存の向きは
//	厳守する」）。
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
//	VW 2026 では AddStoryLevel + AssociateLayerWithStory ではレイヤ→レベルの紐付けが UI
//	上「なし」になるため、バインドが保証される CreateStoryLevelTemplate +
//	AddStoryLevelFromTemplate を使う。テンプレートは CreateStory の suffix を末尾に付けた名前
//	でレイヤを作る（例 "1-FL-1"）ため、GetLayerForStory で取り直して SetObjectName
//	で意図した名前（"1-FL"）へ直す。
//
//	レイヤのスタック順の並べ替え（reorderStoryLayers）もここに置く。**ISDK の
//	InsertObjectAfter / InsertObjectBefore** で図面のオブジェクト列（＝レイヤの並び）を
//	組み替える——M3 では「重ね順を変える呼び出しが無い」と見て per-viewport 上書きへ
//	委ねたが、実機でそちらは効かず、この 2 つが VS の HMoveForward 相当だと分かった
//	（ヘッダの reorderStoryLayers 参照）。
//
//	実描画（ストーリ高さ・レベルのバインド・単位）はローカルの VectorWorks で目視確認する
//	（docs/DEV-NOTES.md M3「ローカル確認」）。
//

#include "PluginPrefix.h"
#include "draw/Story.h"
#include "draw/ColumnMark.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"
#include "core/Progress.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// レベルテンプレートの既定値。スケール 1.0、壁高 2400mm。実描画の高さは要素側の
		// ストーリバウンドで決まるためここは器の既定。
		constexpr double kTemplateScale = 1.0;
		constexpr double kTemplateWallHeight = 2400.0;

		// 1 つのストーリレベルをレベルテンプレート経由で生成し、紐づくレイヤを意図した名前へ
		// リネームする。生成に失敗したら静かに戻る（1 レベルの欠損で全体を止めない）。
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
			{
				gSDK->SetObjectName(layer, TXString(desiredLayerName.c_str()));
				// **このインポートが作ったレイヤ**として undo イベントへ登録する。取り消すと
				// このレイヤごと——上に描いた構造材・壁・スラブ・シンボルもまとめて——消える
				// （draw/DrawUtil.h「なぜレイヤを記録するのか」）。
				RecordCreatedLayer(layer);
			}
		}

		// wanted（図面のオブジェクト列と同じ**背面→前面**の並び）が、いまの図面でその順に
		// 並んでいるか。**間に別のレイヤが挟まっていてもよい**（ユーザーが自分で作った
		// レイヤの位置には口を出さない）——見るのは wanted どうしの前後関係だけ。
		bool MatchesStackOrder(const std::vector<MCObjectHandle>& wanted)
		{
			const std::vector<MCObjectHandle> chain = DesignLayersInStackOrder();
			std::size_t at = 0;
			for (const MCObjectHandle layer : wanted)
			{
				// chain を前から舐めて wanted を順に拾えるなら、前後関係は希望どおり。
				const auto found =
					std::find(chain.begin() + static_cast<std::ptrdiff_t>(at), chain.end(), layer);
				if (found == chain.end())
					return false;
				at = static_cast<std::size_t>(found - chain.begin()) + 1;
			}
			return true;
		}
	} // namespace

	LayerOrderResult reorderStoryLayers(const core::Document& document)
	{
		// 希望順は**前面→背面**（"共通" が先頭＝最前面。core::desiredStoryLayerOrder）。
		// 伏図記号レイヤ（"{to}-柱伏図記号"。M12）はストーリに属さない独立レイヤで story
		// 命令の levels に現れないので、**topLayers として通り芯 "共通" の直下へ差し込む**。
		// 柱・梁の記号なので、床・野地板より前面に無いと覆い隠される。
		const std::vector<std::string> desired =
			core::desiredStoryLayerOrder(document.stories, planMarkLayerNames(document));

		// 図面のオブジェクト列は**背面→前面**（先頭が最背面で、NextObject が前面へ向かう）ので、
		// 希望順を逆に辿って「あるべき列の並び」を作る。希望順に出てこないレイヤ（ユーザーが
		// 別途作ったもの）は触らない。GetNamedLayer が nil を返すレイヤは黙って飛ばす
		// （要素の描画がスキップされてレイヤが無い場合など）。
		std::vector<MCObjectHandle> wanted;
		for (const std::string& name : std::ranges::reverse_view(desired))
		{
			const MCObjectHandle layer = gSDK->GetNamedLayer(TXString(name.c_str()));
			if (layer != nil)
				wanted.push_back(layer);
		}

		// **既に希望どおりなら 1 つも動かさない。** 呼び直しても図面を触らないので、
		// 「描画の前に並べて、伏図の直前に確かめる」という 2 度呼びが安全にできる。
		LayerOrderResult result;
		result.ordered = MatchesStackOrder(wanted);
		if (result.ordered)
			return result;

		// 直前に置いたレイヤの「後ろ」（＝前面側）へ次を挿していくと、列全体が希望どおりになる。
		MCObjectHandle previous = nil;
		for (const MCObjectHandle layer : wanted)
		{
			if (previous != nil && gSDK->InsertObjectAfter(layer, previous))
				++result.moved;
			previous = layer;
		}

		// **並び替わったかは読み戻して確かめる**（moved は「呼び出しが true を返した数」で
		// しかない）。ここが false なら伏図で床・野地板が柱・梁を覆う。
		result.ordered = MatchesStackOrder(wanted);
		return result;
	}

	std::size_t drawStories(const core::Document& document, core::ProgressReporter& progress)
	{
		const std::vector<core::StoryCommand>& commands = document.stories;
		if (commands.empty())
			return 0;

		// 命令セットに登場するレベル種別を登場順に事前登録する。
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
