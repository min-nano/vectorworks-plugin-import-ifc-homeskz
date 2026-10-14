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
//	  * gSDK->CreateLayer / SetLayerLevelType / AssociateLayerWithStory / GetStoryOfLayer
//	                                                       … レベルを持たないレイヤを階に結ぶ
//	                                                         （柱の span レイヤ。下記）と、
//	                                                         そのとき借りたレベルへレイヤを結ぶ
//	  * gSDK->SetStoryLevelElevation(story, type, elev)   … 借りたレベルの階内の相対Z を設定
//	  * gSDK->SetLayerScaleN(layer, scale)                … 借りたレベルに結んだレイヤの縮尺
//
//	VW 2026 では AddStoryLevel + AssociateLayerWithStory ではレイヤ→レベルの紐付けが UI
//	上「なし」になるため、バインドが保証される CreateStoryLevelTemplate +
//	AddStoryLevelFromTemplate を使う。テンプレートは CreateStory の suffix を末尾に付けた名前
//	でレイヤを作る（例 "1±0-FL-1"）ため、GetLayerForStory で再取得して SetObjectName
//	で意図した名前（"1±0-FL"）へ直す。
//
//	**柱の span レイヤ（StoryCommand::storyLayers）はレベルを持たせず、階にだけ結ぶ**（ご要望。
//	M44）。素のレイヤ（種別 ''）は AssociateLayerWithStory に弾かれるので、その階で未使用の
//	仮の種別を与えて結び、結んだあとで種別を外す（SDK リファレンス issue #221・Findings
//	「Layers and Stories」の「レベルを持たないレイヤを階に結ぶ」。実機確認済み）。
//	**仮の種別には、その階でこのあと横架材などのレイヤが使うレベル種別を借りる**——結ぶと
//	階にその種別のレベルができ、種別を外してもレベルは階に残る。専用の種別（以前は "柱"）を
//	使うと、どのレイヤにも使われないレベル "柱" が各階に残っていた。借りた種別のレベルは、
//	あとでその種別のレイヤを作るときに再利用する（AttachLayerToLeftoverLevel）。柱の
//	上下端のバウンドはバウンドに書いた種別（横架材天端）で解決され、入れ物から要るのは
//	階だけなので、レベル付きのレイヤのときと同じ高さになる。レイヤの高さは階の高さ。
//	レベルの無いレイヤは高さ順に自動整列されないが、並びは reorderStoryLayers が
//	InsertObjectAfter で決めるので変わらない。
//
//	レイヤのスタック順の並べ替え（reorderStoryLayers）もここに置く。**ISDK の
//	InsertObjectAfter / InsertObjectBefore** で図面のオブジェクト列（＝レイヤの並び）を
//	並べ替える——M3 では「重ね順を変える呼び出しが無い」と判断して per-viewport 上書きへ
//	委ねたが、実機でそちらは反映されず、この 2 つが VS の HMoveForward 相当だと判明した
//	（ヘッダの reorderStoryLayers 参照）。
//
//	実描画（ストーリ高さ・レベルのバインド・単位）はローカルの VectorWorks で目視確認する
//	（docs/DEV-NOTES.md M3）。
//

#include "PluginPrefix.h"
#include "draw/Story.h"
#include "draw/ColumnMark.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"
#include "core/Progress.h"

