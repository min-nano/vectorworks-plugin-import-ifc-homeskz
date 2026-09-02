//
//	core/Foundation.h
//
//	基礎を **1 つの立体オブジェクト**として描くための命令（FoundationCommand）と、その
//	部品からソリッド群を組み立てる純計算（docs/DEV-NOTES.md M20「基礎を独自 PIO へ」）。
//
//	【なぜ 1 つのオブジェクトか】M9〜M17 では立上りを壁・底盤をスラブ・地中梁をモディファイア
//	＋可視ソリッドと**別々の VW オブジェクト**で描いていた。噛み合わせ（スラブへ「足す」
//	モディファイア）は SDK から作れず（SDK リファレンス Findings「Slabs and Extrudes」）、
//	壁結合・端部のキャップ・呑み込みといった**接ぎ目を隠すための細工**が要素の数だけ要った。
//	M20 では基礎全体を**自作 PIO（Extensions/ExtFoundation）1 つ**にし、PIO が底盤・立上り・
//	地中梁・床付けを押し出しソリッドとして自分の中に描く。接ぎ目は同じ PIO の中の同素材の
//	ソリッドどうしなので、断面では構造用図形として一体に表示される。
//
//	【編集は PIO のパラメータで】取り込んだ後に OIP で 底盤厚・底盤天端・立上り幅・立上り
//	天端・地中梁せい・斜め部分の幅／高さ を変えられる（FoundationParams）。命令は部品 1 つ
//	ずつの実寸（取り込んだ IFC の値）と、その**代表値**（OIP に最初に出る値）を持ち、
//	PIO は「代表値との差」を各部品へ配る（applyFoundationParams）。実データは立上り幅が
//	120 / 150 / 300 と混在するので、値そのものを一律に置き換えると細部が失われる——
//	差を配れば、太い立上りは太いまま全体を厚くできる。
//
//	【ここが core にある理由】部品からソリッド（3D 多角形＋押し出しベクトル）を組み立てる
//	計算は SDK を触らない純計算で、PIO（SDK 側）と無 SDK テストの**両方**がこれを使う。
//	PIO が自分で幾何を計算すると、テストできない場所に基礎の形の知識が積もる。
//	同じ理由で **PIO が自分の中に部品を保存するための直列化**（encodeFoundation /
//	decodeFoundation）もここにある——PIO はパラメータが変わるたびに部品から描き直すので、
//	部品を PIO のレコードへ文字列で持たせる（フェーズ間の受け渡しは構造体のままで、
//	直列化は PIO の永続化だけに使う。CLAUDE.md「命令セット」）。
//
//	【SDK 非依存】このヘッダは core/Geometry.h と標準ライブラリだけに依存する。
//

#pragma once

