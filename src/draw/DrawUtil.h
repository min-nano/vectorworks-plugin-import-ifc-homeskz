//
//	draw/DrawUtil.h
//
//	Phase 2（VW 描画）の共通ヘルパー。要素ごとの draw モジュール（Grid / Story / Floor /
//	Rafter / Roof …）が共通して行う定型処理——クラスの割り当て・描画属性の by-class 化・
//	配置先レイヤの用意——を 1 か所に集める。
//
//	【SDK 依存・include の順序】このヘッダは draw/StructuralMember.h とともに**SDK 型を
//	公開する共通ヘッダ**（MCObjectHandle を引数に取るため）。自身で PluginPrefix.h を
//	include するので、draw/*.cpp のどこから include しても成立する。**このヘッダを
//	要素ごとの draw/*.h から include してはならない**（SDK 型を公開する共通ヘッダ——
//	draw/Tag.h・draw/Legend.h など——は別）。要素ごとの draw/*.h は従来どおり
//	core::Document.h までしか参照しない約束になっているため。
//
//	この約束は、要素ごとの draw/*.h がかつて Extensions/ExtMenu から include されていた
//	ためのもの。現在は殻が draw/ を include しない（draw/ は本体側）が、約束そのものは
//	維持している（CLAUDE.md「依存の向きは厳守する」）。
//
//	【なぜ要るか】これらは以前、各 .cpp の無名名前空間に**逐語的な複製**として置かれていた
//	（SetClassByName は 4 か所、SetAllAttributesByClass は 3 か所）。属性を 1 つ追加する・
//	by-class の指定を修正するといった変更が、修正した .cpp にしか反映されない形になっていた。
//

#pragma once

#include "PluginPrefix.h"

#include "core/Document.h"
#include "core/Layout.h"
#include "core/Progress.h"
#include "draw/Verify.h"