#include <cstddef>
#include <ranges>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// レベルテンプレートの既定値。スケール 1.0、壁高 2400mm。実描画の高さは要素側の
		// ストーリバウンドで決まるためここは形式上の既定値。
		constexpr double kTemplateScale = 1.0;
		constexpr double kTemplateWallHeight = 2400.0;

		// 1 つのストーリレベルをレベルテンプレート経由で生成し、紐づくレイヤを意図した名前へ
		// リネームする。生成に失敗したら通知せずに戻る（1 レベルの欠損で全体を止めない）。
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
			// AddStoryLevelFromTemplate はレイヤ名に suffix を付ける（"1±0-FL-1"）。
			// 再取得して名前を直す。
			MCObjectHandle layer = gSDK->GetLayerForStory(story, levelTypeName);
			if (layer != nil)
			{
				gSDK->SetObjectName(layer, TXString(desiredLayerName.c_str()));
				// **このインポートが作ったレイヤ**として undo イベントへ登録する。取り消すと
				// このレイヤごと——上に描画した構造材・壁・スラブ・シンボルもまとめて——消える
				// （draw/DrawUtil.h「なぜレイヤを記録するのか」）。
				RecordCreatedLayer(layer);
			}
		}

		// レベルを持たないデザインレイヤ layerName を作り、階 story にだけ結ぶ
		// （OIP / オーガナイザで「ストーリ＝その階・ストーリレベル＝なし」）。結べたら true。
		//
		// 手順は Findings のとおり、この順番に意味がある: CreateLayer → 仮の種別 tempType を
		// 与える → AssociateLayerWithStory（素のレイヤ＝種別 '' は弾かれる）→ 種別を外す
		// （階は残る）。結べたかは戻り値ではなく GetStoryOfLayer の読み戻しで確かめる。
		//
		// tempType は**その階でまだレイヤが使っていない種別**でなければならない（使用中の種別は
		// 弾かれる。Findings）。結ぶと階にその種別のレベルが残るので、呼び出し側はこのあと
		// その階で使うレベル種別を渡し、残ったレベルを再利用する（drawStories）。借りられる
		// 種別が無ければ空を渡す——同名のレイヤの確かめだけを行い、無ければ false を返す。
		//
		// **結べなければ作ったレイヤを消して false を返す**——階に属さないレイヤの上では
		// 柱の上下端のバウンドが同じ高さへ潰れ、長さ 0 の柱になる（Findings）。消しておけば
		// 呼び出し側が従来のレベル付きのレイヤで作り直せる。同名のレイヤが既に在れば作らず、
		// その階に属していれば true（取り込み前から在ったレイヤとして記録する）。
		bool CreateStoryOnlyLayer(MCObjectHandle story, const std::string& layerName,
								  const std::string& tempType)
		{
			const TXString name(layerName.c_str());
			if (const MCObjectHandle existing = gSDK->GetNamedLayer(name); existing != nil)
			{
				NoteExistingLayerUsed(existing, layerName);
				return gSDK->GetStoryOfLayer(existing) == story;
			}
			if (tempType.empty())
				return false;
			MCObjectHandle layer = gSDK->CreateLayer(name, static_cast<short>(LayerKind::Design));
			if (layer == nil)
				return false;
			TXString tempTypeName(tempType.c_str()); // SetLayerLevelType は非 const TXString&
			gSDK->SetLayerLevelType(layer, tempTypeName);
			const bool bound = gSDK->AssociateLayerWithStory(layer, story) &&
							   gSDK->GetStoryOfLayer(layer) == story;
			TXString noType;
			gSDK->SetLayerLevelType(layer, noType);
			if (!bound)
			{
				gSDK->DeleteObject(layer);
				return false;
			}
			// このインポートが作ったレイヤとして undo イベントへ登録する
			// （CreateStoryLevelViaTemplate と同じ）。
			RecordCreatedLayer(layer);
			return true;
		}

		// CreateStoryOnlyLayer が仮の種別として借り、レイヤの無いまま階に残ったレベル
		// levelType へ、デザインレイヤ desiredLayerName を結ぶ。レベルの階内の相対Z を
		// offset にする。結べたら true。
		//
		// テンプレート経由（CreateStoryLevelViaTemplate）で作ると同じ種別のレベルが階に
		// もう 1 つでき、借りたレベルがレイヤの無いまま残る。そこで CreateStoryOnlyLayer と
		// 同じく種別を与えて AssociateLayerWithStory で結ぶ（結んだあと種別は外さない）。
		// 結べたかは GetStoryOfLayer と GetLayerForStory の読み戻しで確かめ、**結べなければ
		// 作ったレイヤを消して false を返す**（呼び出し側がテンプレート経由で作り直す）。
		// 同名のレイヤが既に在るときも false（テンプレート経由の従来の扱いに任せる）。
		bool AttachLayerToLeftoverLevel(MCObjectHandle story, const std::string& levelType,
										double offset, const std::string& desiredLayerName)
		{
			const TXString name(desiredLayerName.c_str());
			if (gSDK->GetNamedLayer(name) != nil)
				return false;
			MCObjectHandle layer = gSDK->CreateLayer(name, static_cast<short>(LayerKind::Design));
			if (layer == nil)
				return false;
			TXString levelTypeName(levelType.c_str()); // SetLayerLevelType は非 const TXString&
			gSDK->SetLayerLevelType(layer, levelTypeName);
			const bool bound = gSDK->AssociateLayerWithStory(layer, story) &&
							   gSDK->GetStoryOfLayer(layer) == story &&
							   gSDK->GetLayerForStory(story, levelTypeName) == layer;
			if (!bound)
			{
				gSDK->DeleteObject(layer);
				return false;
			}
			// テンプレート経由と同じ縮尺・相対Z にそろえる（借りたレベルの相対Z は 0）。
			gSDK->SetLayerScaleN(layer, kTemplateScale);
			gSDK->SetStoryLevelElevation(story, levelTypeName, offset);
			RecordCreatedLayer(layer);
			return true;
		}

		// 階 story の levels のうち、その階でまだレイヤが使っていない最初のレベル種別を返す
		// （CreateStoryOnlyLayer に仮の種別として貸す。無ければ空）。
		std::string BorrowableLevelType(MCObjectHandle story,
										const std::vector<core::LevelCommand>& levels)
		{
			for (const core::LevelCommand& level : levels)
			{
				const TXString levelTypeName(level.type.c_str());
				if (gSDK->GetLayerForStory(story, levelTypeName) == nil)
					return level.type;
			}
			return {};
		}
	} // namespace

	std::size_t reorderStoryLayers(const core::Document& document)
	{
		// 希望順は**前面→背面**（"共通" が先頭＝最前面。core::desiredStoryLayerOrder）。
		// 伏図記号レイヤ（"{to}-柱伏図記号"。M12）はストーリに属さない独立レイヤで story
		// 命令の levels に現れないので、**topLayers として通り芯 "共通" の直下へ差し込む**。
		// 柱・梁の記号なので、床・野地板より前面に無いと覆い隠される。
		const std::vector<std::string> desired =
			core::desiredStoryLayerOrder(document.stories, planMarkLayerNames(document));

		// 図面のオブジェクト列は**背面→前面**（先頭が最背面で、NextObject が前面へ向かう）。
		// したがって希望順を**逆から**辿り、直前に置いたレイヤの「後ろ」へ次を挿していくと、
		// 列全体が希望どおりの重ね順になる。
		//
		// 希望順に出てこないレイヤ（ユーザーが別途作ったもの・まだ生成されていないもの）は
		// 変更しない。GetNamedLayer が nil を返すレイヤは通知せずにスキップする（要素の描画が
		// スキップされてレイヤが無い場合など）。
		std::size_t moved = 0;
		MCObjectHandle previous = nil;
		for (const std::string& name : std::ranges::reverse_view(desired))
		{
			const MCObjectHandle layer = gSDK->GetNamedLayer(TXString(name.c_str()));
			if (layer == nil)
				continue;
			if (previous != nil && gSDK->InsertObjectAfter(layer, previous))
				++moved;
			previous = layer;
		}
		return moved;
	}

	std::size_t drawStories(const core::Document& document, core::ProgressReporter& progress,
							std::string* outNote)
	{
		const std::vector<core::StoryCommand>& commands = document.stories;
		if (commands.empty())
			return 0;

		// 命令セットに登場するレベル種別を登場順に事前登録する（dedupe は draw/DrawUtil の
		// PushUnique）。
		std::vector<std::string> levelTypes;
		for (const core::StoryCommand& command : commands)
		{
			for (const core::LevelCommand& level : command.levels)
				PushUnique(levelTypes, level.type);
		}
		for (const std::string& levelType : levelTypes)
		{
			TXString levelTypeName(levelType.c_str()); // CreateLayerLevelType は非 const TXString&
			gSDK->CreateLayerLevelType(levelTypeName);
		}
		std::size_t fallbackLayers = 0;
		std::size_t unattachedLeftovers = 0;

		std::size_t count = 0;
		for (const core::StoryCommand& command : commands)
		{
			if (!AdvanceProgress(progress))
				break;

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

			// レベルを持たずに階だけに結ぶレイヤ（柱の span レイヤ。M44）を**レベル付きの
			// レイヤより先に**作る。仮の種別にはこの階の levels の種別を借り、結んだときに
			// できたレベルはあとでその種別のレイヤに使う（ファイル冒頭）。結べなかったときは
			// 従来のレベル付きのレイヤ（種別＝レイヤ名）で作り直す——柱が描画できることを
			// 優先し、診断に残す。
			const std::string borrowedType = command.storyLayers.empty()
												 ? std::string()
												 : BorrowableLevelType(story, command.levels);
			bool leftoverLevel = false;
			for (const std::string& layer : command.storyLayers)
			{
				if (CreateStoryOnlyLayer(story, layer, borrowedType))
				{
					leftoverLevel = leftoverLevel || !borrowedType.empty();
					continue;
				}
				TXString levelTypeName(layer.c_str());
				gSDK->CreateLayerLevelType(levelTypeName);
				CreateStoryLevelViaTemplate(story, layer, 0.0, layer);
				++fallbackLayers;
			}

			for (const core::LevelCommand& level : command.levels)
			{
				// 借りた種別のレベルは既に階にあるので、そこへ結ぶ。結べなければ
				// テンプレート経由で作る（レベルが 1 つ余るが、レイヤは用意できる）。
				if (leftoverLevel && level.type == borrowedType)
				{
					leftoverLevel = false;
					if (AttachLayerToLeftoverLevel(story, level.type, level.offset, level.layer))
						continue;
					++unattachedLeftovers;
				}
				CreateStoryLevelViaTemplate(story, level.type, level.offset, level.layer);
			}

			++count;
		}

		if (outNote != nullptr)
		{
			std::string text;
			AppendCount(text,
						"ストーリの診断: レベルを持たずに階へ結べず、レベル付きで作ったレイヤ",
						fallbackLayers, "枚");
			AppendCount(text, "ストーリの診断: 柱のレイヤに貸したレベルへ結べず、別に作ったレイヤ",
						unattachedLeftovers, "枚");
			*outNote = std::move(text);
		}
		return count;
	}
} // namespace HomeskzIfcImport::draw
