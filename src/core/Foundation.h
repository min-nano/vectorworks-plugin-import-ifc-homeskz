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
//	【部品は「同一仕様ごとの外形多角形」】部品 1 つずつを芯線＋幅で持つのはやめ、**同じ仕様の
//	ものをまとめて外形の多角形**で持つ:
//	  * 底盤 … **厚さと天端の高さ**が同じものを 1 グループにし、外形を持つ
//	  * 立上り … **天端の高さ**が同じものを 1 グループにし、**天端の面**の外形を持つ
//	    （芯線＋幅ではないので、L 字・T 字・斜めの複雑な形もそのまま表せる）
//	  * 地中梁 … **底の高さと斜め寸法**が同じものを 1 グループにし、**底の面**の外形を持つ
//	多角形なら頂点を動かすだけで形を直せるので、取り込んだ後の編集が素直になる（芯線＋幅では
//	「幅を変える」しかできない）。
//
//	【高さは底盤に取り合う】立上りの下端と地中梁の天端は**その真下／真上に来る底盤の底面**から
//	決める（foundationSlabBottom）。基礎は一体に打つので、底盤を厚くすれば立上りの根元も
//	地中梁の頭も一緒に動くのが正しい。命令が持たない値をここで決めるので、OIP で底盤厚を
//	変えたときに取り合いが崩れない。
//
//	【編集は PIO のパラメータで】取り込んだ後に OIP で 底盤厚・底盤天端・立上り天端・
//	地中梁せい・斜め部分の幅／高さ を変えられる（FoundationParams）。命令はグループごとの
//	実寸（取り込んだ IFC の値）と、その**代表値**（OIP に最初に出る値）を持ち、PIO は
//	「代表値との差」を各グループへ配る（applyFoundationParams）。実データは高さの違う底盤が
//	混在するので、値そのものを一律に置き換えると細部が失われる——差を配れば、深い地中梁は
//	深いまま全体を動かせる。
//
//	【ここが core にある理由】部品からソリッド（3D 多角形＋押し出しベクトル）を組み立てる
//	計算は SDK を触らない純計算で、PIO（SDK 側）と無 SDK テストの**両方**がこれを使う。
//	PIO が自分で幾何を計算すると、テストできない場所に基礎の形の知識が積もる。
//	同じ理由で **PIO が自分の中に部品を保存するための直列化**（encodeFoundation /
//	decodeFoundation）もここにある——PIO はパラメータが変わるたびに部品から描き直すので、
//	部品を PIO のレコードへ文字列で持たせる（フェーズ間の受け渡しは構造体のままで、
//	直列化は PIO の永続化だけに使う。CLAUDE.md「命令セット」）。
//
//	【SDK 非依存】このヘッダは core/Geometry.h / core/PolygonBool.h と標準ライブラリだけに
//	依存する。
//

#pragma once

#include "core/Geometry.h"
#include "core/PolygonBool.h"

#include <cstddef>
#include <string>
#include <vector>

namespace HomeskzIfcImport::core
{
	// --- 床付け（捨てコン・砕石）の既定値（M17）------------------------------------------
	//
	// 底盤の下は**砕石のみ**（厚みは捨てコン + 砕石ぶん＝kSlabBeddingThickness）、地中梁の下は
	// 捨てコン + 砕石、地中梁の側面は法線方向に kSlabBeddingThickness の砕石。3 か所が同じ
	// 厚みを指すので定数は 1 つ（docs/DEV-NOTES.md「基礎の床付け」）。
	inline constexpr double kSlabLeanConcreteThickness = 30.0;
	inline constexpr double kSlabGravelThickness = 100.0;
	inline constexpr double kSlabBeddingThickness =
		kSlabLeanConcreteThickness + kSlabGravelThickness;

	// 外周部で床付けが地中梁の外側へはみ出す量（mm）。捨てコン・砕石とも同じだけ張り出す。
	inline constexpr double kBeddingPerimeterMargin = 50.0;

