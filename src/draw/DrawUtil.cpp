//
//	draw/DrawUtil.cpp
//
//	draw/ 共通ヘルパーの実装。呼ぶ SDK API はいずれも従来 各 draw/*.cpp が個別に
//	持っていたものと同一で、集約しただけ（振る舞いは変えない）。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには
//	入れない（CLAUDE.md「依存の向きは厳守する」）。
//

#include "PluginPrefix.h"
#include "draw/DrawUtil.h"
#include "draw/ObjectHandles.h"

#include "VWFC/VWObjects/VWDocument.h"
#include "VWFC/VWObjects/VWGroupObj.h"
#include "VWFC/VWObjects/VWLayerObj.h"
#include "VWFC/VWObjects/VWPolygon2DObj.h"
#include "VWFC/VWObjects/VWViewportObj.h"

#include <algorithm>
#include <array>
#include <memory>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// デザインレイヤの種別コード（CreateLayer の layerType 引数）。1 = デザインレイヤ、
		// 2 = シート（プレゼンテーション）レイヤ。SDK の ELayerType（Kernel/API/
		// MiniCadCallBacks.h）がこの値を定める。
		constexpr short kDesignLayerType = 1;
		constexpr short kSheetLayerType = 2;

		// SetViewportLayerVisibility の表示種別。**2（グレー）は使わない**——対象外のレイヤを
		// グレーにすると図に薄く残る（Python 版 vw/sheet.py の注記と同じ）。
		constexpr short kLayerVisible = 0;
		constexpr short kLayerHidden = 1;

		// SetViewportClassVisibility の表示種別。SDK の EClassVisibility（VWFC/VWObjects/
		// VWClass.h）が Normal=0 / Invisible=-1 / Grayed=2 と定めており、**VS の 0/1/2 とは
		// 値が違う**（1 は「非表示」ではない）。表示に戻すのが目的なので Normal だけを使う。
		constexpr short kClassVisible = 0;

		// 図面の**デザインレイヤ**を先頭から順に辿る。VWDocument::GetDrawingHeaderFristMember
		// （SDK の綴りママ）が図面のオブジェクト列の先頭＝最初のレイヤで、以降は NextObject で
		// たどれる。レイヤ以外が混ざっても IsLayerObject で弾く（ISDK に「レイヤだけを列挙する」
		// 呼び出しは無いため、この走査が唯一の手立て）。
		//
		// **シートレイヤは除く**（VWLayerObj::GetLayerType が kLayerSheet=2 を返すもの）。
		// ビューポートに映るのはデザインレイヤだけなので、シートレイヤを混ぜても
		// SetViewportLayerVisibility は意味を持たず、CollectUsedClasses が既にできている
		// ビューポートの中身まで辿る無駄が増える。**軸組図は伏図の後に走る**ので、除かないと
		// 伏図の作ったシートレイヤ（"1" / "2" …）がそのまま入ってくる。
		std::vector<MCObjectHandle> AllLayers()
		{
			std::vector<MCObjectHandle> layers;
			try
			{
				for (MCObjectHandle h = VWDocument::GetDrawingHeaderFristMember(); h != nil;
					 h = gSDK->NextObject(h))
				{
					if (!VWLayerObj::IsLayerObject(h))
						continue;
					if (VWLayerObj(h).GetLayerType() == kLayerSheet)
						continue;
					layers.push_back(h);
				}
			}
			catch (...)
			{
				// 走査中の異常で図全体を落とさない（CLAUDE.md「エラーハンドリング」）。
				// そこまでに拾えたレイヤだけを返す（絞り込みの取りこぼしは、そのレイヤが
				// 図に映り込むだけで済む）。
				return layers;
			}
			return layers;
		}

		// 図面で実際に使われているクラスの索引を集める（DrawUtil.h の ViewportSetup 参照）。
		// **コンテナは中まで辿る**のが要点で、通り芯（GridAxis PIO）のラベルのように
		// 「PIO / シンボルの中の図形が、スタイルの決めたクラスを持つ」ものは、外側の
		// オブジェクトのクラスだけ見ても拾えない（ラベルだけ消える。ローカル確認で判明）。
		//
		// 走査中は重複除去のために std::set を使い、**返すのは昇順・重複なしの vector**
		// （ViewportSetup が std::set を持てない理由は DrawUtil.h 参照）。
		// 図形の連なり（と、その入れ子）が身に付けているクラスを集める。heads は走査を
		// 始める先頭オブジェクトの列。
		std::set<InternalIndex> CollectClassesFrom(const std::vector<MCObjectHandle>& heads)
		{
			std::set<InternalIndex> classes;
			// 入れ子（グループ・シンボル・PIO）は深さ上限つきで辿る。上限は「PIO の中の
			// グループの中の図形」に十分で、壊れたデータで無限に潜らない値。
			constexpr int kMaxDepth = 6;
			std::vector<std::pair<MCObjectHandle, int>> pending;
			pending.reserve(heads.size());
			for (const MCObjectHandle head : heads)
			{
				if (head != nil)
					pending.emplace_back(head, 0);
			}
			while (!pending.empty())
			{
				const auto [head, depth] = pending.back();
				pending.pop_back();
				for (MCObjectHandle h = head; h != nil; h = gSDK->NextObject(h))
				{
					const InternalIndex index = gSDK->GetObjectClass(h);
					if (index != 0)
						classes.insert(index);
					if (depth >= kMaxDepth)
						continue;
					const MCObjectHandle child = gSDK->FirstMemberObj(h);
					if (child != nil)
						pending.emplace_back(child, depth + 1);
				}
			}
			return classes;
		}

		std::vector<InternalIndex> CollectUsedClasses(const std::vector<MCObjectHandle>& layers)
		{
			std::vector<MCObjectHandle> heads;
			heads.reserve(layers.size());
			for (const MCObjectHandle layer : layers)
				heads.push_back(gSDK->FirstMemberObj(layer));
			const std::set<InternalIndex> classes = CollectClassesFrom(heads);
			return {classes.begin(), classes.end()};
		}

		// 表示するデザインレイヤの縮尺を返す（Python 版 configure_viewport_scale）。図が映す
		// レイヤの縮尺は揃っているので、最初に取れたものを採る。取れなければ 0（＝ビューポートの
		// 既定縮尺のままにする）。
		double LayerScaleFor(const core::ViewportCommand& command)
		{
			for (const std::string& name : command.layers)
			{
				const MCObjectHandle layer = gSDK->GetNamedLayer(TXString(name.c_str()));
				if (layer == nil)
					continue;
				try
				{
					const VWLayerObj design(layer);
					const double scale = design.GetScale();
					if (scale > 0.0)
						return scale;
				}
				catch (...)
				{
					// このレイヤからは縮尺を取れなかった。次の候補を見る。
					continue;
				}
			}
			return 0.0;
		}

		// 既存のコンポーネント（層）数。取得できなければ 0（＝層を持たない）とみなす。
		short CountComponents(MCObjectHandle object)
		{
			short count = 0;
			if (!gSDK->GetNumberOfComponents(object, count))
				return 0;
			return count;
		}

		// baseName から**まだ使われていない**名前付きリソース名を作って out に入れる。
		// 埋まっていれば " (2)" … と連番を付ける。既存のリソースには触れないので、そこで
		// 設定済みのクラス・マテリアル・用途が失われることが構造的に起きない。連番の上限は
		// 「同名が延々と埋まっている」異常時に無限ループしないための歯止めで、実運用で届く
		// 数ではない。baseName が空・空きが見つからないときは false。
		bool FindFreeResourceName(const std::string& baseName, std::string& out)
		{
			if (baseName.empty())
				return false;

			constexpr int kMaxAttempts = 1000;
			std::string name = baseName;
			for (int attempt = 2; gSDK->GetNamedObject(TXString(name.c_str())) != nil; ++attempt)
			{
				if (attempt > kMaxAttempts)
					return false;
				name = baseName + " (" + std::to_string(attempt) + ")";
			}
			out = name;
			return true;
		}
	} // namespace

	void SetClassByName(MCObjectHandle object, const std::string& className)
	{
		if (className.empty())
			return;
		const InternalIndex classID = gSDK->AddClass(TXString(className.c_str()));
		gSDK->SetObjectClass(object, classID);
	}

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

	MCObjectHandle CreateClosedPolygon(const std::vector<core::Vec2>& boundary)
	{
		if (boundary.empty())
			return nil;

		std::vector<VWPoint2D> vertices;
		vertices.reserve(boundary.size());
		for (const core::Vec2& point : boundary)
			vertices.emplace_back(point.x, point.y);

		VWPolygon2DObj polygon(vertices);
		polygon.SetClosed(true); // スラブのプロファイルは閉じた外形
		return polygon.GetThisObject();
	}

	void SetComponents(MCObjectHandle object, const std::vector<core::ComponentCommand>& components)
	{
		const short original = CountComponents(object);
		const auto wanted = static_cast<short>(components.size());

		// 1. 命令の層を先頭から順に挿入する（索引 i の層の「手前」に入るので、0,1,… の順に
		//    入れると命令どおりの並びが先頭にできる）。fill / ペン太さ / 線種は文書の既定に
		//    任せ（0）、描画属性はクラスに従わせる。
		for (short index = 0; index < wanted; ++index)
		{
			const core::ComponentCommand& component = components[static_cast<std::size_t>(index)];
			gSDK->InsertNewComponentN(object, index, component.thickness, 0, 0, 0, 0, 0);
			gSDK->SetComponentWidth(object, index, component.thickness);
			gSDK->SetComponentName(object, index, TXString(component.name.c_str()));
		}

		// 2. 挿入した層の直後に並んでいる元の層を、前から順に削除する（索引 wanted は常に
		//    「元の層の先頭」を指すので、同じ索引を元の層数だけ削除すればよい）。
		for (short removed = 0; removed < original; ++removed)
		{
			if (!gSDK->DeleteComponent(object, wanted))
				break;
		}
	}

	void SetSlabDatum(MCObjectHandle object, core::SlabDatum datum, short componentCount)
	{
		if (componentCount <= 0)
			return;
		// ローカル名は SDK 側の引数名（componentIndex / datumIsTopOfComponent）に寄せてある。
		// 似た並びの short + bool を渡すため、名前が違うと clang-tidy の
		// readability-suspicious-call-argument が「引数が入れ替わっているのでは」と誤検知する。
		const bool datumIsTop = (datum == core::SlabDatum::Top);
		// 三項演算子の共通型は int になるので、short への縮小は 1 か所でまとめて行う
		// （型はキャスト側に書いてあるので auto。modernize-use-auto）。
		const auto componentIndex = static_cast<short>(datumIsTop ? 0 : componentCount - 1);
		gSDK->SetDatumSlabComponent(object, componentIndex);
		// 構成要素を指すだけでは既定の面（中心／下端）のままなので、面も明示する。
		gSDK->SetComponentDatumIsTopOfComponent(object, componentIndex, datumIsTop);
	}

	InternalIndex ResolveSlabStyle(const std::string& styleName,
								   const std::vector<core::ComponentCommand>& components,
								   core::SlabDatum datum)
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
		// 基準面（構成要素とその上端／下端）はスタイルが持つので、スタイル側へ設定する。
		SetSlabDatum(style, datum, static_cast<short>(components.size()));
		return gSDK->GetObjectInternalIndex(style);
	}

	InternalIndex CreateUniqueSlabStyle(const std::string& baseName,
										const std::vector<core::ComponentCommand>& components,
										core::SlabDatum datum, std::string* outName)
	{
		std::string name;
		if (!FindFreeResourceName(baseName, name))
			return 0;

		MCObjectHandle style = gSDK->CreateSlabStyle(TXString(name.c_str()));
		if (style == nil)
			return 0;

		SetComponents(style, components);
		// 基準面（構成要素とその上端／下端）はスタイルが持つので、スタイル側へ設定する。
		SetSlabDatum(style, datum, static_cast<short>(components.size()));
		if (outName != nullptr)
			*outName = name;
		return gSDK->GetObjectInternalIndex(style);
	}

	InternalIndex CreateUniqueWallStyle(const std::string& baseName,
										const std::vector<core::ComponentCommand>& components,
										std::string* outName)
	{
		std::string name;
		if (!FindFreeResourceName(baseName, name))
			return 0;

		MCObjectHandle style = gSDK->CreateWallStyle(TXString(name.c_str()));
		if (style == nil)
			return 0;

		// 壁は構成層の合計がそのまま壁厚になるので、スラブと違って基準面の設定は要らない。
		SetComponents(style, components);

		// **コア構成要素**を指定する（VW が結合部で構成要素を融合する基準になる。指定が無いと
		// 壁結合しても平面で層が繋がらず、取り合いに面線が残る＝ローカル確認で判明した T 字の
		// 線。ROADMAP.md M10）。基礎の立上りは構成が 1 層（コンクリート）なので、その 1 枚が
		// コアになる。索引は SetComponents と同じ **0 始まり**（draw/DrawUtil.h 参照）。
		if (!components.empty())
			gSDK->SetCoreWallComponent(style, 0);

		if (outName != nullptr)
			*outName = name;
		return gSDK->GetObjectInternalIndex(style);
	}

	TXString ResolveParamName(const VWParametricObj& pio, const char* universalName,
							  const char* localizedName)
	{
		// const にしない: 戻り値として返すので、const だと move されず余計なコピーになる
		// （clang-tidy performance-no-automatic-move）。
		TXString universal(universalName);
		if (pio.GetParamIndex(universal) != static_cast<size_t>(-1))
			return universal;

		const TXString localized(localizedName);
		const size_t count = pio.GetParamsCount();
		for (size_t i = 0; i < count; ++i)
		{
			if (pio.GetParamLocalizedName(i) == localized)
				return pio.GetParamName(i);
		}
		return universal;
	}

	bool SetParamRealChecked(VWParametricObj& pio, const TXString& param, double value,
							 double tolerance)
	{
		pio.SetParamReal(param, value);
		if (std::abs(pio.GetParamReal(param) - value) <= tolerance)
			return true;

		// 実数で入らなかった＝そのパラメータは文字列で保持されている。文字列で入れ直す
		// （"%g" で余分な 0 を落とす。寸法は mm の実数）。
		std::array<char, 32> buffer{};
		std::snprintf(buffer.data(), buffer.size(), "%g", value);
		pio.SetParamAsString(param, TXString(buffer.data()));
		return std::abs(pio.GetParamReal(param) - value) <= tolerance;
	}

	MCObjectHandle CreateRectangleProfileGroup(double minX, double minY, double maxX, double maxY)
	{
		if (maxX - minX <= 0.0 || maxY - minY <= 0.0)
			return nil;

		VWPolygon2DObj profile({VWPoint2D(minX, minY), VWPoint2D(minX, maxY), VWPoint2D(maxX, maxY),
								VWPoint2D(maxX, minY)});
		profile.SetClosed(true);
		const MCObjectHandle profileHandle = profile.GetThisObject();
		if (profileHandle == nil)
			return nil;

		VWGroupObj group;
		group.AddObject(profileHandle);
		const MCObjectHandle groupHandle = group.GetThisObject();
		if (groupHandle == nil)
			return nil;
		// 断面が本当に入ったか（空のグループを渡さない。ヘッダ参照）。
		if (VWGroupObj(groupHandle).GetFirstMemberObject() == nil)
			return nil;
		return groupHandle;
	}

	RefNumber ResolvePluginStyle(const TXString& styleName)
	{
		MCObjectHandle style = gSDK->GetNamedObject(styleName);
		if (style == nil || !gSDK->IsPluginStyle(style))
			return 0;
		return static_cast<RefNumber>(gSDK->GetObjectInternalIndex(style));
	}

	// --- 取り込み全体の Undo（DrawUtil.h「なぜレイヤを記録するのか」）--------------------
	namespace
	{
		// いま開いている undo スコープ（無ければ nullptr）。インポートはメニューコマンドから
		// 1 本しか走らないので、高々 1 つで足りる。要素側が引数で持ち回らずに済むように、
		// 記録の入口（RecordCreatedLayer / NoteExistingLayerUsed）はここを見る。
		ImportUndoScope* gActiveUndoScope = nullptr;
	} // namespace

	ImportUndoScope::ImportUndoScope()
	{
		// kUndoSwapObjects は「入れ替えるオブジェクトを AddBeforeSwapObject /
		// AddAfterSwapObject で指定する」方式（Kernel/API/APIBase.Legacy.Defs.h）。
		// **これがイベントの開始も兼ねる**（ISDK に開始専用の呼び出しは無い）。
		gSDK->SetUndoMethod(kUndoSwapObjects);
		gSDK->NameUndoEvent(TXString("ホームズ君 IFC 取り込み"));
		gActiveUndoScope = this;
	}

	ImportUndoScope::~ImportUndoScope()
	{
		gActiveUndoScope = nullptr;
		if (fCreatedLayers.empty())
		{
			// 何も登録できていない＝「取り消し」しても取り込みは戻らない。それでも
			// イベントを残すと、SDK 内部が途中で開いた記録（断面ビューポート等）が
			// 取り消しの対象になり、**図面が壊れる**（実機で確認。ROADMAP.md M15）。
			// テーブルから取り除いておく——取り込み前のユーザー自身の履歴は残る。
			gSDK->EndAndRemoveUndoEvent();
			return;
		}
		gSDK->EndUndoEvent();
	}

	bool ImportUndoScope::contains(MCObjectHandle layer) const
	{
		return std::find(fCreatedLayers.begin(), fCreatedLayers.end(), layer) !=
			   fCreatedLayers.end();
	}

	void RecordCreatedLayer(MCObjectHandle layer)
	{
		ImportUndoScope* scope = gActiveUndoScope;
		if (scope == nullptr || layer == nil || scope->contains(layer))
			return;
		// 「あとで消してよいもの」として undo テーブルへ登録する。レイヤを消せば、その上の
		// 図形（構造材・壁・スラブ・シンボル・ビューポート）もまとめて消える。
		gSDK->AddAfterSwapObject(layer);
		scope->fCreatedLayers.push_back(layer);
	}

	void NoteExistingLayerUsed(MCObjectHandle layer)
	{
		ImportUndoScope* scope = gActiveUndoScope;
		if (scope == nullptr || layer == nil || scope->contains(layer))
			return;
		scope->fUsedExistingLayer = true;
	}

	MCObjectHandle PrepareLayer(const std::string& layerName)
	{
		const TXString name(layerName.c_str());
		MCObjectHandle layer = gSDK->GetNamedLayer(name);
		if (layer == nil)
		{
			layer = gSDK->CreateLayer(name, kDesignLayerType);
			RecordCreatedLayer(layer); // 取り消しで消してよい（このインポートが作った）
		}
		else
		{
			NoteExistingLayerUsed(layer); // 取り込み前から在った＝取り消しでは戻らない
		}
		if (layer != nil)
			gSDK->SetCurrentLayer(layer);
		return layer;
	}

	MCObjectHandle ActivateExistingLayer(const std::string& layerName)
	{
		MCObjectHandle layer = gSDK->GetNamedLayer(TXString(layerName.c_str()));
		if (layer == nil)
			return nil;
		// ストーリ由来のレイヤは drawStories が作った（＝登録済み）なので何も起きない。
		// 取り込み前から在ったものだけが「戻らない」印になる。
		NoteExistingLayerUsed(layer);
		gSDK->SetCurrentLayer(layer);
		return layer;
	}

	// --- シートレイヤとビューポート（伏図＝M13・軸組図＝M14 が共有する）------------------
	//
	// 使う SDK API は ISDK（gSDK）／VWFC の実在シグネチャに合わせている
	// （Vectorworks 2026 SDK。ci-debug の sdk-grep / shell で確認済み）:
	//   * VWDocument::GetDrawingHeaderFristMember()      … 図面のオブジェクト列の先頭（＝最初の
	//                                                      レイヤ。SDK の綴りママ）
	//   * gSDK->NextObject(h) / VWLayerObj::IsLayerObject … レイヤの走査
	//   * gSDK->GetNamedLayer / CreateLayer               … シートレイヤの取得・生成
	//   * VWLayerObj(h).SetDescription / GetScale         … シートレイヤのタイトル・縮尺
	//   * gSDK->SetViewportLayerVisibility(vp, layer, v)  … 表示レイヤの絞り込み（0=表示/1=非表示）
	//   * gSDK->FirstMemberObj / GetObjectClass           … 図形が使うクラスの数え上げ
	//   * gSDK->ClassNameToID(name)                       … クラス名 → InternalIndex
	//   * gSDK->SetViewportClassVisibility(vp, idx, 0)    … クラス表示（既定は非表示）
	//   * VWViewportObj(vp).SetScale / SetDescription / SetLocator / Update
	//                                                     … 縮尺（1003）・図面タイトル（1032）・
	//                                                       図番（1033）・描画更新
	ViewportSetup PrepareViewportSetup(const core::Document& document)
	{
		ViewportSetup setup;
		setup.layers = AllLayers();
		setup.classes = CollectUsedClasses(setup.layers);
		// 命令セットが名乗るクラスも足す（まだ図形が無いクラスや、走査で辿れなかったものの
		// 取りこぼしを防ぐ保険）。足した後に昇順・重複なしへ整える。
		for (const std::string& name : core::documentClassNames(document))
		{
			const InternalIndex index = gSDK->ClassNameToID(TXString(name.c_str()));
			if (index != 0)
				setup.classes.push_back(index);
		}
		std::ranges::sort(setup.classes);
		const auto duplicates = std::ranges::unique(setup.classes);
		setup.classes.erase(duplicates.begin(), duplicates.end());
		return setup;
	}

	MCObjectHandle PrepareSheetLayer(const std::string& number, const std::string& title)
	{
		const TXString name(number.c_str());
		MCObjectHandle layer = gSDK->GetNamedLayer(name);
		if (layer == nil)
		{
			layer = gSDK->CreateLayer(name, kSheetLayerType);
			RecordCreatedLayer(layer); // 取り消しで消してよい（ビューポートごと消える）
		}
		else
		{
			NoteExistingLayerUsed(layer);
		}
		if (layer == nil)
			return nil;
		try
		{
			VWLayerObj sheet(layer);
			sheet.SetDescription(TXString(title.c_str()));
		}
		catch (...)
		{
			// タイトルが付かなくても図は描ける（1 つの失敗で全体を止めない）ので、
			// レイヤはそのまま返す。
			return layer;
		}
		return layer;
	}

	std::vector<InternalIndex> CollectObjectClasses(MCObjectHandle object)
	{
		const std::set<InternalIndex> classes = CollectClassesFrom({object});
		return {classes.begin(), classes.end()};
	}

	std::size_t ShowViewportClasses(MCObjectHandle viewport,
									const std::vector<InternalIndex>& classes)
	{
		std::size_t applied = 0;
		for (const InternalIndex index : classes)
		{
			if (gSDK->SetViewportClassVisibility(viewport, index, kClassVisible))
				++applied;
		}
		return applied;
	}

	std::size_t ConfigureViewport(MCObjectHandle viewport, MCObjectHandle sheetLayer,
								  const ViewportSetup& setup, const core::ViewportCommand& command)
	{
		// 表示レイヤ: まず全部隠し、命令に挙げたものだけ表示へ戻す。**存在しないレイヤ名は
		// 黙って読み飛ばす**（要素の描画がスキップされてレイヤが無い場合など。図自体は残す）。
		for (const MCObjectHandle layer : setup.layers)
		{
			if (layer == sheetLayer)
				continue;
			gSDK->SetViewportLayerVisibility(viewport, layer, kLayerHidden);
		}
		for (const std::string& name : command.layers)
		{
			const MCObjectHandle layer = gSDK->GetNamedLayer(TXString(name.c_str()));
			if (layer != nil)
				gSDK->SetViewportLayerVisibility(viewport, layer, kLayerVisible);
		}

		// クラス: 1 つずつ表示へ戻す（ヘッダ「クラスをわざわざ数え上げる理由」）。
		const std::size_t applied = ShowViewportClasses(viewport, setup.classes);

		// 縮尺・ラベル・更新。**縮尺は表示レイヤを絞った後に読む**（映すレイヤの縮尺に
		// 合わせるため）。設定に失敗しても図そのものは残す。
		const double scale = LayerScaleFor(command);
		try
		{
			VWViewportObj vp(viewport);
			if (scale > 0.0)
				vp.SetScale(scale);
			vp.SetDescription(TXString(command.drawingTitle.c_str()));
			vp.SetLocator(TXString(command.drawingNumber.c_str()));
			vp.Update();
		}
		catch (...)
		{
			// ラベル・縮尺が付かなくてもビューポートは図面に残るので、ここで戻る。
			return applied;
		}
		return applied;
	}

	// 「命令インデックス → ハンドル」の対応表の所有者。**表の中身（MCObjectHandle）は
	// SDK 型なので、宣言（draw/ObjectHandles.h）と定義（draw/DrawUtil.h）が分かれている**。
	// 不完全型の unique_ptr を持つので、構築と破棄は定義の見えるこの翻訳単位に置く。
	ObjectHandles::ObjectHandles() : fTable(std::make_unique<ObjectHandleTable>()) {}

	ObjectHandles::~ObjectHandles() = default;
} // namespace HomeskzIfcImport::draw
