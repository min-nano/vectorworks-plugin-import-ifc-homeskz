//
//	draw/DrawUtil.h
//
//	Phase 2（VW 描画）の共通ヘルパー。要素ごとの draw モジュール（Grid / Story / Floor /
//	Rafter / Roof …）が等しく行う定型——クラス分け・描画属性の by-class 化・配置先レイヤの
//	用意——を 1 か所に集める。
//
//	【なぜ要るか】これらは以前、各 .cpp の無名名前空間に**逐語的な複製**として置かれていた
//	（SetClassByName は 4 か所、SetAllAttributesByClass は 3 か所）。属性を 1 つ足す・
//	by-class の指定を直すといった変更が、直した .cpp でしか効かない形になっていた。
//
//	【SDK 依存・include の順序】このヘッダは draw/StructuralMember.h とともに**SDK 型を
//	公開する共通ヘッダ**（MCObjectHandle を引数に取るため）。自分で PluginPrefix.h を
//	include するので、draw/*.cpp のどこから include しても成立する。逆に、要素ごとの
//	draw/*.h は従来どおり core::Document.h までしか参照しない（SDK を持たない翻訳単位＝
//	Extensions/ExtMenu から安全に include できるようにするため。CLAUDE.md「依存の向きは
//	厳守する」）。したがって**このヘッダを draw/*.h から include してはならない**。
//

#pragma once

#include "PluginPrefix.h"

#include "core/Document.h"

