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

#include "VWFC/VWObjects/VWLayerObj.h" // レイヤ高さの読み取り（診断ログの計測）

#include <algorithm>
#include <cmath>
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

		// **レイヤを作る順序の向き。** true なら「前面に来るものから作る」。
		//
		// 【なぜ作る順序で決めるのか】並べ替え（reorderStoryLayers）の結果は、取り込み中に
		// 作るビューポートの描画へ届かない（実機で 4 通り試して全滅。draw/Story.h）。届かない
		// のは**並べ替え**であって、**作った順そのもの**は最初から図面の並びなので、希望順に
		// 沿って作れば並べ替えが要らなくなる——というのがこの向きの意味。
		//
		// **どちらが正しいかは実機でしか分からない。** 取り込み後に reorderStoryLayers が
		// 「1 つも動かさなかった」なら当たり、「逆順だった」と診断行に出たらこの定数を
		// 反転させる（draw/ExecuteDocument の診断行）。
		constexpr bool kCreateFrontLayerFirst = true;

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
		// レイヤ 1 枚の素性を「名前[ストーリ/レベル種別/レベル高さ]」の形にする（診断ログ用）。
		//
		// **VW がストーリ従属レイヤをどんな規則で並べているかを突き止めるための計測。** 実機で
		// 分かっているのは「作った順は完全に無視され、ストーリ単位でまとまって上階が前面・
		// 各ストーリの先頭が n-FL」というところまで。残る候補はレベルの高さか、レベル種別の
		// 登録順か、その他の内部順なので、突き合わせに要る値をそのまま並べる。
		std::string DescribeLayer(MCObjectHandle layer)
		{
			TXString name;
			gSDK->GetObjectName(layer, name);

			std::string storyName = "-";
			const MCObjectHandle story = gSDK->GetStoryOfLayer(layer);
			if (story != nil)
			{
				TXString storyText;
				gSDK->GetObjectName(story, storyText);
				storyName = storyText.GetStdString();
			}

			std::string levelType = gSDK->GetLayerLevelType(layer).GetStdString();
			if (levelType.empty())
				levelType = "-";

			std::string elevation;
			try
			{
				elevation = std::to_string(std::llround(VWLayerObj(layer).GetElevation()));
			}
			catch (...)
			{
				// 高さを読めなくても他の値は出す（1 枚の欠損で計測を止めない）。
				elevation = "?";
			}

			return name.GetStdString() + "[" + storyName + "/" + levelType + "/" + elevation + "]";
		}

		// 図面のレイヤの並びを**前面→背面**の 1 行にする（診断ログ用）。希望順に出てこない
		// レイヤ（ユーザーが自分で作ったもの）もそのまま出す——「VW がどう並べているか」を
		// 見るのが目的なので、省くと分からなくなる。
		std::string DescribeStackOrder(const std::vector<MCObjectHandle>& chain)
		{
			std::string text;
			for (const MCObjectHandle layer : std::ranges::reverse_view(chain))
			{
				if (!text.empty())
					text += " | ";
				text += DescribeLayer(layer);
			}
			return text;
		}

		// レベル種別の**登録順**（診断ログ用）。VW の並べ方がこの順に沿っているかを見る。
		std::string DescribeLevelTypes()
		{
			const short count = gSDK->GetNumLayerLevelTypes();
			std::string text;
			for (short index = 0; index < count; ++index)
			{
				if (!text.empty())
					text += " | ";
				text +=
					std::to_string(index) + "=" + gSDK->GetLayerLevelTypeName(index).GetStdString();
			}
			return text;
		}

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
		result.trace = "希望（前面→背面）: " +
					   [&]
		{
			std::string text;
			for (const std::string& name : desired)
			{
				if (!text.empty())
					text += " | ";
				text += name;
			}
			return text;
		}() +
					   "\nレベル種別の登録順: " + DescribeLevelTypes() +
					   "\n実際（並べ替え前・前面→背面。名前[ストーリ/レベル種別/レベル高さ]）: " +
					   DescribeStackOrder(DesignLayersInStackOrder());

		result.wasOrdered = MatchesStackOrder(wanted);
		result.ordered = result.wasOrdered;
		if (result.ordered)
			return result;

		// 逆順だったかも見ておく（作る順序の向きが逆だった、という切り分けのため。
		// draw/Story.cpp の kCreateFrontLayerFirst）。
		{
			const std::vector<MCObjectHandle> reversed(wanted.rbegin(), wanted.rend());
			result.wasReversed = MatchesStackOrder(reversed);
		}

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
		result.trace +=
			"\n実際（並べ替え後・前面→背面）: " + DescribeStackOrder(DesignLayersInStackOrder());
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

		// **ストーリを先に全部作り、レイヤは後からまとめて作る。** レイヤの重ね順は
		// **作る順で決まる**ものとして扱い（並べ替えは取り込み中の描画へ届かない。
		// draw/Story.h の reorderStoryLayers）、希望順に沿った順序で作りたいため、
		// 「ストーリを作る」段と「レベル（＝レイヤ）を作る」段を分ける。
		std::size_t count = 0;
		std::vector<MCObjectHandle> stories; // commands と同じ並び（作れなかった階は nil）
		stories.reserve(commands.size());
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
			stories.push_back(story);
			if (story == nil)
				continue;

			// ストーリ高さは CreateStory 直後・レベル追加前に設定する。直後に設定しないと
			// 「既定高さ 0 のストーリが複数」となり次の CreateStory が衝突して失敗し得る。
			gSDK->SetStoryElevation(story, command.elevation);
			++count;
		}

		// レイヤ（ストーリレベル）を**希望する重ね順に沿った順序で**作る。順序の意味は
		// kCreateFrontLayerFirst（前面のものから作る＝新しいレイヤが背面へ入る、という
		// VW の並べ方を前提にする）。希望順に出てこないレイヤ（想定外）は最後に作る。
		std::vector<const core::LevelCommand*> levels; // 作る順
		std::vector<MCObjectHandle> owners;			   // levels と同じ並びのストーリ
		const std::vector<std::string> desired =
			core::desiredStoryLayerOrder(commands, planMarkLayerNames(document));
		const auto pushLevel = [&](const core::LevelCommand& level, MCObjectHandle story)
		{
			levels.push_back(&level);
			owners.push_back(story);
		};
		std::vector<bool> queued; // levels に入れたか（命令の並びで持つ）
		std::size_t levelCount = 0;
		for (const core::StoryCommand& command : commands)
			levelCount += command.levels.size();
		queued.assign(levelCount, false);
		const auto forEachLevel = [&](const auto& visit)
		{
			std::size_t index = 0;
			for (std::size_t s = 0; s < commands.size() && s < stories.size(); ++s)
			{
				for (const core::LevelCommand& level : commands[s].levels)
					visit(index++, level, stories[s]);
			}
		};
		for (const std::string& name : desired)
		{
			forEachLevel(
				[&](std::size_t index, const core::LevelCommand& level, MCObjectHandle story)
				{
					if (queued[index] || level.layer != name || story == nil)
						return;
					queued[index] = true;
					pushLevel(level, story);
				});
		}
		// 希望順に載っていないレベル（あれば）は最後に。
		forEachLevel(
			[&](std::size_t index, const core::LevelCommand& level, MCObjectHandle story)
			{
				if (queued[index] || story == nil)
					return;
				queued[index] = true;
				pushLevel(level, story);
			});

		if (!kCreateFrontLayerFirst)
		{
			std::ranges::reverse(levels);
			std::ranges::reverse(owners);
		}
		for (std::size_t i = 0; i < levels.size(); ++i)
			CreateStoryLevelViaTemplate(owners[i], levels[i]->type, levels[i]->offset,
										levels[i]->layer);

		return count;
	}
} // namespace HomeskzIfcImport::draw