	// 地中梁の側面が外周部か（他の地中梁と取り合うか）を見分けるときに、その面のすぐ外側を
	// 突く距離（mm）。面は外形の上に乗っているので、1mm 外せば足りる。
	inline constexpr double kBeddingOutsideProbe = 1.0;

	// 地中梁の可視ソリッドを底盤へ呑み込ませる量（mm）。地中梁の天端は底盤の底面とちょうど
	// 接する（実データで確認: 天端 = 底盤天端 − 底盤厚）ため、地中梁だけを少し大きくして
	// 底盤本体に重ね、断面ビューポートで境界線が不安定に出るのを防ぐ（M10 で実機確認）。
	inline constexpr double kGroundBeamSlabBite = 10.0;

	// 地中梁の天端とみなす頂点の許容差（mm）。最大 v からこの差以内の頂点を天端の辺とみなす。
	inline constexpr double kModifierTopVertexTol = 0.5;

	// 基礎の寸法を「同じ」とみなす許容（mm）。代表値の集計（同じ厚みの底盤をまとめる）と
	// 地中梁断面の当てはめ（天端・下端の辺の判定）が共有する。
	inline constexpr double kFoundationTol = 1.0;

	// --- 地中梁のプリズム（解析が IFC の押し出しから断面を読み取るための中間表現）--------

	// 地中梁（台形断面プリズム）1 本。解析側が IFC の押し出しソリッドから取り出す中間表現で、
	// ここから**底の面の外形と斜め寸法**（FoundationBeamFit）へ当てはめてグループに畳む。
	//
	// 【断面の座標系】profile は断面の 2D 頂点列 (u, v) で、u＝幅軸（押し出し方向を +90 度
	// 回した水平軸）・v＝鉛直軸（v=0 が断面原点＝梁下端）。origin は断面原点のワールド
	// 絶対座標（XY はセンタリング済み・z は絶対値）で、azimuth は押し出し方向（梁の走る
	// 向き）の方位角（度・+X から反時計回り）。**u 軸の取り方は復元規約と対で決まっている**
	// ので、片方だけ変えてはいけない（parse/Footing の groundBeamPrism と beamPrismAxes）。
	struct BeamPrism
	{
		std::vector<Vec2> profile;
		double depth = 0.0;
		Vec3 origin;
		double azimuth = 0.0;
	};

	// 地中梁の押し出し方向（axis）と幅軸（width＝axis を +90 度回した向き）の水平単位ベクトル。
	// **この復元規約がプリズムの u 軸の定義**で、解析側・テストがすべてこれを通る。
	void beamPrismAxes(const BeamPrism& prism, Vec2& axis, Vec2& width);

	// 地中梁 1 本の断面を「下端の幅・斜め部の張り出しと高さ・せい」へ当てはめた形。断面は
	// 下端（幅 bottomWidth）から上へ、鉛直部 → 斜め部（haunch）と広がって天端（＝底盤の
	// 底面）に至る台形:
	//
	//        ┌──────────────────┐  ← 天端 top（底盤の底面）
	//       ／                    ＼      斜め部: 高さ haunchHeight・片側の張り出し
	//      ／                      ＼     haunchLeft（+u 側）／ haunchRight（−u 側）
	//      │                        │    鉛直部: depth − haunchHeight（無ければ 0）
	//      └────────────────────────┘  ← 下端 top − depth（幅 bottomWidth）
	//
	// 実データの断面は 4 頂点の台形（斜め部が全高＝haunchHeight == depth）か矩形（張り出し
	// 0）で、外周の地中梁は外側の面が鉛直（片側だけ張り出す）。start / end は**下端の中心線**
	// （センタリング済み）。
	struct FoundationBeamFit
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

