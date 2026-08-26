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

#include "VWFC/VWObjects/VWClass.h"
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
		// グレーにすると図に薄く残る。
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
		// SetViewportLayerVisibility は意味を持たない。**軸組図は伏図の後に走る**ので、
		// 除かないと伏図の作ったシートレイヤ（"1" / "2" …）がそのまま入ってくる。
		//
		// 実体は公開関数 DesignLayersInStackOrder（この走査は図面に 1 か所だけ）。
		std::vector<MCObjectHandle> AllLayers()
		{
			return DesignLayersInStackOrder();
		}

		// 図面のクラスをすべて集める（昇順・重複なしの vector で返す）。
		//
		// **ビューポートはクラスの表示を明示しないと非表示のまま**なので（M13 のローカル確認
		// で判明。レイヤは命令どおりなのに図形が 1 つも出なかった）、映したいクラスを
		// 1 つずつ表示へ戻す必要がある。ここでは**ドキュメントの全クラスを表示**にする——
		// ビューポートごとに映る/映らないをクラスで絞る要件は無く、絞らないなら「どのクラスが
		// 要るか」を推し量る必要も無い。
		//
		// 列挙は VWClass::ForEachClass（ISDK::ForEachClass の VWFC 版）。**受け取った VWClass は
		// そのまま InternalIndex へ変換できる**（VWClass::operator InternalIndex）。
		// doGuestClasses=true は参照ファイル由来（ゲスト）のクラスも含める指定で、
		// 「全クラス表示」の趣旨どおり含める（ビューポートへ設定できないものは
		// SetViewportClassVisibility が false を返すだけで無害）。
		std::vector<InternalIndex> AllClasses()
		{
			std::set<InternalIndex> classes;
			try
			{
				VWClass::ForEachClass(true,
									  [&classes](const VWClass& clas)
									  {
										  const InternalIndex index = clas;
										  if (index != 0)
											  classes.insert(index);
									  });
			}
			catch (...)
			{
				// 列挙中の異常で図全体を落とさない（CLAUDE.md「エラーハンドリング」）。
				// そこまでに拾えたクラスだけを返す（取りこぼしたクラスは、そのクラスの
				// 図形がビューポートに映らないだけで済む）。**catch の中で return する**のは
				// AllLayers と同じ形で、clang-tidy の bugprone-empty-catch（コメントだけの
				// catch は握り潰しとみなす）を避けるためでもある。
				return {classes.begin(), classes.end()};
			}
			return {classes.begin(), classes.end()};
		}

		// ビューポートで指定のクラスを表示へ戻す（戻せた数を返す）。**表示種別の値を
		// ここ 1 か所に閉じ込める**ためのもの。
		std::size_t ShowClasses(MCObjectHandle viewport, const std::vector<InternalIndex>& classes)
		{
			std::size_t applied = 0;
			for (const InternalIndex index : classes)
			{
				if (gSDK->SetViewportClassVisibility(viewport, index, kClassVisible))
					++applied;
			}
			return applied;
		}

		// 表示するデザインレイヤの縮尺を返す。図が映すレイヤの縮尺は揃っているので、
		// 最初に取れたものを採る。取れなければ 0（＝ビューポートの既定縮尺のままにする）。
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

		// 構成要素（層）1 枚のクラスを名前で設定する。オブジェクト本体の SetClassByName と
		// 同じ作法（AddClass は既存なら索引を返し、無ければ作る）で、クラス名が空なら
		// 何もしない（層は無クラス＝既定クラスのまま）。
		void SetComponentClassByName(MCObjectHandle object, short componentIndex,
									 const std::string& className)
		{
			if (className.empty())
				return;
			const InternalIndex classID = gSDK->AddClass(TXString(className.c_str()));
			gSDK->SetComponentClass(object, componentIndex, classID);
		}

		// 構成要素（層）1 枚の描画属性をすべてクラス属性に従わせる。層が持つ属性は
		// **塗り**と**左右のペン**の 2 系統だけで、クラスを割り当てただけでは
		// by-instance の既定値（挿入時に渡した 0）のまま残るため、オブジェクト本体の
		// SetAllAttributesByClass と同じく明示的に by-class を指定する。
		void SetComponentAttributesByClass(MCObjectHandle object, short componentIndex)
		{
			gSDK->SetComponentUseFillClassAttr(object, componentIndex, true);
			// 左右のペン（層の境界線）はどちらもクラス属性に従わせる。
			gSDK->SetComponentUsePenClassAttr(object, componentIndex, true, true);
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
		//    入れると命令どおりの並びが先頭にできる）。InsertNewComponentN へ渡す
		//    fill / ペン太さ / 線種は文書の既定に任せ（0）、描画属性は下の
		//    SetComponentAttributesByClass で層のクラスに従わせる。
		for (short index = 0; index < wanted; ++index)
		{
			const core::ComponentCommand& component = components[static_cast<std::size_t>(index)];
			gSDK->InsertNewComponentN(object, index, component.thickness, 0, 0, 0, 0, 0);
			gSDK->SetComponentWidth(object, index, component.thickness);
			gSDK->SetComponentName(object, index, TXString(component.name.c_str()));
			SetComponentClassByName(object, index, component.drawClass);
			SetComponentAttributesByClass(object, index);
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

	void SetWallComponents(MCObjectHandle wall,
						   const std::vector<core::ComponentCommand>& components)
	{
		if (components.empty())
			return;

		SetComponents(wall, components);

		// **コア構成要素**を指定する（draw/DrawUtil.h 参照）。立上りは 1 層なので索引 0。
		gSDK->SetCoreWallComponent(wall, 0);
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
			// 取り消しの対象になり、**図面が壊れる**（実機で確認。docs/DEV-NOTES.md M15）。
			// テーブルから取り除いておく——取り込み前のユーザー自身の履歴は残る。
			gSDK->EndAndRemoveUndoEvent();
			return;
		}
		gSDK->EndUndoEvent();
	}

	bool ImportUndoScope::contains(MCObjectHandle layer) const
	{
		return std::ranges::find(fCreatedLayers, layer) != fCreatedLayers.end();
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

	void RecordCreatedObject(MCObjectHandle object)
	{
		if (gActiveUndoScope == nullptr || object == nil)
			return;
		// レイヤと違い一覧には控えない（数えるのは「取り消しで戻せるか」の判断材料であって、
		// 下ごしらえのオブジェクトはその判断に関係しないため）。
		gSDK->AddAfterSwapObject(object);
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

	std::vector<MCObjectHandle> DesignLayersInStackOrder()
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

	namespace
	{
		// ビューポートの向き（オブジェクト変数 1007）。3D の「上」＝standardViewTop（7）。
		// 2D/平面かどうかは向きではなく Project 2D（1005）が持つので、**この 2 つは組で
		// 扱う**（DrawUtil.h の ViewportProjection）。
		constexpr TStandardView kViewTop = standardViewTop;

		// ビューポートを 1 つ描き直させる（＝オブジェクト情報パレットの「更新」ボタン相当）。
		// 走ったら true。
		//
		// **out-of-date を立ててから更新する。** `Update` 系は「out-of-date なら描き直す」で
		// あって「無条件に描き直す」ではなく、**投影（Project 2D）の切り替え**のように VW が
		// out-of-date を立ててくれない変更のあとでは黙って何もしない。`IsDirty` / `SetDirty`
		// がそのフラグそのものなので、立ててから更新し、**下りたかを読み戻して**返す。
		//
		// 更新の口は **`ISDK::UpdateViewport`**（OIP の「更新」ボタンに当たる ISDK の呼び出し。
		// SDK のビューポート API を全部並べても「描き直させる」口はこれ 1 つ）。VWFC の
		// `VWViewportObj::Update()` とは別経路なので、そちらで効かなかった件（重ね順）を
		// こちらで確かめられる。
		bool RefreshOne(MCObjectHandle viewport)
		{
			try
			{
				VWViewportObj vp(viewport);
				vp.SetDirty(true);
				gSDK->UpdateViewport(viewport);
				return !vp.IsDirty();
			}
			catch (...)
			{
				// 描き直せなくても図面は残る（ユーザーが「更新」を押せば直る）。
				return false;
			}
		}

		// ビューポートを 2D/平面（Top/Plan）で**正しく描かせる**。作り直せたら true。
		//
		// 【なぜ「OFF → 更新 → ON」なのか】ただ Project 2D を ON にするだけでは足りない——生成
		// 直後のビューポートはパレット上こそ「2D/平面」だが、描画キャッシュは 3D の「上」
		// ビューのままで、**更新ボタンを押しても作り直されない**（実機の症状）。
		// 手動での唯一の対処が「いったん『上』を選んでから『2D/平面』へ戻す」ことなので、
		// その操作をそのままなぞる: 向きを「上」にし、Project 2D を OFF にして**更新を挟み**
		// （＝ 3D の「上」でキャッシュを作り直させ）、その上で ON へ戻す。最後の更新は呼び出し
		// 元（ConfigureViewport の末尾）が行い、そこで 2D/平面のキャッシュができる。
		//
		// **途中の更新も必ず RefreshOne で行う**——投影の切り替えでは out-of-date が立たない
		// ので、素の更新では OFF 側のキャッシュが作り直されず、この「なぞり」が空振りする。
		// 走ったかどうかはここでは問わない（作り直しの途中経過。図を仕上げるのは取り込みの
		// 最後の RefreshViewports）。
		//
		// **入ったかどうかは読み戻して確かめる**——SDK の setter は書けなかったときも黙って
		// 何もしないので、戻り値の無いまま「設定したつもり」で終わらせない。
		bool ForcePlanView(VWViewportObj& viewport, MCObjectHandle handle)
		{
			viewport.SetViewType(kViewTop);
			viewport.SetProject2D(false);
			static_cast<void>(RefreshOne(handle));
			viewport.SetProject2D(true);
			return viewport.GetProject2D();
		}
	} // namespace

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
	//   * VWClass::ForEachClass(true, cb)                 … 図面の全クラスの列挙
	//                                                       （ISDK::ForEachClass の VWFC 版）
	//   * gSDK->SetViewportClassVisibility(vp, idx, 0)    … クラス表示（既定は非表示）
	//   * VWViewportObj(vp).SetScale / SetDescription / SetLocator / Update
	//                                                     … 縮尺（1003）・図面タイトル（1032）・
	//                                                       図番（1033）・描画更新
	//   * VWViewportObj(vp).SetViewType / SetProject2D / GetProject2D
	//                                                     … ビューの向き（1007）・2D/平面か
	//                                                       （1005）。伏図の作り直しに使う
	//                                                       （DrawUtil.h の ViewportProjection）
	//   * VWViewportObj(vp).SetDirty(true) / IsDirty()    … out-of-date（更新が要る）の設定と
	//                                                       読み戻し。**更新の直前に必ず立てる**
	//                                                       （更新は out-of-date でなければ
	//                                                       何もしない）
	//   * gSDK->UpdateViewport(vp)                        … 描き直させる（OIP の「更新」相当）。
	//                                                       SDK のビューポート API で「描き直す」
	//                                                       口はこれ 1 つ
	ViewportSetup PrepareViewportSetup()
	{
		ViewportSetup setup;
		setup.layers = AllLayers();
		setup.classes = AllClasses();
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

	std::size_t ShowAllViewportClasses(MCObjectHandle viewport)
	{
		return ShowClasses(viewport, AllClasses());
	}

	ViewportFinish ConfigureViewport(MCObjectHandle viewport, MCObjectHandle sheetLayer,
									 const ViewportSetup& setup,
									 const core::ViewportCommand& command,
									 ViewportProjection projection)
	{
		ViewportFinish finish;
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

		// クラス: 全クラスを 1 つずつ表示へ戻す（ヘッダ「クラスを表示へ戻す理由」）。
		finish.classesApplied = ShowClasses(viewport, setup.classes);
		finish.planViewApplied = projection == ViewportProjection::Keep;

		// 縮尺・［投影の作り直し］・ラベル・更新。**縮尺は表示レイヤを絞った後に読む**
		// （映すレイヤの縮尺に合わせるため）。設定に失敗しても図そのものは残す。
		const double scale = LayerScaleFor(command);
		try
		{
			VWViewportObj vp(viewport);
			if (scale > 0.0)
				vp.SetScale(scale);
			if (projection == ViewportProjection::Plan)
				finish.planViewApplied = ForcePlanView(vp, viewport);
			vp.SetDescription(TXString(command.drawingTitle.c_str()));
			vp.SetLocator(TXString(command.drawingNumber.c_str()));
			// **ここでは描き直さない。** out-of-date のまま残し、取り込みの最後（undo イベントを
			// 閉じた後）に RefreshViewports がまとめて描き直す——取り込みの途中で描き直すと
			// レイヤの重ね順の並べ替えが届かないため（DrawUtil.h の RefreshViewports）。
			vp.SetDirty(true);
		}
		catch (...)
		{
			// ラベル・縮尺が付かなくてもビューポートは図面に残るので、ここで戻る。
			return finish;
		}
		return finish;
	}

	std::size_t RefreshViewports(const ObjectHandles& viewports)
	{
		std::size_t refreshed = 0;
		for (const auto& [index, viewport] : viewports.table().handles)
		{
			if (viewport == nil)
				continue;
			if (RefreshOne(viewport))
				++refreshed;
		}
		return refreshed;
	}

	// 「命令インデックス → ハンドル」の対応表の所有者。**表の中身（MCObjectHandle）は
	// SDK 型なので、宣言（draw/ObjectHandles.h）と定義（draw/DrawUtil.h）が分かれている**。
	// 不完全型の unique_ptr を持つので、構築と破棄は定義の見えるこの翻訳単位に置く。
	ObjectHandles::ObjectHandles() : fTable(std::make_unique<ObjectHandleTable>()) {}

	ObjectHandles::~ObjectHandles() = default;
} // namespace HomeskzIfcImport::draw