#include "VWFC/VWObjects/VWParametricObj.h"

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	// オブジェクトのクラスを名前で設定する（Python 版の vs.SetClass(handle, class) に対応）。
	// AddClass は既存なら索引を返し、無ければクラスを作る。クラス名が空なら何もしない
	// （無クラス＝既定クラスのまま）。
	void SetClassByName(MCObjectHandle object, const std::string& className);

	// 描画属性（線幅・色・パターン・矢印・透明度）をすべてクラス属性に従わせる。
	// SetObjectClass はクラスを割り当てるだけで各属性は by-instance の既定値のまま残るため、
	// 属性ごとに by-class を指定する（Python 版 _set_all_attributes_by_class と同じ意図。
	// ISDK の関数名は VS と異なる: PColors=ペン色 / FColors=面色 / PPat=線種 /
	// FPat=面パターン / Arrow=マーカー）。
	void SetAllAttributesByClass(MCObjectHandle object);

	// PIO のパラメータ名を解決する。universal 名で見つかればそれを使い、見つからなければ
	// ローカライズ名（OIP に出る日本語）で引き直す。**名前が 1 つ違うだけで setter は黙って
	// 無視される**（M6 の垂木で実証済み: 勾配・構造用途・ラベルが名前違いで効いていなかった）
	// ため、確実に見つかる方を選ぶ。どちらでも見つからなければ universal 名をそのまま返す。
	TXString ResolveParamName(const VWParametricObj& pio, const char* universalName,
							  const char* localizedName);

	// PIO に実数パラメータを書き、**読み戻して書けたか確かめる**。書けていれば true。
	// 角度・寸法のような数値パラメータでも、PIO の登録次第で実数ではなく文字列として
	// 保持されていることがあり、その場合 SetParamReal は黙って無視される。そこで実数で
	// 書けなかったときは文字列で入れ直す（M6 の垂木で「寸法を文字列で渡すと既定値のまま
	// だった」逆のケースが起きており、どちらに転んでも入るようにする）。
	bool SetParamRealChecked(VWParametricObj& pio, const TXString& param, double value,
							 double tolerance = 1e-6);

	// 構造材ツール（StructuralMember）へ渡す**断面プロファイルのグループ**を作る。
	// 矩形（[minX, minY]〜[maxX, maxY]）1 枚を閉じたポリゴンとしてグループへ入れて返す
	// （Python 版の BeginGroup / ClosePoly / Poly(…) / EndGroup に対応）。矩形の位置は
	// 呼び出し側の**断面基準点の規約**で決まる: 横架材は天端中央基準なので原点が上辺中央、
	// 柱は断面中心基準なので原点が中心（AxisAlign の設定と一致させる）。
	//
	// **グループへは VWFC の VWGroupObj::AddObject で入れる**。gSDK->AddObjectToContainer を
	// 直に呼ぶと「レイヤに作ってから移す」形になり、移動に失敗すると**空のグループ**が残る。
	// 空のプロファイルは断面が無いのと同じで、PIO は生成できても実体が描かれない（＝
	// オブジェクトはあるのに画面に何も出ない）。入ったかどうかを GetFirstMemberObject で
	// 確かめ、空なら nil を返すので、呼び出し側はフォールバックへ回せる。
	// 幅・せいが 0 以下なら nil。
	MCObjectHandle CreateRectangleProfileGroup(double minX, double minY, double maxX, double maxY);

	// 名前付きプラグインスタイル（"木質構造材_横架材" 等）の RefNumber を引く。文書に無ければ
	// 0 を返す（＝スタイル無しで描く。スタイルの欠落で部材を失わない）。
	//
	// ISDK はスタイル名から RefNumber を引く呼び出しを持たないので、名前付きオブジェクト
	// （プラグインスタイルはシンボル定義）を GetNamedObject で引き、その InternalIndex を
	// RefNumber として渡す（どちらも SysName を表す Sint32。SDK ヘッダでも InternalIndex と
	// RefNumber は相互に渡し合う形で使われている）。
	RefNumber ResolvePluginStyle(const TXString& styleName);

	// 平面外形を閉じた 2D ポリゴンとして作る（スラブのプロファイル・フォールバック描画）。
	// 頂点が空なら nil。
	MCObjectHandle CreateClosedPolygon(const std::vector<core::Vec2>& boundary);

	// --- スラブ（床板＝M5・基礎の底盤＝M9 が共有する作法）--------------------------
	//
	// 床（draw/Floor）と底盤（draw/Footing）は**同じ手順**でスラブを描く（外形ポリゴン →
	// CreateSlab → クラス → スタイル（構成層・基準面）→ SetSlabHeight → バインド →
	// ResetObject）。違うのはスタイル名と構成層の中身だけなので、SDK を叩く部分はここに
	// 1 つだけ置く（かつては draw/Floor.cpp の無名名前空間にあり、底盤が同じものを 2 つ目に
	// 書く形になっていた）。

	// オブジェクト（スラブ本体／スラブスタイル）の構成層を命令どおりに作り直す。
	//
	// 手順: **先頭に命令の層を順に挿入し、その後ろに残った元の層を削除する**。
	//   * 厚み 0 の層は VW が受け付けない（＝「潰す」では構成を確定できない）ので、
	//     余った層は必ず削除する。
	//   * 先に挿入してから削除するので、途中で層が 0 枚になる瞬間が無い
	//     （層が 1 枚も無いスラブ／スタイルは作れない）。
	//   * 削除に失敗したら（想定外の API 挙動）そこで打ち切り、元の層が残ったままでも
	//     スラブ自体は残す（1 枚の失敗で全体を止めない）。
	//
	// ★コンポーネントの索引は **0 始まり**（実機で確認: 索引 1 に挿入すると既定層の後ろへ
	// 入り、索引 = 層数 で削除すると範囲外で失敗した）。GetNumberOfComponents が返すのは
	// 「個数」なので、有効な索引は 0 … 個数−1。
	void SetComponents(MCObjectHandle object,
					   const std::vector<core::ComponentCommand>& components);

	// スラブ（またはスラブスタイル）の高さ基準面を設定する。基準面は「どの構成要素か」
	// （SetDatumSlabComponent）＋「その上端か下端か」（SetComponentDatumIsTopOfComponent）の
	// 2 つで決まる。スラブスタイル設定ダイアログの「基準面」欄のポップアップとラジオが
	// それぞれこの 2 つに対応する。
	//   Top    … 最上層（索引 0）の**上端**＝スラブ天端
	//   Bottom … 最下層（索引 個数−1）の**下端**＝スラブ底面
	void SetSlabDatum(MCObjectHandle object, core::SlabDatum datum, short componentCount);

	// 名前付きのスラブスタイルを用意して索引を返す。既にあればそれを使い、無ければ作る。
	// 構成層と基準面は毎回命令どおりに更新する（再インポートで構成が変わっても追従する）。
	// 用意できなければ 0（＝スタイル無し。呼び出し側はスラブ本体へ直接構成を組む）。
	//
	// ★**既存のスタイルを見つけたときはその構成層を作り直す**（＝上書きする）。ドキュメントの
	// テンプレートに同名のスタイルがあると、そこで設定済みのクラス・マテリアル・用途が既定値へ
	// 戻る。上書きされては困る用途では CreateUniqueSlabStyle を使うこと（基礎の底盤はそちら。
	// 床＝draw/Floor は階ごとに構成（床仕上げ厚）を計算して当てる必要があるため現状こちら）。
	InternalIndex ResolveSlabStyle(const std::string& styleName,
								   const std::vector<core::ComponentCommand>& components,
								   core::SlabDatum datum);

	// **既存のリソースには一切触れずに**新しいスラブスタイルを作って索引を返す。baseName が
	// 既に使われていれば " (2)" … と連番を付けて空いている名前を探す（上書き事故が構造的に
	// 起きない）。構成層と基準面は命令どおりに設定する。作れなければ 0。
	//
	// 実際に使った名前は outName に入る（同じ命令スタイル名の底盤どうしで 1 つのスタイルを
	// 共有できるよう、呼び出し側が対応表に覚えるため）。
	InternalIndex CreateUniqueSlabStyle(const std::string& baseName,
										const std::vector<core::ComponentCommand>& components,
										core::SlabDatum datum, std::string* outName = nullptr);

	// 同じく**既存のリソースには一切触れずに**新しいウォールスタイルを作って索引を返す。
	// 名前の空き探しと構成層の設定はスラブ版と同じで、基準面（datum）を持たない点だけが違う
	// （壁は構成層の合計がそのまま壁厚になる）。作れなければ 0。
	InternalIndex CreateUniqueWallStyle(const std::string& baseName,
										const std::vector<core::ComponentCommand>& components,
										std::string* outName = nullptr);

	// --- 取り込み全体の Undo（ROADMAP.md M15）--------------------------------------
	//
	// 【なぜレイヤを記録するのか】VectorWorks は取り込みの開始時に undo イベントを開かない
	// （実機の診断ログで確認。ROADMAP.md M15）。そこで**自分でイベントを開き**、
	// `AddAfterSwapObject` で「あとで消してよいもの」を登録する。登録するのは
	// **このインポートが新しく作ったレイヤだけ**——レイヤを消せばその上の図形も消えるので、
	// 図形を 1 つずつ登録する必要は無く、**二重登録（レイヤと中身の両方）で undo が既に
	// 消えたものを消しにいく事故**も避けられる。
	//
	// 取り込みが作らないもの（クラス・スラブ／ウォールスタイル・ストーリ・レベル
	// テンプレート）はリソースであり、undo では戻らない。空のクラスやスタイルが残るが、
	// 図面の見た目は取り込み前に戻る。
	//
	// 【既にあったレイヤ】2 回目の取り込みのように、**取り込み前から在ったレイヤ**へ描いた
	// 分は登録できない（そのレイヤごと消すわけにいかない）。その場合は取り消しが部分的に
	// なるので、完了ダイアログでその旨を伝える（ImportUndoScope::partial）。

	// 取り込みの図面変更をまるごと包む undo イベント。**構築で開き、破棄で閉じる**
	// （途中で例外が出ても閉じる）。記録が 1 件も無ければ、閉じるときにイベントごと
	// 捨てる——空のイベントを残すと「取り消し」が中途半端に効いて図面が壊れるため
	// （実機で確認。ROADMAP.md M15）。
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

		// **取り込み前から在ったレイヤ**へも描いたか（＝取り消しはその分だけ戻らない）。
		bool partial() const
		{
			return fUsedExistingLayer;
		}

	private:
		// 記録の実体はスコープが持ち、下の 2 つの自由関数が「いま開いているスコープ」を
		// 通して書き込む（要素側の draw モジュールはスコープを持ち回らずに済む。
		// インポートはメインスレッドから 1 本しか走らないので、開いているスコープは高々 1 つ）。
		friend void RecordCreatedLayer(MCObjectHandle layer);
		friend void NoteExistingLayerUsed(MCObjectHandle layer);

		bool contains(MCObjectHandle layer) const;

		std::vector<MCObjectHandle> fCreatedLayers; // このインポートが作ったレイヤ
		bool fUsedExistingLayer = false; // 取り込み前から在ったレイヤへも描いた
	};

	// このインポートが新しく作ったレイヤを undo イベントへ登録する（デザイン／シートの
	// どちらも）。イベントが開いていなければ何もしない。nil は無視。
	void RecordCreatedLayer(MCObjectHandle layer);

	// 取り込み前から在ったレイヤへ描いたことを控える（取り消しが部分的になる）。
	// レイヤを用意するヘルパー（下記 3 つ）が自分で呼ぶので、要素側は意識しなくてよい。
	void NoteExistingLayerUsed(MCObjectHandle layer);

	// 名前付きデザインレイヤを取得（無ければ作成）してアクティブにする。以後に生成する
	// オブジェクトはこのレイヤへ入る。取得・生成できなければ nil を返し、カレントレイヤも
	// 変えない。**通り芯の "共通" レイヤのように、その要素が自分で用意してよいレイヤ専用。**
	MCObjectHandle PrepareLayer(const std::string& layerName);

	// 既存の名前付きデザインレイヤをアクティブにする。**存在しなければ何もせず nil**
	// （レイヤを作らない）。ストーリ由来のレイヤ（"1-FL" / "n-垂木" / "n-野地板"）は
	// story 命令が作るので、無い＝そのストーリの生成がスキップされたということ。要素の
	// ために勝手にレイヤを作らない（Python 版 execute_floors / execute_rafters /
	// execute_roofs と同じ規約）。
	MCObjectHandle ActivateExistingLayer(const std::string& layerName);

	// --- シートレイヤとビューポート（伏図＝M13・軸組図＝M14 が共有する作法）------------
	//
	// 伏図（draw/Sheet）と軸組図（draw/Section）は、ビューポートの**種類が違うだけ**で
	// 前後の手当ては同じ（シートレイヤを用意 → 生成 → 表示レイヤを絞る → クラスを表示に
	// 戻す → 縮尺 → 図面タイトル・図番 → 更新）。SDK を叩く部分はここに 1 つだけ置く。
	// 断面ビューポートも例外ではなく、ISDK::CreateSectionViewport のコメントが
	// 「クラス・レイヤの表示はこの呼び出しでは扱わない。呼び出し後に設定し、そのあとで
	// ビューポートを更新すること」と明記している。

	// ビューポート共通の下ごしらえ。図面の全レイヤと、**表示に戻すクラス**の索引を持つ。
	//
	// 【クラスをわざわざ数え上げる理由】ビューポートはクラスの表示を明示しないと**非表示の
	// まま**（M13 のローカル確認で判明。レイヤは命令どおりなのに図形が 1 つも出なかった）。
	// ところが ISDK には「ドキュメントの全クラスを列挙する」呼び出しが無い（VWClass にあるのは
	// 名前↔索引の変換だけ）。そこで**図形が身に付けているクラス**を全レイヤ走査で数え上げ、
	// 命令セットが名乗るクラス（core::documentClassNames）も取りこぼし防止に足す。
	//
	// classes は**昇順・重複なしの vector**（集合として使うが std::set では持たない）。
	// Windows の clang-tidy が std::set を持つ構造体の暗黙の特殊メンバに
	// bugprone-exception-escape を出すため、走査中だけ set を使い、結果は vector へ移す
	// （用途は「1 つずつ表示へ戻す」走査だけなので、連続領域の方が素直でもある）。
	struct ViewportSetup
	{
		std::vector<MCObjectHandle> layers;
		std::vector<InternalIndex> classes;
	};

	// 上の下ごしらえを行う。図面の規模なりに走査するので、**ビューポートを作るフェーズごとに
	// 1 回だけ**呼ぶこと（伏図・軸組図がそれぞれ 1 回。フェーズをまたいで持ち回さないのは、
	// 要素ごとの draw/*.h に SDK 型を出さない約束を守るため。DrawUtil.h 冒頭参照）。
	ViewportSetup PrepareViewportSetup(const core::Document& document);

	// シートレイヤを用意する（同じ番号のものがあれば再利用）。**シートレイヤ番号はレイヤ名が
	// 担う**（Python 版と同じ）。タイトルはレイヤの説明＝オブジェクト変数 159
	// （ovLayerDescription。"only used for sheet layers"）へ入れる。用意できなければ nil。
	MCObjectHandle PrepareSheetLayer(const std::string& number, const std::string& title);

	// オブジェクト（PIO・グループ・シンボル）が**中身も含めて**身に付けているクラスを
	// 昇順・重複なしで返す。**後から注釈へ足した図形のクラスを表示へ戻す**のに使う
	// ——PrepareViewportSetup はデザインレイヤしか走査しないので、ビューポート注釈に
	// 置いたデータタグ（とスタイルが決めるその中身）のクラスは数え上げに入らない
	// （ローカル確認で「タグに含まれるクラスが非表示」と判明。draw/Tag）。
	std::vector<InternalIndex> CollectObjectClasses(MCObjectHandle object);

	// ビューポートで指定のクラスを表示へ戻す（戻せた数を返す）。ConfigureViewport が
	// 使うのと同じ規約で、**表示種別の値をここ 1 か所に閉じ込める**ためのもの。
	std::size_t ShowViewportClasses(MCObjectHandle viewport,
									const std::vector<InternalIndex>& classes);

	// 生成済みのビューポートを命令どおりに仕上げる（表示レイヤの絞り込み → クラス表示 →
	// 縮尺 → 図面タイトル・図番 → 更新）。**表示に戻せたクラスの数**を返す（0 なら図形が
	// 1 つも映らないので、呼び出し側は診断行に出す）。
	//
	// 表示レイヤは「まず全部隠してから、命令に挙げたものだけ表示へ戻す」——ビューポートは
	// 既定でドキュメントの表示状態を引き継ぐため、挙げていないレイヤが映り込む。グレー表示
	// （2）は薄く残るので使わず、必ず非表示（1）にする。
	std::size_t ConfigureViewport(MCObjectHandle viewport, MCObjectHandle sheetLayer,
								  const ViewportSetup& setup, const core::ViewportCommand& command);

	// 「命令インデックス → 描いたオブジェクトのハンドル」の対応表の**中身**。所有者
	// （draw/ObjectHandles.h の ObjectHandles）は SDK 非依存のヘッダに置いてあり、
	// SDK 型を持つこの定義だけがここに来る（そちらのヘッダ冒頭を参照）。
	struct ObjectHandleTable
	{
		std::map<std::size_t, MCObjectHandle> handles;
	};
} // namespace HomeskzIfcImport::draw