	// BeamPrism（頂点列の断面）→ FoundationBeamFit。天端（v 最大）の辺と下端（v 最小）の辺を
	// 見つけ、下端の幅・両側の張り出し・せいを読む。斜め部の高さは「下端の角から天端へ向かう
	// 側辺に、下端と同じ u の中間頂点（鉛直部の上端）があればその高さから上」、無ければ全高。
	// 天端か下端の辺が見つからない断面（三角形・水平な板）は外接矩形で近似する（下端幅＝u の
	// 幅・張り出し 0・せい＝v の幅）。断面が 3 点未満／押し出し長 0 なら false。
	bool fitFoundationBeam(const BeamPrism& prism, FoundationBeamFit& out);

	// 当てはめた地中梁の**底の面**の外形（下端幅 × 長さの矩形。反時計回り）。長さ 0・幅 0 なら空。
	std::vector<Vec2> beamFitOutline(const FoundationBeamFit& fit);

	// --- 部品（同一仕様ごとの外形多角形）------------------------------------------------

	// 底盤のグループ。厚さ（thickness）と天端の絶対 Z（top）が同じ外形の集まり。
	// 底盤の下には kSlabBeddingThickness の砕石を敷く（描画側が足す）。
	struct FoundationSlabGroup
	{
		double top = 0.0;
		double thickness = 0.0;
		PolygonList outlines;
	};

	// 立上り（基礎梁）のグループ。**天端の面**の外形を、天端の絶対 Z（top）ごとに持つ。
	// 下端は真下の底盤の底面（foundationSlabBottom）。人通口で切り下げられた区間は天端が
	// 違うので、別のグループの外形になる。
	struct FoundationRiserGroup
	{
		double top = 0.0;
		PolygonList outlines;
	};

	// 地中梁のグループ。**底の面**の外形を、底の絶対 Z（bottom）と斜め寸法（haunchWidth /
	// haunchHeight）ごとに持つ。天端は真上の底盤の底面（foundationSlabBottom）で、せいは
	// そこから bottom までの差＝取り合いで決まる。
	//
	// 斜め部は**外形の辺ごと**に付ける／付けないを決める（付ける辺は外へ haunchWidth 広がる）:
	//   * 底盤の外（＝建物の外周）に面する辺 … 付けない（外周の地中梁は外側の面が鉛直）
	//   * 他の地中梁と取り合う辺 … 付けない（相手のコンクリートの中）
	//   * それ以外の内側の辺 … 付ける
	// これで「斜め寸法は数値ひとつ」のまま、外周でだけ鉛直という実データの形を再現できる。
	struct FoundationBeamGroup
	{
		double bottom = 0.0;
		double haunchWidth = 0.0;
		double haunchHeight = 0.0;
		PolygonList outlines;
	};

	// OIP で編集できる寸法（PIO のパラメータと 1 対 1）。命令が持つのは**代表値**——取り込み
	// 時のグループから最も多い値（外形の面積で重み付け）を採ったもので、OIP に最初に出る値。
	//
	//   slabTop        … 底盤天端の高さ（GL からの絶対 Z。mm）
	//   slabThickness  … 底盤のコンクリート厚
	//   riserTop       … 立上り天端の高さ（GL からの絶対 Z）
	//   beamDepth      … 地中梁のせい（底盤の底面から梁下端まで）
	//   haunchWidth    … 地中梁の斜め部分の張り出し幅
	//   haunchHeight   … 地中梁の斜め部分の高さ
	//
	// **立上りの幅は無い**——立上りは天端の面の外形で持つので、幅は多角形そのものが表す
	// （ヘッダ冒頭「部品は同一仕様ごとの外形多角形」）。
	struct FoundationParams
	{
		double slabTop = 0.0;
		double slabThickness = 0.0;
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
	//   slabs / risers / beams … 部品（同一仕様ごとの外形多角形）
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
		std::vector<FoundationSlabGroup> slabs;
		std::vector<FoundationRiserGroup> risers;
		std::vector<FoundationBeamGroup> beams;
		FoundationParams params;
	};