#include "core/Geometry.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::core
{
	// --- 床付け（捨てコン・砕石）の既定値（M17）------------------------------------------
	//
	// 底盤の下は**砕石のみ**（厚みは捨てコン + 砕石ぶん＝kSlabBeddingThickness）、地中梁の下は
	// 捨てコン + 砕石、地中梁の傾斜部は法線方向に kSlabBeddingThickness の砕石。3 か所が同じ
	// 厚みを指すので定数は 1 つ（docs/DEV-NOTES.md「基礎の床付け」）。
	inline constexpr double kSlabLeanConcreteThickness = 30.0;
	inline constexpr double kSlabGravelThickness = 100.0;
	inline constexpr double kSlabBeddingThickness =
		kSlabLeanConcreteThickness + kSlabGravelThickness;

	// 外周部で床付けが地中梁の外側へはみ出す量（mm）。捨てコン・砕石とも同じだけ張り出す。
	inline constexpr double kBeddingPerimeterMargin = 50.0;

	// 地中梁の側面が外周部かを見分けるときに、その面のすぐ外側を突く距離（mm）。突いた点が
	// 底盤の外形の外なら外周部とみなす（面は外形の上に乗っているので、1mm 外せば足りる）。
	inline constexpr double kBeddingOutsideProbe = 1.0;

	// 地中梁の可視ソリッドを底盤へ呑み込ませる量（mm）。地中梁の天端は底盤の底面とちょうど
	// 接する（実データで確認: 天端 = 底盤天端 − 底盤厚）ため、地中梁だけを少し大きくして
	// 底盤本体に重ね、断面ビューポートで境界線が不安定に出るのを防ぐ（M10 で実機確認）。
	inline constexpr double kGroundBeamSlabBite = 10.0;

	// 地中梁の天端とみなす頂点の許容差（mm）。最大 v からこの差以内の頂点を天端の辺とみなす。
	// raiseBeamPrismTop と、その期待値を書くテストが共有する。
	inline constexpr double kModifierTopVertexTol = 0.5;

	// 基礎の寸法を「同じ」とみなす許容（mm）。代表値の集計（同じ厚みの底盤をまとめる）と
	// 地中梁断面の当てはめ（天端・下端の辺の判定）が共有する。
	inline constexpr double kFoundationTol = 1.0;

	// --- 地中梁のプリズム（断面 ＋ 水平押し出し）------------------------------------------

	// 地中梁の下に敷く床付け（捨てコン・砕石）1 区間。地中梁（BeamPrism）と**押し出しの向き
	// （azimuth）と断面の座標系を共有**して、断面と押し出し方向の区間だけが違う。
	//
	// 【なぜ区間に分かれるか】床付けは 1 本の地中梁の中でも断面が変わる。傾斜部を覆う砕石の
	// 帯は、**直交する地中梁と取り合う区間では相手のコンクリートへ食い込む**ので、その区間
	// だけ帯を切り下げる（docs/DEV-NOTES.md「基礎の床付け」）。切り下げる高さが同じ区間は
	// 1 つにまとめてあるので、取り合いの無い地中梁では区間は 1 つ（＝地中梁の全長）になる。
	//
	// フィールド:
	//   profile             … 断面の 2D 頂点列（u, v）。座標系は BeamPrism と同じ
	//   drawClass           … クラス名（構成要素の素材クラス。z構成要素-砕石 等）
	//   start               … 地中梁の origin から押し出し方向へ何 mm の位置から始まるか
	//   depth               … 押し出し長（mm。start + depth ≤ 地中梁の depth）
	struct BeddingPrism
	{
		std::vector<Vec2> profile;
		std::string drawClass;
		double start = 0.0;
		double depth = 0.0;
	};

	// 地中梁（台形断面プリズム）1 本。解析側が IFC の押し出しソリッドから取り出す中間表現で
	// あり、PIO が FoundationBeam（パラメータ化した断面）から描くときにも同じ形へ戻す
	// （beamPrism）。床付けの計算はこの形の上で行う。
	//
	// 【断面の座標系】profile は断面の 2D 頂点列 (u, v) で、u＝幅軸（押し出し方向を +90 度
	// 回した水平軸）・v＝鉛直軸（v=0 が断面原点＝梁下端）。origin は断面原点のワールド
	// 絶対座標（XY はセンタリング済み・z は絶対値）で、azimuth は押し出し方向（梁の走る
	// 向き）の方位角（度・+X から反時計回り）。**u 軸の取り方は復元規約と対で決まっている**
	// ので、片方だけ変えてはいけない（parse/Footing の groundBeamPrism と beamPrismAxes）。
	//
	// フィールド:
	//   profile             … 断面の 2D 頂点列（u, v）
	//   depth               … 押し出し長（軸方向。mm）
	//   origin              … 断面原点のワールド絶対座標（[x, y, z]）
	//   azimuth             … 押し出し方向の方位角（度）
	struct BeamPrism
	{
		std::vector<Vec2> profile;
		double depth = 0.0;
		Vec3 origin;
		double azimuth = 0.0;
	};

	// 地中梁の押し出し方向（axis）と幅軸（width＝axis を +90 度回した向き）の水平単位ベクトル。
	// **この復元規約がプリズムの u 軸の定義**で、解析側・PIO 側・テストがすべてこれを通る。
	void beamPrismAxes(const BeamPrism& prism, Vec2& axis, Vec2& width);

	// 地中梁の平面外形＝断面の u 範囲を軸方向へ depth だけ掃引した矩形（頂点 4 つ）。
	// 断面が空なら空。底盤への振り分けと、床付けの取り合い判定に使う。
	std::vector<Vec2> beamPrismFootprint(const BeamPrism& prism);

	// 地中梁の**可視ソリッド**用に、天端（profile の最大 v）を底盤側へ bite だけ持ち上げた
	// コピーを返す。地中梁は台形断面で側辺が斜めなので、天端頂点を**真上へ**上げると側面の
	// 勾配が変わる。そこで各天端頂点を隣接する側辺（下端側の頂点へ向かう斜辺）の延長線上へ
	// 動かす（v を bite 上げるのに合わせて u も勾配ぶんずらす）。側辺が見つからない／ほぼ
	// 水平な頂点は真上へ上げる。bite が 0 以下なら入力をそのまま返す。
	BeamPrism raiseBeamPrismTop(const BeamPrism& prism, double bite);

	// 地中梁 1 本の床付け（捨てコン・砕石）の断面を組み立てる（M17）。戻りは**上から**
	// （捨てコン → 砕石）で、地中梁の断面から床付けを求められない（天端／下端の辺が
	// 見つからない等）ときは空。start / depth は地中梁の全長で埋める（区間に切り分けるのは
	// foundationBeddings）。
	//
	// lowPerimeter / highPerimeter は断面の **u が小さい側／大きい側の側面が外周部か**。
	// 外周部の側面では床付けが側面から kBeddingPerimeterMargin だけ張り出して**そこで終わる**
	// （建物の外なので上へ回り込まない）。外周部でない側面は、傾斜・鉛直によらず側面を
	// kSlabBeddingThickness だけ法線方向へオフセットした砕石で覆う。
	//
	// 捨てコンは**下端の平らな面の下だけ**（傾斜部は砕石のみ）で、厚みは
	// kSlabLeanConcreteThickness。砕石はその残り全部。topLimit は**断面を切り上げる高さ**
	// （断面座標 v）。切る必要が無ければ天端の v 以上を渡す。切った結果が面にならない層は落とす。
	// leanClass / gravelClass は各層に付ける素材クラス名。
	std::vector<BeddingPrism> groundBeamBedding(const BeamPrism& prism, bool lowPerimeter,
												bool highPerimeter, double topLimit,
												const std::string& leanClass,
												const std::string& gravelClass);

	// --- 基礎の部品（取り込んだ IFC の実寸）--------------------------------------------

	// 底盤 1 枚。平面外形（センタリング済み・末尾に始点を重複させない）と、コンクリートの
	// 天端の絶対 Z・厚み。底盤の下には kSlabBeddingThickness の砕石を敷く（描画側が足す）。
	struct FoundationSlab
	{
		std::vector<Vec2> boundary;
		double top = 0.0;
		double thickness = 0.0;
	};

	// 立上り（基礎梁）1 本。壁芯の始点・終点（センタリング済み）と幅、下端・天端の絶対 Z。
	// 人通口で分割・切り下げ済み（天端が低い区間は別の部品になっている。parse/Footing）。
	struct FoundationRiser
	{
		Vec2 start;
		Vec2 end;
		double width = 0.0;
		double bottom = 0.0;
		double top = 0.0;
	};

	// 地中梁（下り梁）1 本。**下端の中心線**（start → end。センタリング済み）と、断面の
	// パラメータで表す。断面は下端（幅 bottomWidth）から上へ、鉛直部 → 斜め部（haunch）と
	// 広がって天端（＝底盤の底面）に至る台形:
	//
	//        ┌──────────────────┐  ← 天端 top（底盤の底面）
	//       ／                    ＼      斜め部: 高さ haunchHeight・片側の張り出し
	//      ／                      ＼     haunchLeft（+u 側）／ haunchRight（−u 側）
	//      │                        │    鉛直部: depth − haunchHeight（無ければ 0）
	//      └────────────────────────┘  ← 下端 top − depth（幅 bottomWidth）
	//
	// 実データの断面は 4 頂点の台形（斜め部が全高＝haunchHeight == depth）か矩形（張り出し
	// 0）で、外周の地中梁は外側の面が鉛直（片側だけ張り出す）。鉛直部を持つ 6 頂点の断面も
	// 同じ形で表せる。u 軸は BeamPrism と同じ（押し出し方向を +90 度回した向き＝進行方向の
	// 左が +u）。
	struct FoundationBeam
	{
		Vec2 start;
		Vec2 end;
		double bottomWidth = 0.0;
		double haunchLeft = 0.0;
		double haunchRight = 0.0;
		double haunchHeight = 0.0;
		double top = 0.0;
		double depth = 0.0;
	};

	// FoundationBeam（パラメータ化した断面）→ BeamPrism（頂点列の断面）。断面原点は下端の
	// 中心（origin.z = top − depth）。張り出しの無い側の斜め部の頂点は作らない（重複点を
	// 出さない）。長さ 0 の地中梁は空の断面を返す。
	BeamPrism beamPrism(const FoundationBeam& beam);

	// BeamPrism（頂点列の断面）→ FoundationBeam（パラメータ化した断面）。天端（v 最大）の辺と
	// 下端（v 最小）の辺を見つけ、下端の幅・両側の張り出し・せいを読む。斜め部の高さは
	// 「下端の角から天端へ向かう側辺に、下端と同じ u の中間頂点（鉛直部の上端）があれば
	// その高さから上」、無ければ全高。天端か下端の辺が見つからない断面（三角形・水平な板）は
	// 外接矩形で近似する（下端幅＝u の幅・張り出し 0・せい＝v の幅）。断面が 3 点未満／
	// 押し出し長 0 なら false。
	bool fitFoundationBeam(const BeamPrism& prism, FoundationBeam& out);

	// OIP で編集できる寸法（PIO のパラメータと 1 対 1）。命令が持つのは**代表値**——取り込み
	// 時の部品から最も多い値（面積／長さで重み付け）を採ったもので、OIP に最初に出る値。
	//
	//   slabTop        … 底盤天端の高さ（GL からの絶対 Z。mm）
	//   slabThickness  … 底盤のコンクリート厚
	//   riserWidth     … 立上りの幅
	//   riserTop       … 立上り天端の高さ（GL からの絶対 Z）
	//   beamDepth      … 地中梁のせい（底盤の底面から梁下端まで）
	//   haunchWidth    … 地中梁の斜め部分の片側の張り出し幅
	//   haunchHeight   … 地中梁の斜め部分の高さ
	struct FoundationParams
	{
		double slabTop = 0.0;
		double slabThickness = 0.0;
		double riserWidth = 0.0;
		double riserTop = 0.0;
		double beamDepth = 0.0;
		double haunchWidth = 0.0;
		double haunchHeight = 0.0;
	};

	// 基礎全体を 1 つの PIO として描く命令（docs/DEV-NOTES.md M20）。draw/Footing がこれを
	// 自作 PIO（Extensions/ExtFoundation）へ変換する。
	//
	// フィールド:
	//   layer              … PIO を置くデザインレイヤ名（"F-基礎"。基礎ストーリの GL レベル＝
	//                        高さ 0 のレイヤ。**部品の Z は GL 基準の絶対値**なので、レイヤの
	//                        高さが 0 でなければならない）
	//   drawClass          … PIO 本体のクラス（基礎スラブ。予約語 class を機械置換）
	//   slabClass          … 底盤・地中梁のソリッドのクラス（基礎スラブ）
	//   riserClass         … 立上りのソリッドのクラス（立ち上がり）
	//   leanConcreteClass  … 床付けの捨てコンのクラス（z構成要素-捨てコンクリート）
	//   gravelClass        … 床付けの砕石のクラス（z構成要素-砕石）
	//   slabs / risers / beams … 部品（取り込んだ IFC の実寸）
	//   params             … 代表値（OIP に最初に出る値。foundationBaseParams）
	//
	// 【ソリッドの素材クラスは命令が持つ】PIO は parse/ を include できない（依存の向き）
	// ので、クラス名は命令に載せて PIO のレコードにも一緒に保存する。
	struct FoundationCommand
	{
		std::string layer;
		std::string drawClass;
		std::string slabClass;
		std::string riserClass;
		std::string leanConcreteClass;
		std::string gravelClass;
		std::vector<FoundationSlab> slabs;
		std::vector<FoundationRiser> risers;
		std::vector<FoundationBeam> beams;
		FoundationParams params;
	};

	// 部品から代表値を求める。底盤は**面積**、立上り・地中梁は**長さ**で重み付けして最も多い
	// 値を採る（同率なら大きい値）。斜め部の幅・高さは張り出しのある側だけを数える。部品が
	// 無い項目は 0。結果は入力順に依存しない（決定的）。
	FoundationParams foundationBaseParams(const FoundationCommand& command);

	// 代表値の変更を各部品へ配る。imported は取り込み時の部品と代表値（params）を持つ命令で、
	// edited が OIP の現在値。戻りは部品を動かした命令（params は edited）。
	//
	//   * 底盤 … 天端 += Δ天端、厚み += Δ厚
	//   * 立上り … 幅 += Δ幅、天端 += Δ天端、**下端 += Δ(底盤の底面)**（立上りは底盤と一体に
	//     打つので、底盤が厚く・低くなれば立上りの根元も一緒に下がる）
	//   * 地中梁 … 天端 += Δ(底盤の底面)（天端＝底盤の底面）、せい += Δせい、**張り出しの
	//     ある側だけ** += Δ張り出し（鉛直な面は鉛直のまま）、斜め部の高さ += Δ高さ
	//     （0〜せい にクランプ）
	// 差を配るので、太い立上り・深い地中梁はその差を保ったまま全体が動く。
	FoundationCommand applyFoundationParams(const FoundationCommand& imported,
											const FoundationParams& edited);

	// --- ソリッドの組み立て（PIO が描くもの）--------------------------------------------

	// 押し出しソリッド 1 つ。base は**閉じた 3D 多角形**（末尾に始点を重複させない）、extent
	// は押し出しベクトル（鉛直なら (0,0,厚み)・地中梁なら 軸方向×長さ）。描画側は base を
	// 3D ポリゴンにして |extent| だけ押し出す（法線の向きは描画側が揃える）。
	struct FoundationSolid
	{
		enum class Kind
		{
			Slab,  // 底盤のコンクリート
			Riser, // 立上り
			Beam, // 地中梁（天端を kGroundBeamSlabBite だけ底盤へ呑み込ませてある）
			Bedding, // 床付け（底盤の下の砕石・地中梁の下の捨てコンと砕石）
		};
		Kind kind = Kind::Slab;
		std::string drawClass;
		std::vector<Vec3> base;
		Vec3 extent;
	};

	// 平面（2D/平面ビュー）に描く外形 1 つ。閉じた多角形（末尾に始点を重複させない）。
	struct FoundationPlanShape
	{
		FoundationSolid::Kind kind = FoundationSolid::Kind::Slab;
		std::string drawClass;
		std::vector<Vec2> outline;
	};

	// 各地中梁がどの底盤に付くか（beams と同じ並びで底盤の添字。どの底盤にも入らなければ
	// 重心が最も近い底盤へフォールバック。底盤が 1 枚も無ければすべて npos）。代表点
	// （重心・各頂点・各辺の中点）が外形内に入る数が最大の底盤を選ぶ。入力順に対して決定的。
	std::vector<std::size_t> attachBeamsToSlabs(const std::vector<FoundationSlab>& slabs,
												const std::vector<BeamPrism>& beams);

	// 地中梁ごとの床付け（beams と同じ並び。求められない地中梁は空）。断面は beamPrism
	// （呑み込み前の実形状）の座標系。ここが決めるのは断面そのものではなく、**断面を組み
	// 立てる条件**の 3 つ: 外周部かどうか（付いた底盤の外形で判定）・帯を切り上げる高さ
	// （底盤の砕石の底）・区間ごとの切り下げ（平面外形が掛かる他の地中梁の下端まで）。
	std::vector<std::vector<BeddingPrism>> foundationBeddings(const FoundationCommand& command);

	// 命令（部品）からソリッド群を組み立てる。並びは 底盤 → 底盤の下の砕石 → 立上り →
	// 地中梁 → 床付け で決定的。実体にならない部品（厚み・幅・長さが 0 以下、外形 3 点未満）
	// は落とす。地中梁は天端を kGroundBeamSlabBite だけ底盤へ呑み込ませる（床付けは呑み込
	// ませない——接する相手が別素材なので境界線が出てよい）。
	std::vector<FoundationSolid> foundationSolids(const FoundationCommand& command);

	// 平面に描く外形（底盤の外形・立上りの平面矩形・地中梁の天端幅の矩形）。並びは底盤 →
	// 立上り → 地中梁。
	std::vector<FoundationPlanShape> foundationPlanShapes(const FoundationCommand& command);

	// --- PIO のレコードへ保存するための直列化 ------------------------------------------

	// 命令の部品・素材クラス・代表値を 1 行の文字列にする（layer / drawClass は含めない——
	// PIO 自身の置き場所とクラスは VW が持つ）。改行を含まず、";" で項目・" " で値を区切る。
	// 数値は小数 3 桁まで（0.001mm の粒度で丸める。図面の寸法としては同一）。
	std::string encodeFoundation(const FoundationCommand& command);

	// encodeFoundation の逆。書式が違う（版が違う・壊れている）なら false（out は変更しない）。
	bool decodeFoundation(const std::string& text, FoundationCommand& out);
} // namespace HomeskzIfcImport::core