#include "VWFC/VWObjects/VWParametricObj.h"

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace HomeskzIfcImport::draw
{
	// --- SDK へ渡す数値を意味のある名前で持つ ---------------------------------------------
	//
	// ISDK は種別・表示状態・変数セレクタを**short の数値のまま**受け取る。呼び出し側に 0 / 1 /
	// 1064 が並ぶと何を選んだのか読めないので、使うものだけを列挙して名前を付ける。

	// CreateLayer の layerType（SDK の ELayerType。Kernel/API/MiniCadCallBacks.h）。
	enum class LayerKind : short
	{
		Design = 1, // デザインレイヤ
		Sheet = 2,	// シート（プレゼンテーション）レイヤ
	};

	// SetViewportLayerVisibility の表示種別。**対象外のレイヤにグレー（2）は使わない**——
	// 図に薄く残る。グレーは、薄く残したいレイヤ（命令の grayedLayers。母屋伏図の軒桁）に
	// だけ使う。
	enum class LayerVisibility : short
	{
		Visible = 0,
		Hidden = 1,
		Grayed = 2,
	};

	// SetViewportClassVisibility の表示種別（SDK の EClassVisibility。VWFC/VWObjects/
	// VWClass.h）。**VS の 0/1/2 とは値が違う**（1 は「非表示」ではない）。表示へ戻す
	// Normal と、命令が隠すクラスへ適用する Invisible を使う（Grayed は薄く残るので使わない）。
	enum class ClassVisibility : short
	{
		Normal = 0,
		Invisible = -1,
		Grayed = 2,
	};

	// GetObjectTypeN が返すノード種別（Objs.TDType.h）。走査で判別に使うものだけ。
	enum class ObjectNodeType : short
	{
		InternalRecord = 0,	   // 空のシンボル定義が 1 つだけ持つレコード
		SymbolDefinition = 16, // kSymDefNode
	};

	// SetObjectVariable / GetObjectVariable のセレクタ（Kernel/API/ObjectVariables.h）。
	// SDK が名前を持つものはその定数を、持たないものは ci-debug で確かめた番号を入れる。
	enum class ObjectVariable : short
	{
		PlanarObjectIsScreen =
			ovPlanarObjIsSrceen, // 平面図形をスクリーン平面に置くか（綴りは SDK ママ）
		IsStructural = ovIsStructural,				// 構造オブジェクトとして扱うか
		SlabHeight = ovSlabHeight,					// スラブの高さ
		SlabRoofPt1 = ovSlabRoofPt1,				// 屋根面の基準線（始点）
		SlabRoofPt2 = ovSlabRoofPt2,				// 同（終点）
		SlabRoofUpslopePt = ovSlabRoofUpslopePt,	// 登り方向の側の点
		SlabRoofRise = ovSlabRoofRise,				// 勾配の高さ
		SlabRoofRun = ovSlabRoofRun,				// 勾配の底辺
		SheetPaperWidth = ovLayerSheetPaperWidth,	// 用紙の幅（インチ）
		SheetPaperHeight = ovLayerSheetPaperHeight, // 用紙の高さ（インチ）
		// 断面ビューポートの見え方。**SDK に名前が無い**ので番号で持つ（ci-debug で確認）。
		ViewportPlanarObjects = 1035, // プレイナー（レイヤ平面）／2D 図形を表示するか
		Viewport2DComponents = 1059, // ハイブリッドシンボル等の 2D コンポーネントを表示するか
		ViewportBeyondCutPlane = 1064, // 切断面より奥の図形を表示するか
		// 寸法の寸法規格を名前で指定する（M31）。index（ovDimStandard）ではなく名前で
		// 書く——index の意味は図面ごとに変わり、ヘッダの「0 は無効」も実機と合わない
		// （SDK リファレンス Findings「Dimensions」）。
		DimStandardName = ovDimStandardName,
		DimShowValue = ovDimShowValue, // 寸法値を表示するか
		// 寸法の文字の図面上の大きさ（mm）と、文字スタイルの大きさ（**インチ**）。
		// Findings「Dimensions」#143 / #157 / #161。
		DimFontSize = ovDimFontSize,
		TextStyleSize = ovTextStyleSize,
		// ビューポートのビュー行列と、断面ビューポートの断面の向き（CopySectionViewMatrix）。
		ViewportViewMatrix = ovViewportViewMatrix,
		SectionViewMatrix = ovSheetLayerSectionViewportViewMatrix,
	};

	// SetObjectStoryBound / GetObjectStoryBound のバウンド ID（SDK の TObjectBoundID
	// ＝ Sint32。別名が SDK の名前空間の中にあるので実体で持つ）。
	//   * スラブ（床板・底盤）は高さ基準を 1 つだけ持つので常に Slab。
	//   * 構造材（横架材・垂木・柱）は始端・終端の 2 つ（柱では下端＝始端・上端＝終端）。
	enum class StoryBoundSlot : Sint32
	{
		Slab = 0,
		Start = 0,
		End = 1,
	};

	// 寸法・断面寸法データタグ・レベル基準線を置くクラス（M13 / M31）。**注釈はすべてこの
	// クラス**に置き、見え方（色・線の太さ）を図面側のクラスで一括して決められるようにする。
	// 存在しなければ SetClassByName（AddClass）が作る。
	inline constexpr const char* kDimensionClass = "寸法";

	// オブジェクトのクラスを名前で設定する。AddClass は既存なら索引を返し、無ければクラスを
	// 作成する。クラス名が空なら何もしない（無クラス＝既定クラスのまま）。
	void SetClassByName(MCObjectHandle object, const std::string& className);

	// 描画属性（線幅・色・パターン・矢印・透明度）をすべてクラス属性に従わせる。
	// SetObjectClass はクラスを割り当てるだけで各属性は by-instance の既定値のまま残るため、
	// 属性ごとに by-class を指定する（ISDK の関数名は VS と異なる: PColors=ペン色 /
	// FColors=面色 / PPat=線種 / FPat=面パターン / Arrow=マーカー）。
	// withMarker=false なら矢印マーカーだけは個別のまま残す（軸組図のレベル記号の線・▽・
	// 文字。ご要望で「矢印マーカーは除く」）。
	void SetAllAttributesByClass(MCObjectHandle object, bool withMarker = true);

	// クラスを割り当てて、描画属性をそのクラスに従わせる（上の 2 つをこの順で呼ぶ）。
	// 描画したものは**ほぼ必ず**この組で仕上げるので、2 行の繰り返しを 1 か所にまとめる。
	void SetClassWithAttributes(MCObjectHandle object, const std::string& className,
								bool withMarker = true);

	// 【PIO を作成する前にクラスを「文書の既定」として設定する】構築時にクラスと by-class の
	// 指定を文書の既定として設定し、破棄時に元へ戻す。**この間に作成した PIO は最初から
	// そのクラスに属し、6 属性も by-class になる**（SDK リファレンス Findings「Attributes and
	// Classes」）。クラス名が空なら何もしない（SetClassByName と同じく無クラス＝既定のまま）。
	//
	// **このオブジェクトが存在する間に作成したものだけ**が既定を継承するので、スコープは
	// 「作成する 1 行」だけを囲む。破棄時に**設定した既定をすべて元へ戻す**——文書の既定は
	// 利用者の図面の設定なので、取り込みの後に描画するものへ持ち越さない（実機で、戻さない
	// 版では取り込みの後もクラススタイルのままだった。PR #133）。
	//
	// マーカーの既定は設定しない（いずれにしても継承されない。オブジェクトごとに追加の
	// 費用なしで与える）。
	//
	// 【理由】構造材 PIO のように再計算の重い PIO では、作成した**後**の SetObjectClass と
	// 6 つの Set*ByClass が**1 回ごとに PIO を再計算させる**（構造材で 1 回 9〜12ms。取り込み
	// 全体の 54% を占めていた。docs/DEV-NOTES.md「描画の高速化」）。作成する前に文書の既定を
	// 設定しておけば、その 7 回がすべて不要になる。
	//
	// 【戻し方の注意】ペン色・面色・線の太さ・線種・面パターンの既定の by-class には
	// 解除する API が無く、**既定の値を書くとその属性のフラグが解除される**（同じ値でも
	// 解除される。色は 1 回の呼び出しでペンと面の両方）。そこで作成前に**値とフラグの両方**を
	// 退避し、戻すときは値を書き戻してから、**元から設定されていたフラグだけ**を設定し直す
	// （元から by-class の図面を by-instance へ変えてしまわないため。SDK リファレンス
	// Findings「Attributes and Classes」の「既定の by-class は既定の値を書き戻すと下りる」。
	// #102）。
	class ScopedCreationClass
	{
	public:
		explicit ScopedCreationClass(const std::string& className);
		~ScopedCreationClass();
		ScopedCreationClass(const ScopedCreationClass&) = delete;
		ScopedCreationClass& operator=(const ScopedCreationClass&) = delete;
		ScopedCreationClass(ScopedCreationClass&&) = delete;
		ScopedCreationClass& operator=(ScopedCreationClass&&) = delete;

		// 既定に設定したクラスの索引（クラス名が空なら 0）。
		[[nodiscard]] InternalIndex classID() const
		{
			return fClassID;
		}

	private:
		bool fActive = false;
		InternalIndex fClassID = 0;
		// 作成前の文書の既定（破棄時に書き戻す）。値とフラグは別々に保持されている。
		InternalIndex fPreviousClass = 0;
		Boolean fPreviousPenOpacity = false;
		Boolean fPreviousFillOpacity = false;
		ObjectColorType fPreviousColors{};
		short fPreviousLineWeight = 0;
		InternalIndex fPreviousPenPat = 0;
		InternalIndex fPreviousFillPat = 0;
		Boolean fPreviousPColorsByClass = false;
		Boolean fPreviousFColorsByClass = false;
		Boolean fPreviousLWByClass = false;
		Boolean fPreviousPPatByClass = false;
		Boolean fPreviousFPatByClass = false;
	};

	// ScopedCreationClass の中で作成したオブジェクトを仕上げる。**作成されたオブジェクトが
	// 実際に既定を継承したかを読み戻し**（クラスと 6 属性）、継承していればマーカーだけを
	// by-class にする。継承していなければ従来どおり SetClassWithAttributes で設定し直す。
	// 戻り値は既定を継承していたか（false なら設定し直した）。
	//
	// **読み戻しが描画結果を決める**ので、検算と違って本番でも実行する（draw/Verify.h の
	// 基準）。読み出しは再計算を起こさない。マーカーは既定を継承しないが、by-class 化は
	// 再計算を起こさないので費用がかからない。
	bool FinishCreatedWithClass(MCObjectHandle object, const ScopedCreationClass& scope,
								const std::string& className);

	// PIO の定義を**設定ダイアログを出さずに**用意する。その PIO を 1 つでも置くフェーズの
	// 先頭で 1 回呼ぶ。**静的フラグで 1 回だけにしない**——定義は文書ごとなので、次の文書への
	// 取り込みで定義が用意されなくなる。
	//
	// 理由: CreateCustomObject は、その名前の PIO が**その文書に**まだ定義されていなければ
	// DefineCustomObject で定義を作成する。既定が kCustomObjectPrefAlways なので、**最初の
	// 1 個を作成するときだけ「オブジェクトの設定」ダイアログが出て取り込みが止まる**
	// （M12 の記号 PIO で実機確認）。
	void PrepareCustomObjectDefinition(const char* universalName);

	// PIO がいま持っているプロファイルグループ（データタグのタグレイアウト・レベル基準線の
	// マーカーレイアウト）。無ければ nil。**2 つの取得経路を両方確認する**——VW2020 で
	// 「プロファイルグループは aux コンテナに持つ」経路が追加されており
	// （ISDK::GetCustomObjectProfileGroupInAux）、どちらで取得できるかはオブジェクトによって
	// 変わる。
	MCObjectHandle HeldProfileGroup(MCObjectHandle pio);

	// オブジェクトがそのノード種別か（GetObjectTypeN）。
	bool IsObjectType(MCObjectHandle object, ObjectNodeType type);

	// 命令 1 件ぶん進めてよいか。中止（進捗ダイアログのキャンセル）なら false で、呼び出し側は
	// break して残りを描画しない。進捗は**描画の前に** 1 件進める（＝「いま何件目を描画して
	// いるか」が分かる）。要素ごとの描画ループが同じ 4 行を各々書いていた。
	inline bool AdvanceProgress(core::ProgressReporter& progress)
	{
		if (progress.cancelled())
			return false;
		progress.step();
		return true;
	}

	// PIO のパラメータ名を解決する。universal 名で見つかればそれを使い、見つからなければ
	// ローカライズ名（OIP に出る日本語）で引き直す。どちらでも見つからなければ universal 名を
	// そのまま返す。
	// 理由: **名前が 1 つ違うだけで setter は失敗を返さずに値を無視する**（M6 の垂木で実証
	// 済み: 勾配・構造用途・ラベルが名前違いで反映されていなかった）ため、確実に見つかる方を
	// 選ぶ。
	TXString ResolveParamName(const VWParametricObj& pio, const char* universalName,
							  const char* localizedName);

	// 候補が複数あるパラメータ名を解決する。universal 名の候補を順に引き、見つからなければ
	// ローカライズ名（OIP に出る日本語）の候補を順に引く。**どれも無ければ空文字を返す**
	// ——ResolveParamName と違って「書き込めたつもりで値が無視される」ことがないので、
	// 呼び出し側は空文字を「このパラメータは無い」と扱って診断へ回せる。
	//
	// **なぜ候補が要るか**: PIO のパラメータ名は SDK ヘッダのどこにも無く（ci-debug の
	// sdk-grep で確認済み）、実機の OIP を読んで特定するしかない。VW 標準ツールのように
	// 名前の候補を絞りきれないものは、ありうる universal 名とローカライズ名を並べて
	// 引き、どれで一致したかを診断へ記録する（SDK リファレンス Findings「Parametric Objects」）。
	TXString ResolveParamNameAmong(const VWParametricObj& pio,
								   const std::vector<const char*>& universalNames,
								   const std::vector<const char*>& localizedNames);

	// PIO のパラメータ名（universal とローカライズ）のうち、needle を含むものを
	// "universal(ローカライズ)" 形式で連ねて返す。**名前を特定できなかったときだけ**
	// 診断へ載せる（ローカルの VectorWorks でしか読めない情報を 1 周で取得するため）。
	//
	// **開発ビルドだけ**（draw/Verify.h）。出力は診断の文言にしかならず、除外しても利用者の
	// 図面の描画結果は何も変わらない。
#if VW_DRAW_VERIFY
	std::string DescribeParamsContaining(const VWParametricObj& pio, const char* needle);
#endif

	// PIO の文字列パラメータを読む（無ければ・例外なら空）。**PIO 本体（Extensions/）が
	// 自身や他のオブジェクトのパラメータを参照するときの唯一の入口**——柱記号（ExtColumnMark）と
	// 耐力壁（ExtShearWall）がどちらも構造材の構造用途を読むので、try/catch ごとここに
	// 1 つだけ置く。
	std::string PioParamString(const VWParametricObj& pio, const char* name);

	// オブジェクトが構造材ツール（StructuralMember）なら、その**構造用途**（"4"＝柱 /
	// "5"＝小屋束 …。core::kStructuralUse*）を返す。構造材でなければ空。
	//
	// 記号 PIO と耐力壁 PIO が「対象レイヤの中から柱だけを抽出する」のに共有する。
	std::string StructuralUseOf(MCObjectHandle object);

	// PIO に実数パラメータを書き、**読み戻して書き込めたか確認する**。書き込めていれば true。
	// 実数で書き込めなかったときは文字列で書き直す。
	// 理由: 角度・寸法のような数値パラメータでも、PIO の登録次第で実数ではなく文字列として
	// 保持されていることがあり、その場合 SetParamReal は失敗を返さずに無視される（M6 の垂木で
	// 「寸法を文字列で渡すと既定値のままだった」逆のケースが起きており、どちらの場合でも
	// 書き込めるようにする）。
	bool SetParamRealChecked(VWParametricObj& pio, const TXString& param, double value,
							 double tolerance = 1e-6);

	// 構造材ツール（StructuralMember）へ渡す**断面プロファイルのグループ**を作る。
	// 矩形（[minX, minY]〜[maxX, maxY]）1 枚を閉じたポリゴンとしてグループへ入れて返す。
	// 矩形の位置は呼び出し側の**断面基準点の規約**で決まる: 横架材は天端中央基準なので原点が
	// 上辺中央、柱は断面中心基準なので原点が中心（AxisAlign の設定と一致させる）。
	// 幅・せいが 0 以下なら nil。断面がグループに入らなかったときも nil を返すので、
	// 呼び出し側はフォールバックへ回せる。
	//
	// **グループへは VWFC の VWGroupObj::AddObject で入れる**。gSDK->AddObjectToContainer を
	// 直接呼ぶと「レイヤに作成してから移動する」形になり、移動に失敗すると**空のグループ**が
	// 残る。空のプロファイルは断面が無いのと同じで、PIO は生成できても実体が描画されない（＝
	// オブジェクトはあるのに画面に何も表示されない）。そこで入ったかどうかを
	// GetFirstMemberObject で確認し、空なら nil を返す。
	MCObjectHandle CreateRectangleProfileGroup(double minX, double minY, double maxX, double maxY);

	// 名前付きプラグインスタイル（図面枠スタイル等）の RefNumber を引く。文書に無ければ
	// 0 を返す（＝スタイル無しで描画する。スタイルの欠落で部材を失わない）。
	//
	// ISDK はスタイル名から RefNumber を引く呼び出しを持たないので、名前付きオブジェクト
	// （プラグインスタイルはシンボル定義）を GetNamedObject で引き、その InternalIndex を
	// RefNumber として渡す（どちらも SysName を表す Sint32。SDK ヘッダでも InternalIndex と
	// RefNumber は相互に渡し合う形で使われている）。
	RefNumber ResolvePluginStyle(const TXString& styleName);

	// 平面外形を閉じた 2D ポリゴンとして作る（スラブのプロファイル・フォールバック描画）。
	// 頂点が空なら nil。
	MCObjectHandle CreateClosedPolygon(const std::vector<core::Vec2>& boundary);

	// その名前のシンボル定義が図面に在るか（例外は false として扱う）。シンボル置換系
	// （draw/Symbol）が「置けなかった理由を診断へ書き分ける」ために使う唯一の判定。
	//
	// ※ **中身が在るかは分からない**——空のシンボル定義でも true になるし、
	// `GetFirstMemberObject()` も空の定義で非 nil を返す（M19 の実機確認。
	// SDK リファレンス Findings「Symbols」の「付随して分かったこと」）。
	bool HasSymbolDefinition(const std::string& name);

	// オブジェクト変数への書き込みの定型（TVariableBlock の組み立てを 1 か所に）。型ごとに
	// 名前を分けるのは、オーバーロードにすると Boolean（unsigned char）と double の変換順位が
	// 並んで呼び分けが曖昧になるため。
	void SetBooleanVariable(MCObjectHandle object, ObjectVariable variable, Boolean value);
	void SetRealVariable(MCObjectHandle object, ObjectVariable variable, double value);
	void SetPointVariable(MCObjectHandle object, ObjectVariable variable, const core::Vec2& point);
	// 図面の寸法規格の名前を、組み込み（index 1〜9）→ カスタム（0〜−8）の順に並べる
	// （Findings「Dimensions」の index の体系）。読めない index は飛ばす。設定ダイアログの
	// 候補と、規格の文字スタイルの引き当て（DimensionStandardTextStyle）が共有する。
	std::vector<std::pair<short, std::string>> DimensionStandards();

	// その名前の寸法規格が持つ文字スタイル（ref number）。規格が無い・文字スタイルを持たない
	// （組み込み規格はどれも持たない）なら 0。寸法へは SetTextStyleRef で適用する——注釈に
	// 置いた寸法は〈クラスの文字スタイル〉のままだと値が描画されない（Findings「Dimensions」
	// #157）。
	InternalIndex DimensionStandardTextStyle(const std::string& name);

	// 文字スタイル（ref number）の大きさを**紙の pt** で返す（ovTextStyleSize はインチ）。
	// 読めなければ 0。
	double TextStylePoints(InternalIndex style);
	// 文字列のオブジェクト変数を書く。**書けたか**を返す（寸法規格の名前は、図面に無い
	// 名前だと SetObjectVariable が false を返して値が変わらない。Findings「Dimensions」）。
	bool SetTextVariable(MCObjectHandle object, ObjectVariable variable, const std::string& text);

	// 一覧に無ければ追加する（登場順の dedupe。診断へ残すシンボル名・伏図記号レイヤ名・
	// レベル種別の事前登録が同じ形を各々書いていた）。**参照を三項演算子で束ねてから
	// push_back する形にしない**——clang-tidy の misc-const-correctness がその形の変更を
	// 見落とし、束ねた先の vector に const を要求してくる（CI の tidy-mac / tidy-windows）。
	void PushUnique(std::vector<std::string>& values, const std::string& value);

	// 診断の 1 文を追加する（count が 0 なら何もしない）。"<説明> <件数> <助数詞>（<補足>）。"
	// の形で、detail が nullptr なら補足を省く。要素ごとの診断が同じ 2 行を 20 か所以上で
	// 各々書いていた。
	void AppendCount(std::string& text, const char* what, std::size_t count,
					 const char* counter = "件", const char* detail = nullptr);

	// 診断・記録の行を改行区切りで追加する（text が空なら無視・sink が nullptr なら何もしない）。
	// 要素ごとの診断の連結（draw/ExecuteDocument）と伏図・軸組図の診断組み立て
	// （draw/Sheet・draw/Section）が同じ 4 行を各々持っていた。
	void AppendLine(std::string* sink, const std::string& text);

	// 本文を改行で分割して 1 行ずつにする（末尾の空行は除く）。**ダイアログの本文は
	// 1 行 1 コントロール**で組むので、結果ダイアログ（draw/ResultDialog）がこれを使う
	// ——VWStaticTextCtrl は埋め込んだ改行がそのまま行になる保証を持たないため、分割は
	// 呼び出し側で行う。
	std::vector<std::string> SplitLines(const std::string& text);

	// 「用紙・マスに収まったか」を測定して確認するときの許容差（用紙 mm）。線の太さのぶん
	// 外形がわずかに広がるので、ぴったりの図を「はみ出した」と数えない。伏図（draw/Sheet）と
	// 軸組図（draw/Section）が同じ値で判定する（値がずれると片方だけ「収まらなかった」と
	// 診断される）。
	inline constexpr double kFitTol = 1.0;

	// 用紙 mm の寸法を診断の 1 行にする（"325.4×198.0"）。
	std::string DescribePaperSize(const core::Vec2& size);

	// **収まらなかった 1 枚目の実測**を 1 行にする（"3: 測った 402.1×205.6 / 枠 383.0×297.0
	// / 横に 19.1 はみ出し"）。number は図番、drawn は測った外形、frame は割り当てた枠
	// （どちらも用紙 mm）。伏図（draw/Sheet）と軸組図（draw/Section）が共有する唯一の実装で、
	// できた文字列は AppendCount の detail へ添える。
	//
	// 【なぜ件数だけでは足りないか】「用紙に収まらなかった伏図 N 枚」は**原因を何も
	// 示していない**——見積もり（core::planContentBounds）が用紙 2〜3mm ぶん足りないのか、
	// 前の周の描画結果が残っていて図そのものが 2 倍になっているのかで、修正する箇所が
	// まったく違う。どちらかは**はみ出した量**ですぐに判別できる（柱・横架材が退化した
	// （長さ 0 になった）1 本目の実測を添えるのと同じ方針。draw/StructuralMember の
	// collapsedProbe）。M29。
	std::string DescribeFitOverflow(const std::string& number, const core::Vec2& drawn,
									const core::Vec2& frame);

	// --- 高さ基準（ストーリバウンド）の定型 ----------------------------------------------
	//
	// バウンド ID は上の StoryBoundSlot。

	// 命令の高さ基準（StoryBoundCommand）を SDK の SStoryObjectData へ変換する。**この変換は
	// ここに 1 つだけ置く**——かつて床板（インライン展開）・基礎（StoryBound）・構造材
	// （StoryBoundOf）が同じ 6 行を各々持っており、フィールドを 1 つ追加すると 3 か所を
	// 修正する形になっていた。
	VectorWorks::SStoryObjectData StoryBoundData(const core::StoryBoundCommand& bound);

	// 高さ基準を 1 つ書いて、**書き込めたかを返す**（`ISDK::SetObjectStoryBound` は bool を
	// 返す）。**戻り値を捨てない**——捨てていたため「命令どおりに書いたつもりの高さ基準」
	// と「VW が実際に持っている高さ基準」を切り分けられず、実体が無い柱の原因を
	// 解析側とも描画側とも決められない周が 4 つ続いた（docs/DEV-NOTES.md
	// 「柱が長さ 0 で描かれる（M27）」）。
	bool ApplyStoryBound(MCObjectHandle object, StoryBoundSlot slot,
						 const core::StoryBoundCommand& bound);

	// **VW が実際に持っている**高さ基準を読み戻して 1 行にする（`HasObjectStoryBound` ＋
	// `GetObjectStoryBound`）。診断専用で、命令の値ではなく**図面の値**を出すことに意味が
	// ある——同じ命令から作成した柱の一部だけが実体を持たないとき、record が書き込めていない
	// のか・書き込めているのに解決が違うのかは、ここでしか判別できない。
	// 高さ基準が無ければ "なし"、読めなければ "読めない" を返す。
	//
	// **開発ビルドだけ**（draw/Verify.h）。**書き込む側（`ApplyStoryBound`）は本番にも要る**
	// ——除外してよいのは「書いた record を読み戻して並べる」こちらだけである。
#if VW_DRAW_VERIFY
	std::string DescribeStoryBound(MCObjectHandle object, StoryBoundSlot slot);
#endif

	// **プラグインオブジェクト（PIO）が実際に持っているパス**を読み戻して 1 行にする
	// （`GetCustomObjectPath` ＋ `NurbsGetNumPts` ＋ `NurbsGetPt3D`）。診断専用。
	//
	// **開発ビルドだけ**（draw/Verify.h）。
	//
	// 【なぜ要るのか】M27 で、上下端の高さ基準を**どう書いても**（ストーリ相対でもレイヤ基準
	// でも、VW が記録しているとおりでも）実体が 0 のままの柱が 46 本あった。**高さ基準は
	// 両端の Z を決めていない**ということなので、残る入力はパスだけである。渡した曲線は
	// 2 点だった（`PathProbe`）が、**PIO の中のパスがそうとは限らない**——それを確認するのが
	// この関数である（docs/DEV-NOTES.md「柱が長さ 0 で描かれる（M27）」）。
#if VW_DRAW_VERIFY
	std::string DescribePioPath(MCObjectHandle object);
#endif

	// **PIO が実際に持っているパスの、両端の距離**（3 次元）。読めたら true を返し、
	// outLength に長さを入れる。読む口は `DescribePioPath` と同じ（`GetCustomObjectPath` ＋
	// `NurbsGetNumPts` ＋ `NurbsGetPt3D`）で、違うのは**人へ見せる文字列ではなく数として
	// 返す**ことだけ。ピース索引の起点が 0 / 1 のどちらの規約かは分からないままなので、
	// **2 点以上あった最初のピース**を測る。
	//
	// **開発ビルドだけ**（draw/Verify.h）。観測するだけでパスには触らないので、除外しても
	// 描画結果は何も変わらない。唯一の呼び出し元は `MeasureDrawnMember` の `Horizontal`
	// 分岐で、そちらも開発ビルドだけになった——読み戻す理由が検算だけになったためである
	// （自己修復の撤去。docs/DEV-NOTES.md「柱が長さ 0 で描かれる（M27）」）。
	//
	// 【これが「描画された実体」そのものである】構造材 PIO は `ResetObject` のときに、
	// **解決済みストーリバウンドから自身のパスを再生成する**——始点が ID 0 の解決 Z、終点が
	// ID 1 の解決 Z になり、水平成分は渡したパスのまま残る（[Findings「Parametric Objects」
	// の「高さ・実体を最終的に決めるのは…」](https://github.com/min-nano/vectorworks-developer-sdk-reference/blob/main/Findings/Parametric%20Objects.md)）。
	// したがって**リセット後のこのパスが、材がどれだけの実体を持って描画されたかを示す
	// 唯一の値**である（同 Findings が検算の手段として挙げているのもこの読み方）。
	// **パラメータでは代わりにならない**——構造材 PIO に部材長のパラメータは無く、名前で
	// 引ける `CenterPointLength`（OIP の「長さ」）は部材長ではない（実長 5333 の柱で 100 を
	// 返した。docs/DEV-NOTES.md M27）。
#if VW_DRAW_VERIFY
	bool PioPathChord(MCObjectHandle object, double& outLength);
#endif

	// --- 複合オブジェクトの構成（スラブ＝床板 M5・底盤 M9／壁＝立上り M9 が共有する作法）---
	//
	// 床（draw/Floor）と底盤（draw/Footing）は**同じ手順**でスラブを描画する（外形ポリゴン →
	// CreateSlab → クラス → 構成層・基準面 → SetSlabHeight → バインド → ResetObject）。
	// 違うのは構成層の中身だけなので、SDK を呼び出す部分はここに 1 つだけ置く（かつては
	// draw/Floor.cpp の無名名前空間にあり、底盤が同じものを 2 つ目に書く形になっていた）。
	//
	// ★**スタイルは作成しない・適用しない**（スラブ・壁とも）。構成層・基準面は**各オブジェクトへ
	// 直接**設定し、オブジェクトはスタイル無し（unstyled）のままにする。当初は「厚みごとに
	// スタイルを新規作成して適用する」形だったが、
	//   * インポートのたびに名前付きリソース（スラブ／ウォールスタイル）が増える。undo では
	//     消えないので、取り消してもリソースだけが残る（DrawUtil.h「取り込み全体の Undo」）。
	//   * 名前が埋まっていれば " (2)" … と連番になり、同じ構成のスタイルが図面に並ぶ。
	//   * 構成そのものは命令が全て持っているので、スタイルという間接段を挟む必要が無い。
	// という理由で、オブジェクトへ直接与える形へ改めた。個々のオブジェクトを後から手で
	// 編集しても他へ波及しない（スタイルで束ねていたときとの唯一の差）。

	// オブジェクト（スラブ／壁）の構成層を命令どおりに再構築する。
	//
	// 手順: **先頭に命令の層を順に挿入し、その後ろに残った元の層を削除する**。
	//   * 厚み 0 の層は VW が受け付けない（＝余った層の厚みを 0 にする方法では構成を
	//     確定できない）ので、
	//     余った層は必ず削除する。
	//   * 先に挿入してから削除するので、途中で層が 0 枚になる瞬間が無い
	//     （層が 1 枚も無いスラブ／スタイルは作れない）。
	//   * 削除に失敗したら（想定外の API 挙動）そこで打ち切り、元の層が残ったままでも
	//     スラブ自体は残す（1 枚の失敗で全体を止めない）。
	//
	// ★コンポーネントの索引は **0 始まり**（実機で確認: 索引 1 に挿入すると既定層の後ろへ
	// 入り、索引 = 層数 で削除すると範囲外で失敗した）。GetNumberOfComponents が返すのは
	// 「個数」なので、有効な索引は 0 … 個数−1。
	//
	// 併せて**層ごとのクラス（素材）と by-class 属性**を設定する。命令の drawClass
	// （"z構成要素-コンクリート" 等。core/Document.h「構成要素のクラス」）を層へ割り当て、
	// 層が持つ描画属性——**塗り**（SetComponentUseFillClassAttr）と**左右のペン**
	// （SetComponentUsePenClassAttr）——をすべてそのクラスの属性に従わせる。オブジェクト
	// 本体に対する SetClassByName ＋ SetAllAttributesByClass と同じ意図で、クラスを
	// 割り当てただけでは挿入時の既定値（0）が by-instance のまま残るため、明示的に指定する。
	void SetComponents(MCObjectHandle object,
					   const std::vector<core::ComponentCommand>& components);

	// スラブ（またはスラブスタイル）の高さ基準面を設定する。基準面は「どの構成要素か」
	// （SetDatumSlabComponent）＋「その上端か下端か」（SetComponentDatumIsTopOfComponent）の
	// 2 つで決まる。スラブスタイル設定ダイアログの「基準面」欄のポップアップとラジオが
	// それぞれこの 2 つに対応する。
	//   Top    … 最上層（索引 0）の**上端**＝スラブ天端
	//   Bottom … 最下層（索引 個数−1）の**下端**＝スラブ底面
	void SetSlabDatum(MCObjectHandle object, core::SlabDatum datum, short componentCount);

	// 壁（スタイル無し）へ構成層を組む。**スラブと違って基準面は持たず**、構成層の合計が
	// そのまま壁厚になる（立上りはコンクリート 1 層＝壁厚。parse/Footing）。
	//
	// 併せて**コア構成要素**を指定する（VW が結合部で構成要素を融合する基準になる。指定が
	// 無いと壁結合しても平面で層が繋がらず、取り合いに面線が残りうる。docs/DEV-NOTES.md M10）。
	// 立上りは構成が 1 層なので、その 1 枚（索引 0）がコアになる。索引は SetComponents と
	// 同じ **0 始まり**（上記 ★）と解釈している——VS 版 SetCoreWallComponent の説明は
	// 「0 はコア無しにする」だが、VS の構成要素索引は 1 始まりで ISDK のそれは 0 始まりなので、
	// ここは先頭の層を指す。スタイルへ同じ呼び出しをしていた M10 でも、これが原因の問題は
	// 起きていない（T 字の面線の原因は ResetObject 不足だった。docs/DEV-NOTES.md M10）。
	// 構成層が空なら何もしない。
	void SetWallComponents(MCObjectHandle wall,
						   const std::vector<core::ComponentCommand>& components);

	// --- 取り込み全体の Undo（docs/DEV-NOTES.md M15）--------------------------------------
	//
	// 【なぜレイヤを記録するのか】VectorWorks は取り込みの開始時に undo イベントを開かない
	// （実機の診断ログで確認。docs/DEV-NOTES.md M15）。そこで**自分でイベントを開き**、
	// `AddAfterSwapObject` で「あとで消してよいもの」を登録する。登録するのは
	// **このインポートが新しく作ったレイヤだけ**——レイヤを消せばその上の図形も消えるので、
	// 図形を 1 つずつ登録する必要は無く、**二重登録（レイヤと中身の両方）で undo が既に
	// 削除されたものを削除しようとする不具合**も避けられる。
	//
	// レイヤ以外に取り込みが作るもの（クラス・ストーリ・レベルテンプレート）はリソースで、
	// undo へ登録しないので戻らない。空のクラスが残るが、図面の見た目は取り込み前に戻る。
	//
	// 【既にあったレイヤ】2 回目の取り込みのように、**取り込み前から在ったレイヤ**へ描画した
	// 分は登録できない（そのレイヤごと消すわけにいかない）。その場合は取り消しが部分的に
	// なるので、完了ダイアログでその旨を伝える（ImportUndoScope::partial）。

	// 取り込みの図面変更をまるごと包む undo イベント。**構築で開き、破棄で閉じる**
	// （途中で例外が出ても閉じる）。記録が 1 件も無ければ、閉じるときにイベントごと
	// 破棄する——空のイベントを残すと「取り消し」が中途半端に機能して図面が壊れるため
	// （実機で確認。docs/DEV-NOTES.md M15）。
	class ImportUndoScope final
	{
	public:
		ImportUndoScope();
		~ImportUndoScope();
		ImportUndoScope(const ImportUndoScope&) = delete;
		ImportUndoScope& operator=(const ImportUndoScope&) = delete;
		ImportUndoScope(ImportUndoScope&&) = delete;
		ImportUndoScope& operator=(ImportUndoScope&&) = delete;

		// 取り消しで戻せる状態か（レイヤを 1 つでも登録できたか）。
		bool armed() const
		{
			return !fCreatedLayers.empty();
		}

		// **取り込み前から在ったレイヤ**へも描画したか（＝取り消しはその分だけ戻らない）。
		bool partial() const
		{
			return !fExistingLayers.empty();
		}

		// 取り込み前から在ったレイヤの名前（登場順に重複なし）。
		//
		// **真偽 1 つでは足りない。** 図面のテンプレートに「共通」等が最初から在れば、
		// 1 周目からこれは空にならない——「戻し忘れ」と「もともと在った」を真偽では
		// 区別できない（実機の指摘。docs/DEV-NOTES.md M23「基準は 1 周目に採る」）。
		// 名前で持っておけば、1 周目のレイヤの一覧を基準にして次の周と**照合できる**。
		const std::vector<std::string>& existingLayers() const
		{
			return fExistingLayers;
		}

	private:
		// 記録の実体はスコープが持ち、下の 2 つの自由関数が「いま開いているスコープ」を
		// 通して書き込む（要素側の draw モジュールはスコープを持ち回らずに済む。
		// インポートはメインスレッドから 1 本しか走らないので、開いているスコープは高々 1 つ）。
		friend void RecordCreatedLayer(MCObjectHandle layer);
		friend void NoteExistingLayerUsed(MCObjectHandle layer, const std::string& name);

		bool contains(MCObjectHandle layer) const;

		std::vector<MCObjectHandle> fCreatedLayers; // このインポートが作ったレイヤ
		std::vector<std::string> fExistingLayers; // 取り込み前から在ったレイヤ（登場順）
	};

	// **図面のすべてのレイヤで選択を解除する。** 取り込みの終わりに呼ぶ。走査中の異常は
	// そこで打ち切って false を返す（選択が残るだけで、図は壊れない）。図面の中身（形・属性）
	// には触らないので undo イベントに何も記録しない。
	//
	// 理由: VectorWorks は作成したオブジェクトを選択したまま残すので、そのままにすると数百の
	// 部材が選択された状態で戻り、次のクリックやキー操作（Delete・移動）が取り込んだ図全体へ
	// 適用されてしまう。
	//
	// 【全レイヤを辿る】`DeselectAll` だけに頼らず、デザイン・シートの全レイヤの直下の
	// オブジェクトを 1 つずつ `SelectObject(h, false)` で選択解除する。取り込みは多数の
	// レイヤへ描画するので、アクティブレイヤ以外に選択が残る余地を作らない。選択はレイヤ直下の
	// オブジェクトの状態なので、グループや PIO の中身までは辿らない。
	bool DeselectEverything();

	// このインポートが新しく作ったレイヤを undo イベントへ登録する（デザイン／シートの
	// どちらも）。イベントが開いていなければ何もしない。nil は無視。
	void RecordCreatedLayer(MCObjectHandle layer);

	// 取り込み前から在ったレイヤへ描画したことを記録する（取り消しが部分的になる）。
	// レイヤを用意するヘルパー（PrepareLayer / ActivateExistingLayer / PrepareSheetLayer）が
	// 自身で呼ぶので、要素側は意識しなくてよい。
	// **名前も一緒に渡す**——1 周目のレイヤの一覧を基準に、次の周で図面が戻っているかを
	// 照合するため（ImportUndoScope::existingLayers）。
	void NoteExistingLayerUsed(MCObjectHandle layer, const std::string& name);

	// **SDK に渡して消費させる下ごしらえのオブジェクト**（PIO のパス・プロファイル等）を
	// 「このインポートが追加したもの」として undo イベントへ申告する。
	//
	// 【なぜ要るか】通り芯は `CreateCustomObjectPath` にポリライン（パス）を渡して PIO を
	// 作成する。SDK はそのポリラインを **undo 記録つきで削除**して PIO へ取り込むため、こちらが
	// イベントを開いていると「削除」がその記録に入り、**取り消しでポリラインが復活する**
	// （実機で確認: 取り込み直後は PIO だけなのに、取り消すとレイヤ「共通」に曲線だけが残った）。
	//
	// 対処は SDK の作法どおり「**自分が追加したものは申告する**」——`AddAfterSwapObject` の
	// 説明は "Use this callback after you add an object in your routine. A reference to h is
	// stored in the undo table, and that object is deleted when Undo is selected."
	// つまり申告しておけば、取り消しのときに**復活したポリラインが改めて削除される**。
	//
	// レイヤ（RecordCreatedLayer）と違い、**レイヤの上に通常どおり置いた図形へは使わない**
	// ——レイヤごと削除されるものを二重に登録しない（DrawUtil.h「なぜレイヤを記録するのか」）。
	// 使うのは「SDK へ渡して消費されるもの」だけ。
	void RecordCreatedObject(MCObjectHandle object);

	// 名前付きデザインレイヤを取得（無ければ作成）してアクティブにする。以後に生成する
	// オブジェクトはこのレイヤへ入る。取得・生成できなければ nil を返し、カレントレイヤも
	// 変えない。**通り芯の "共通" レイヤのように、その要素が自分で用意してよいレイヤ専用。**
	MCObjectHandle PrepareLayer(const std::string& layerName);

	// 既存の名前付きデザインレイヤをアクティブにする。**存在しなければ何もせず nil**（レイヤ
	// を作らない）。ストーリ由来のレイヤ（"1-FL" / "n-垂木" / "n-野地板"）は story
	// 命令が作成するので、無い＝そのストーリの生成がスキップされたということ。要素のために
	// 独自にレイヤを作成しない。
	MCObjectHandle ActivateExistingLayer(const std::string& layerName);

	// --- シートレイヤとビューポート（伏図＝M13・軸組図＝M14 が共有する作法）------------
	//
	// 伏図（draw/Sheet）と軸組図（draw/Section）は、ビューポートの**種類が違うだけ**で
	// 前後の処理は同じ（シートレイヤを用意 → 生成 → 表示レイヤを絞る → クラスを表示に
	// 戻す → 縮尺 → 図面タイトル・図番 → 更新）。SDK を呼び出す部分はここに 1 つだけ置く。
	// 断面ビューポートも例外ではなく、ISDK::CreateSectionViewport のコメントが
	// 「クラス・レイヤの表示はこの呼び出しでは扱わない。呼び出し後に設定し、そのあとで
	// ビューポートを更新すること」と明記している。

	// ビューポート共通の下ごしらえ。図面の全レイヤと、**表示に戻すクラス**の索引を持つ。
	//
	// classes は**昇順・重複なしの vector**（集合として使うが std::set では持たない）。
	// Windows の clang-tidy が std::set を持つ構造体の暗黙の特殊メンバに
	// bugprone-exception-escape を出すため、列挙中だけ set を使い、結果は vector へ移す
	// （用途は「1 つずつ表示へ戻す」走査だけなので、連続領域の方が単純でもある）。
	//
	// 【クラスを表示へ戻す理由】ビューポートはクラスの表示を明示しないと**非表示のまま**（M13
	// のローカル確認で判明。レイヤは命令どおりなのに図形が 1 つも表示されなかった）。そこで
	// **ドキュメントの全クラスを表示へ戻す**——列挙は VWClass::ForEachClass（＝ISDK::
	// ForEachClass の VWFC 版）で行う。
	//
	// **［訂正の記録］**M13 では「ISDK にドキュメントの全クラスを列挙する呼び出しが無い」と
	// 判断し、図形に割り当てられているクラスを全レイヤ走査で数え上げ、命令セットが名乗る
	// クラス名（当時の core::documentClassNames）も保険で追加していた。**この前提が誤りで**、
	// SDK には ForEachClass がある（sdk-grep で確認）。走査による推定は、取得漏れがあれば
	// 図形が表示されないうえに、ビューポート注釈のように後から追加したものを別経路で
	// 取得し直す必要もあった。全クラス表示なら「どのクラスが要るか」を推定する必要そのものが
	// 無くなる。
	struct ViewportSetup
	{
		std::vector<MCObjectHandle> layers;
		std::vector<InternalIndex> classes;
	};

	// 上の下ごしらえを行う。図面の規模なりに走査するので、**ビューポートを作るフェーズごとに
	// 1 回だけ**呼ぶこと（伏図・軸組図がそれぞれ 1 回。フェーズをまたいで持ち回さないのは、
	// 要素ごとの draw/*.h に SDK 型を出さない約束を守るため。DrawUtil.h 冒頭参照）。
	ViewportSetup PrepareViewportSetup();

	// シートレイヤを用意する（同じ番号のものがあれば再利用）。**シートレイヤ番号はレイヤ名が
	// 担う**。タイトルはレイヤの説明＝オブジェクト変数 159（ovLayerDescription。"only used
	// for sheet layers"）へ入れる。用意できなければ nil。
	MCObjectHandle PrepareSheetLayer(const std::string& number, const std::string& title);

	// ビューポートで**いまドキュメントにある全クラス**を表示へ戻す（戻せた数を返す）。
	// ConfigureViewport が使うのと同じ列挙・同じ表示種別で、**ビューポートを仕上げた後に
	// 増えたクラス**を取得し直すためのもの。
	// hiddenClasses（core::ViewportCommand の同名フィールド）に挙がったクラスは非表示のまま
	// 保つ——**ConfigureViewport と同じものを渡す**こと（渡し忘れると隠したクラスが表示に戻る）。
	//
	// 理由: 注釈へ後から置いたデータタグは、スタイルが決める中身と一緒に新しいクラスを
	// 文書へ持ち込むことがある（draw/Tag）。
	std::size_t ShowAllViewportClasses(MCObjectHandle viewport,
									   const std::vector<std::string>& hiddenClasses);

	// ビューポートの投影をどう扱うか（ConfigureViewport の引数）。
	//
	// **軸組図（断面ビューポート）は Keep**——あちらは断面の向きで作成されており、平面へ
	// 変更しては意味を成さない。
	//
	// 【伏図は 2D/平面（Top/Plan）へ再生成する必要がある】`CreateViewport` が作成する平面
	// ビューポートは、**オブジェクト情報パレット上は「2D/平面」と表示されるのに、
	// 実際の描画は 3D の「上」ビューのまま**という食い違いを起こす（実機で確認された症状。
	// 更新ボタンを押しても直らず、パレットでいったん「上」を選んでから「2D/平面」
	// へ戻すと正しく描画される）。対処は**ユーザーの手動対処をそのまま SDK で再現する**こと——
	// Project 2D（オブジェクト変数 1005）をいったん OFF にして**更新を挟み**、再度 ON
	// に戻して 2D/平面のキャッシュを再生成する。
	enum class ViewportProjection
	{
		Keep, // いまの投影のまま触らない（軸組図＝断面ビューポート）
		Plan, // 2D/平面（Top/Plan）へ再生成する（伏図）
	};

	// ConfigureViewport の結果。**どちらも「反映されなかったこと」を呼び出し側の診断行へ
	// 出すためのもの**で、図そのものは失敗しても残る。
	struct ViewportFinish
	{
		// 表示へ戻せたクラスの数（0 なら図形が 1 つも表示されない）。
		std::size_t classesApplied = 0;
		// 2D/平面へ再生成できたか（`ViewportProjection::Keep` のときは常に true）。
		// **書き込めたかどうかは読み戻して確認する**——SDK の setter は書き込めなかったときも
		// 失敗を返さずに何もしないので、「設定したつもりで反映されていない」は目視では
		// 判別できない。
		bool planViewApplied = true;
	};

	// 生成済みのビューポートを命令どおりに仕上げる（表示レイヤの絞り込み → クラス表示 →
	// 縮尺 → ［伏図なら 2D/平面の再生成］→ 図面タイトル・図番 → 更新）。
	//
	// クラスは全クラスを表示へ戻し、命令の hiddenClasses に挙がったものだけ非表示にする。
	//
	// 表示レイヤは「まず全部隠してから、命令に挙げたものだけ表示へ戻す」——ビューポートは
	// 既定でドキュメントの表示状態を引き継ぐため、挙げていないレイヤが映り込む。グレー表示
	// （2）は薄く残るので対象外のレイヤには使わず、必ず非表示（1）にする。命令の
	// grayedLayers に挙がったレイヤだけをグレーにする（母屋伏図に軒桁を薄く重ねる）。
	//
	// **投影の再生成は「表示レイヤを絞った後・最後の更新の前」**に行う（上の
	// ViewportProjection）。再生成は更新を 1 回挟むので、レイヤを絞る前に行うと図面の
	// 全レイヤを描画することになり、無駄に重い。順番を入れ替えないこと。
	//
	// scale は縮尺の分母（1/100 なら 100.0）。**用紙と建物の大きさから呼び出し側が決める**
	// （core::planLayout / core::sectionLayout。M18）。0 以下なら縮尺には触らない
	// ——ビューポートの既定のままになる。かつてはここが「映すデザインレイヤの縮尺」を読んで
	// 適用していたが、デザインレイヤの縮尺は用紙に対する図の大きさとは関係が無く、図が用紙から
	// はみ出しても気付けなかった。
	ViewportFinish ConfigureViewport(MCObjectHandle viewport, MCObjectHandle sheetLayer,
									 const ViewportSetup& setup,
									 const core::ViewportCommand& command,
									 ViewportProjection projection, double scale);

	// シートレイヤの用紙まわりの実測値（長さはすべて用紙 mm）。
	//
	//   printable       … **印刷可能領域**（＝図を置いてよい矩形）。割り付けはこれを使う
	//   paper           … 用紙の外形の大きさ（ovLayerSheetPaperWidth/Height＝167/168）
	//   sheet           … シートレイヤの大きさ（VWLayerObj::GetSheetWidht＝165/166）
	//   margins         … 解釈後の 4 辺の余白（mm）。解釈できなければすべて 0
	//   rawMargins      … ISDK::GetPageMargins が返した**生の値**（単位不明のまま）
	//   marginsQueried  … ISDK::GetPageMargins が**実際に値を書いたか**。★この API は
	//                     戻り値を持たないので、有り得ない値（負）を種に置いてから呼び、
	//                     種のまま戻ったら「書かなかった」とみなす（SheetPaperArea の実装）。
	//                     **これが無いと「縁なし印刷の 0」と「読み出せずに 0」を見分け
	//                     られない**（M29）
	//   marginsRead     … 余白を意味のある値として解釈できたか（**四辺 0 も「できた」**
	//                     ——縁なし印刷ができる機種では余白 0 の用紙設定が実際に選べる。
	//                     判定は core::resolvePageMargins）
	//   marginsInInches … その解釈が「インチ」だったか（false なら mm とみなした）
	struct SheetPaper
	{
		core::PaperArea printable;
		core::Vec2 paper;
		core::Vec2 sheet;
		core::PageMargins margins;
		core::PageMargins rawMargins;
		bool marginsQueried = false;
		bool marginsRead = false;
		bool marginsInInches = false;
	};

	// シートレイヤの用紙と印刷可能領域を読む。用紙が読めなければ core::kDefaultPaperSize
	// （A3 横）で代用する（用紙が読めないだけで図を捨てない）。
	//
	// ★**用紙の大きさと印刷可能領域は別の値**（M18）。かつては用紙の大きさだけを読み、
	// 余白は四辺 15mm と決め打ちしていたが、余白は用紙ではなく**印刷の設定**が決めるので、
	// 仮定した瞬間に実際とずれる（狭く見積もれば図が 1 段階小さくなり、広く見積もれば
	// 印刷で切れる）。SDK には両方がある——用紙は ovLayerSheetPaperWidth/Height（167/168）、
	// 余白は ISDK::GetPageMargins（4 辺）で、シートレイヤの大きさ（165/166＝
	// VWLayerObj::GetSheetWidht。**インチ**なので 25.4 倍して mm にする）はまた別値。
	// **GetPageMargins だけ単位がヘッダに書かれていない**ので、インチと mm のどちらとして
	// 読むかは「用紙 − 余白」がシートレイヤの大きさと一致するかで決め、決められなければ
	// 用紙に収まる方を採る（**解釈そのものは無 SDK の純計算**なので core/Layout の
	// resolvePageMargins に置いてある。四辺 0 を「余白なし」として受け取る理由もそこ）。
	// **実機では図面の単位で返った**
	// （mm の図面で 2.963 → 420 − 5.969 = 414 ＝ シートレイヤの幅。M18 のローカル確認）
	// が、単位が図面依存である以上インチの図面ではインチで返るはずなので、**「mm 固定」に
	// はしない**。**採った解釈と生の値は診断へ出す**（draw/Sheet）ので、別の環境でも
	// 実機で確かめられる。
	//
	// ★**用紙は原点を中心に置かれている前提**で矩形を組む。用紙が図面座標のどこに在るかを
	// 返す呼び出しは無い（ObjectVariables にも位置の変数は無い。ci-debug で確認）。
	// VWLayerObj::GetSheetOrigin() はあるが、それが用紙の中心を指すのか隅を指すのかは
	// ヘッダからは決まらないので**使わない**——意味の分からない値を使うより、規約を 1 つ
	// 決めて実機で確かめる方がよい（docs/DEV-NOTES.md M18「用紙の位置」）。
	SheetPaper SheetPaperArea(MCObjectHandle sheetLayer);

	// ビューポートの外形（用紙 mm）を測る。中心と大きさを書き戻し、測れれば true。
	//
	// 【なぜ測るのか】ビューポートの実寸は**描画するまで分からない**（映る図形の広がりで
	// 決まる）。したがって「どこに置くか」は生成・更新の後に測定してから決める（データタグを
	// 置いた後に測定して補正するのと同じ考え方。draw/Tag）。大きさは**見積もった縮尺で実際に
	// 収まったか**を確認して診断へ残すのにも使う（core/Layout.h の PlanLayout::plan）。
	bool MeasureViewport(MCObjectHandle viewport, core::Vec2& center, core::Vec2& size);

	// ビューポートを再描画する（`VWViewportObj::Update`）。できたら true。
	//
	// ★**外形を測る前に、中身を変更したなら必ず呼ぶ。** 更新は重いので**中身を変えたときだけ**
	// 呼ぶこと。
	//
	// 理由: `GetObjectBounds` が返すのは**最後に描画したときの外形**なので、再描画していない
	// ビューポートを測ると「いま図面に何が在るか」ではなく「前に何が在ったか」を測ることになる
	// ——同じ命令・同じ割り付けなのに「用紙に収まらなかった」の件数が周ごとに変動した原因が
	// ここだった（M29。伏図は**耐力壁レイヤの縮尺を変更した後**＝図の中身が変わった後に、
	// 縮尺が同じなら再描画せずに測っていた）。
	bool RefreshViewport(MCObjectHandle viewport);

	// 生成済みのビューポートの**縮尺だけ**を差し替えて再描画する。書き込めたら true。
	//
	// ConfigureViewport で一度仕上げた後に縮尺を変えたいときに使う。更新を 1 回余分に
	// 実行するので、**縮尺が実際に変わったときだけ**呼ぶこと。
	//
	// 理由: 伏図は**凡例の実測**（draw/Legend の measureLegendWidth）を待って初めて最終的な
	// 縮尺が決まるが、その凡例に何が並ぶかはビューポートに映るものが決めるので、先に仮の
	// 縮尺で図を作成せざるを得ない（draw/Sheet の 2 巡）。
	bool ApplyViewportScale(MCObjectHandle viewport, double scale);

	// 断面ビューポートの**断面の向き（1055）をビュー行列（1050）へコピーする**。書き込めたら
	// true。**UpdateViewport は 1050 を単位行列へ戻す**ので、更新を済ませた後に呼び、注釈の
	// 個体を ResetObject する。
	//
	// 理由: CreateSectionViewport が作成するビューポートはビュー行列が単位行列のまま残り、
	// 注釈に縦の基準が無い——ストーリレベルへ結んだレベル基準線が高さ 0 で描画される
	// （UI で作成した断面ビューポートは 2 つが同じ値。SDK リファレンス Findings
	// 「Viewports」「Level Objects」#141 / #147）。
	bool CopySectionViewMatrix(MCObjectHandle viewport);

	// ビューポートを用紙の上で delta（用紙 mm）だけ動かす。注釈（データタグ）は
	// ビューポートと一緒に動く。
	//
	// ★**測ってから動かすまでの間にデータタグを置くこと**（順序を入れ替えてはならない）。
	// タグは「注釈へ置いた実位置を測って目標との差だけ動かす」作りで（draw/Tag の
	// MovePendingTags）、**その実測はビューポートが用紙のどこに在るかに影響される**。
	// 先にビューポートを動かしてからタグを置くと、動かした分だけタグが図からずれる
	// （M18 のローカル確認で実測。伏図・軸組図とも全タグが同じ向きへ外れていた）。
	void MoveViewportBy(MCObjectHandle viewport, const core::Vec2& delta);

	// 「命令インデックス → 描画したオブジェクトのハンドル」の対応表の**中身**。所有者
	// （draw/ObjectHandles.h の ObjectHandles）は SDK 非依存のヘッダに置いてあり、
	// SDK 型を持つこの定義だけがここに来る（そちらのヘッダ冒頭を参照）。
	struct ObjectHandleTable
	{
		std::map<std::size_t, MCObjectHandle> handles;
	};
} // namespace HomeskzIfcImport::draw