	// 外形の真下（真上）に来る底盤の底面の絶対 Z。外形の代表点（重心・頂点・辺の中点）が
	// いちばん多く入る底盤のグループを選び、その 天端 − 厚み を返す。どの底盤にも入らなければ
	// 重心が最も近い底盤、底盤が 1 グループも無ければ代表値（params）から求める。
	// **立上りの下端と地中梁の天端はここから決まる**（ヘッダ冒頭「高さは底盤に取り合う」）。
	double foundationSlabBottom(const FoundationCommand& command, const std::vector<Vec2>& outline);

	// 地中梁の天端＝**底より上に来る**底盤の底面。選び方は foundationSlabBottom と同じだが、
	// 候補を「底盤の底面が bottom より上」に限る——高さの違う底盤が混在する図面で、真下に
	// ある低い底盤を選んでしまうと せいが 0 以下になり、地中梁が丸ごと消えてしまう。
	// 上に来る底盤が 1 枚も無ければ false（out は変更しない）。
	bool foundationBeamTop(const FoundationCommand& command, const std::vector<Vec2>& outline,
						   double bottom, double& out);

	// グループから代表値を求める。外形の**面積**で重み付けして最も多い値を採る（同率なら
	// 大きい値）。斜め部の幅・高さは張り出しのあるグループだけを数える。部品が無い項目は 0。
	// 結果は入力順に依存しない（決定的）。
	FoundationParams foundationBaseParams(const FoundationCommand& command);

	// 代表値の変更を各グループへ配る。imported は取り込み時の部品と代表値（params）を持つ
	// 命令で、edited が OIP の現在値。戻りは部品を動かした命令（params は edited）。
	//
	//   * 底盤 … 天端 += Δ天端、厚み += Δ厚
	//   * 立上り … 天端 += Δ天端（下端は底盤の底面に取り合うので自動で動く）
	//   * 地中梁 … 底 += Δ(底盤の底面) − Δせい（天端は底盤の底面なので、底を動かすと
	//     せいが Δせいだけ変わる）、斜めの幅・高さ += Δ
	// 差を配るので、深い地中梁・高い立上りはその差を保ったまま全体が動く。
	FoundationCommand applyFoundationParams(const FoundationCommand& imported,
											const FoundationParams& edited);

	// --- ソリッドの組み立て（PIO が描くもの）--------------------------------------------

	// 押し出しソリッド 1 つ。base は**閉じた 3D 多角形**（末尾に始点を重複させない）、extent
	// は押し出しベクトル（鉛直なら (0,0,厚み)・斜め部や側面の帯なら 辺方向×長さ）。描画側は
	// base を 3D ポリゴンにして |extent| だけ押し出す（法線の向きは描画側が揃える）。
	struct FoundationSolid
	{
		enum class Kind
		{
			Slab,  // 底盤のコンクリート
			Riser, // 立上り
			Beam, // 地中梁（天端を kGroundBeamSlabBite だけ底盤へ呑み込ませてある）
			Bedding, // 床付け（底盤の下の砕石・地中梁の下の捨てコンと砕石・側面の帯）
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

	// 地中梁グループの**天端の面**の外形（底の面を、斜め部の付く辺だけ haunchWidth 外へ
	// 広げたもの）。slabOutlines は外周部の判定に使う底盤の外形、others は取り合いの判定に
	// 使う他の地中梁の底の面の外形。斜め寸法が 0 なら底の面と同じ。
	std::vector<Vec2> beamTopOutline(const std::vector<Vec2>& outline, double haunchWidth,
									 const PolygonList& slabOutlines, const PolygonList& others);

	// 命令（部品）からソリッド群を組み立てる。並びは 底盤 → 底盤の下の砕石 → 立上り →
	// 地中梁（本体 → 斜め部 → 床付け）で決定的。実体にならない部品（厚み・高さが 0 以下、
	// 外形 3 点未満）は落とす。
	std::vector<FoundationSolid> foundationSolids(const FoundationCommand& command);

	// 平面に描く外形（底盤の外形・立上りの天端の外形・地中梁の天端の外形）。並びは底盤 →
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
