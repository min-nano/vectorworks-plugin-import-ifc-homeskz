//
//	draw/StructuralMember.cpp
//
//	構造材ツール（StructuralMember PIO）共通ヘルパーの実装。呼ぶ SDK API はいずれも
//	従来 draw/Member.cpp・draw/Column.cpp が個別に持っていたものと同一で、集約しただけ
//	（振る舞いは変えない）。設計の意図・パスを共通化しない理由はヘッダ冒頭を参照。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには
//	入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	【パラメータは名前解決してから書き、読み戻して確かめる】断面寸法が入らないと材のせいが
//	0 になり、オブジェクトはあるのに画面に出ない。PIO のパラメータは universal 名が 1 つ違う
//	だけで setter が黙って無視され、しかも数値パラメータが実数ではなく文字列で保持されている
//	ことがある（M6 の垂木で両方に遭遇）。そこで draw/DrawUtil の ResolveParamName で名前を
//	解決し、SetParamRealChecked で書いた値を読み戻して確認する。入らなかった本数は
//	StructuralMemberResult::sectionOk 経由で呼び出し側の診断へ流す。
//
//	【スタイルを使わない】横架材・柱・垂木のどれにもプラグインスタイルを関連付けない
//	（`SetPluginObjectStyle` も `UpdateStyledObjects` も呼ばない）。描画属性はクラスに、
//	寸法・構造材 ID 等は個別フィールドに持たせる。以前は横架材（木質構造材_横架材）と柱
//	（木質構造材_柱・束）にだけ図面側のスタイルを当てていたが、ご要望によりやめた——
//	図面にスタイルがあるかどうかで絵が変わらず、垂木と同じ作法に揃う
//	（docs/DEV-NOTES.md「構造材のスタイルをやめた」）。
//
//	【パーツの描画属性もクラスへ向ける】スタイルを外しただけでは、PIO がパーツごとに持つ
//	2D 属性（構造材・被覆・中心線・端部）の既定が残り、クラスに関係なく描かれる。構造材と
//	端部はクラス属性に、被覆と中心線は非表示にする（ApplyClassStyleAttributes）。
//

#include "PluginPrefix.h"
#include "draw/StructuralMember.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"

#include "VWFC/VWObjects/VWParametricObj.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 構造材ツールの PIO 名（VW 実機の登録名に一致させる）。柱も横架材もこの
		// 1 つのツールで描く（柱・間柱ツールはスクリプトからの操作に対して不安定なので、
		// 柱も標準の構造材ツールで描く。parse/Column.h）。
		const TXString kStructuralMember(kStructuralMemberPlugin);

		// パス（NURBS 曲線）の次数。直線 1 本なので 1。byCtrlPts=false ＝ 通過点で定義する。
		constexpr short kPathDegree = 1;
		// パスに必要な頂点数（始端・終端）。読み戻して診断に使う。
		constexpr Sint32 kPathPointCount = 2;
		// **パスを置く平面の Z。** パスは 2D で渡し、高さはストーリバウンドだけが決める
		// （draw/StructuralMember.h 冒頭「パスは 2D で渡す」）。ここが 0 以外になることは
		// 無い——なるなら「高さを 2 か所で指定する」形へ戻っている。
		constexpr double kPathPlaneZ = 0.0;

		// 構造材ツールのフィールド名。**名前が 1 つ違うだけで setter は黙って無視される**（M6
		// の垂木で実証済み。draw/Rafter.cpp 冒頭）ので、寸法は読み戻して確かめる。記号 PIO
		// も読む 3 つ（kFieldStructuralUse / kFieldMajorBreadth / kFieldMajorDepth）
		// とそのローカライズ名は draw/StructuralMember.h にある。ここは書き手だけが使う残り。
		constexpr const char* kFieldMemberID = "MemberID";			   // 構造材 ID
		constexpr const char* kFieldProfileShape = "ProfileShape";	   // 断面形状
		constexpr const char* kFieldB = "B";						   // 幅（矩形断面）
		constexpr const char* kFieldD = "D";						   // せい（矩形断面）
		constexpr const char* kFieldMemberType = "MemberType";		   // 部材種別
		constexpr const char* kFieldAxisAlign = "AxisAlign";		   // 軸の配置基準
		constexpr const char* kFieldStartCondition = "StartCondition"; // 始端の端部条件
		constexpr const char* kFieldEndCondition = "EndCondition";	   // 終端の端部条件
		constexpr const char* kFieldProfileSeries = "ProfileSeries";   // 断面シリーズ

		// universal 名で引けなかったときに使う OIP のローカライズ名（ResolveParamName）。
		constexpr const char* kLocalizedProfileShape = "断面形状";

		// 端部オフセット（OIP の「始端オフセット」「終端オフセット」）。**universal 名は
		// SDK ヘッダに無い**ので候補を並べて引く（ヘッダ冒頭「端部オフセット」）。順番は
		// ありそうな順で、最初に見つかったものを使う。ローカライズ名は OIP の表示名。
		const std::vector<const char*> kStartOffsetNames = {"StartOffset", "OffsetStart",
															"StartExtension", "StartCutOffset"};
		const std::vector<const char*> kEndOffsetNames = {"EndOffset", "OffsetEnd", "EndExtension",
														  "EndCutOffset"};
		const std::vector<const char*> kLocalizedStartOffset = {"始端オフセット"};
		const std::vector<const char*> kLocalizedEndOffset = {"終端オフセット"};
		// 名前を解決できなかったときに診断へ載せる候補の絞り込み（OIP の表示名に含まれる語）。
		// **診断にしか使わない**ので開発ビルドだけ（draw/Verify.h）。
#if VW_DRAW_VERIFY
		constexpr const char* kOffsetParamNeedle = "オフセット";
#endif

		// 描き上がった部材の長さ（OIP の「長さ」）。**端部オフセットと同じく universal 名が
		// SDK ヘッダに無い**ので候補を並べて引き、引けなければ手掛かりを診断へ持ち帰る。
		// 読むだけで、書きはしない（PIO がリセットのたびに入れる値）。
		// **両端の解決済み絶対 Z**（実機 round 2 で判明。ヘッダ DrawnMemberSize 参照）。
		// **ローカライズ名で引かない**——「始端高さオフセット」は設定ダイアログ側の
		// `DialogStartElevation`（別物。レイヤの高さ基準で 0 を返す）とぶつかる。
		// **読むのは `MeasureDrawnMember` だけ**で、そこは検算専用になったので開発ビルドだけ
		// （draw/Verify.h。自己修復の撤去。docs/DEV-NOTES.md M27）。
#if VW_DRAW_VERIFY
		const std::vector<const char*> kStartElevationNames = {"StartElevation"};
		const std::vector<const char*> kEndElevationNames = {"EndElevation"};
		const std::vector<const char*> kNoLocalized = {};
#endif
		// **水平材の実体はパラメータでは測らない。** OIP に「スパン」に当たるパラメータは
		// 無く（確定済み。**再調査しない**——ヘッダ StructuralExtentKind が指す Findings の
		// 「打ち切った調査」）、名前で引ける `CenterPointLength(長さ)` は部材長ではない
		// （実長 5333 の柱で 100。センターマークの線の長さである）。
		// 代わりに**PIO が実際に持っているパスの両端の距離**（DrawUtil の `PioPathChord`）で
		// 測る——`ResetObject` が解決済みバウンドから作り直したパスが、そのまま「描かれた
		// 実体」だからである（ヘッダ StructuralExtentKind ／ Findings「Parametric Objects」）。
		//
		// 名前に「長さ」「高さ」「スパン」を含むパラメータを**名前と値で**並べて証拠にする
		// ときの手掛かり（DescribeSizeParams）。どのパラメータが OIP のどの欄なのかを実機で
		// 確かめる手段がほかに無い——実際、round 2 のこの一覧で「長さ」で引ける
		// `CenterPointLength` が部材長ではないと分かり、round 1 のこの一覧に「スパン」が
		// 1 件も出なかったことが、測る相手をパラメータからパスへ変える最初の手掛かりになった
		// （その後 SDK リファレンス側で全数から確定した。上記）。
		// **診断にしか使わない**ので開発ビルドだけ（draw/Verify.h）。
#if VW_DRAW_VERIFY
		const std::vector<const char*> kSizeParamNeedles = {"長さ", "高さ", "スパン", "Span"};
#endif
		// 「長さ 0」とみなす閾値（mm）。潰れた部材はちょうど 0 を返すので、実部材の長さ
		// （最短でも数十 mm）と取り違える余地は無い。**検算でしか使わない**ので開発ビルド
		// だけ（上記と同じ理由）。
#if VW_DRAW_VERIFY
		constexpr double kCollapsedLength = 0.01;
#endif
		// 描かれた絶対 Z が命令と「合っている」とみなす許容（mm）。丸めのぶんだけで、
		// 意味のあるずれ（レイヤ原点へ落ちる・階ぶん動く）とは桁が違う。**高さの検算は
		// 開発ビルドだけ**（draw/Verify.h）。
#if VW_DRAW_VERIFY
		constexpr double kElevationTol = 1.0;
#endif

		// フィールドに渡す値。
		constexpr const char* kProfileShapeRectangle = "Rectangle";
		constexpr const char* kProfileSeriesDefault = "AISC (Inch)";

		// --- ポップアップのキー ---------------------------------------------------------
		//
		// 構造材ツールのポップアップは**選択肢をキー（数値の文字列）で保持する**ので、素で
		// 書くと "2" / "3" / "7" が何を選んだのか読めない。使う選択肢だけを列挙して名前を
		// 付け、文字列への変換は PopupKey 1 か所に置く。

		// 部材種別。横架材（梁）・柱とも Structural で、種別の違いは構造用途
		// （StructuralUse）の方に出る。
		enum class MemberTypeKey : int
		{
			Structural = 2,
		};

		// 断面基準点。3×3 グリッドを 0 始まり・行優先（上段 0,1,2 / 中段 3,4,5 / 下段 6,7,8）
		// で並べたキー。天端中央＝1・中央＝4 は実機確認済みで、中下＝7 はその並びから採った
		// （ローカル確認の項目。docs/DEV-NOTES.md M16）。
		enum class AxisAlignKey : int
		{
			TopCentre = 1,	  // 上段中央
			Centre = 4,		  // 中央
			BottomCentre = 7, // 下段中央
		};

		// 端部条件。
		enum class EndConditionKey : int
		{
			Square = 3, // 直切り
		};

		// --- 2D 属性（「構造材設定」の「属性」タブ） ----------------------------------------
		//
		// 【描画属性はクラススタイルで描く】構造材 PIO は 2D の描画属性を**パーツ（構造材・
		// 被覆・中心線・端部）ごとに自前で**持っており、既定のままだと PIO 自身のクラスに
		// 関係なく per-part の線種・太さ・色で描かれる（切断面より上は構造材がラインタイプの
		// 破線・0.18 など）。そこで構造材と端部を**クラス属性**にし、指定のクラス（＝PIO と
		// 同じ drawClass）で描かせる。被覆と中心線は描かない（ご要望）。
		//
		// universal 名は `<パーツ><欄>_<面>`。**面は 3 面とも同じ値を書く**——どの面が描かれる
		// かは切断面と材の高さの関係で決まる（平面図の Z=0 の材は `_Below`）ので、面を選ぶと
		// 切断面の高さ次第で絵が変わる。名前・値・書き方はすべて SDK リファレンス
		// Findings「Parametric Objects」の「取り込みで使う形（構造材＝クラススタイル・
		// 被覆と中心線は非表示・端部は両端）」で実機確認済み:
		//   * クラス欄（欄型 18 `kFieldClassesPopup`）は**名前**で書く（`SetParamClass` は効かない）。
		//   * 「クラス属性」は線（`…PenStyle`）が 4、面（`…FillStyle`）が 6。
		//   * 端部の「両端」はポップアップではなく、始端・終端の表示（真偽）2 つが両方 true。
		//   * `AttributesMode` は描画に効かないので触らない。
		constexpr std::array<const char*, 3> kAttributeFaces = {"_Above", "_At", "_Below"};
		constexpr const char* kPenStyleClass = "4";	 // 線の属性＝クラス属性
		constexpr const char* kFillStyleClass = "6"; // 面の属性＝クラス属性

		// パーツ・欄・面から universal 名を組み立てる。
		TXString AttributeParam(const char* field, const char* face)
		{
			TXString name(field);
			name += face;
			return name;
		}

		// 2D 属性を 3 面とも書く（構造材・端部はクラス属性、被覆・中心線は非表示）。
		// **ResetObject より前に呼ぶ**（書いた値は次の ResetObject でそのまま描画に効く。
		// 同 Findings）。クラス名が空ならクラス欄は書かず、属性の出どころだけをクラスにする。
		void ApplyClassStyleAttributes(VWParametricObj& pio, const std::string& className)
		{
			const TXString drawClass(className.c_str());
			for (const char* face : kAttributeFaces)
			{
				// 構造材: 表示・線と面はクラス属性。
				pio.SetParamBool(AttributeParam("MemberDisplay", face), true);
				if (!className.empty())
					pio.SetParamValue(AttributeParam("MemberClass", face), drawClass);
				pio.SetParamValue(AttributeParam("MemberPenStyle", face), kPenStyleClass);
				pio.SetParamValue(AttributeParam("MemberFillStyle", face), kFillStyleClass);
				// 被覆・中心線: 表示しない。
				pio.SetParamBool(AttributeParam("CoverDisplay", face), false);
				pio.SetParamBool(AttributeParam("CenterlineDisplay", face), false);
				// 端部: 両端を表示し、属性は構造材と同じ（同じクラスのクラス属性）。端部は
				// 線だけのパーツで面の欄を持たない。
				pio.SetParamBool(AttributeParam("StartCapDisplay", face), true);
				pio.SetParamBool(AttributeParam("EndCapDisplay", face), true);
				if (!className.empty())
					pio.SetParamValue(AttributeParam("CapsClass", face), drawClass);
				pio.SetParamValue(AttributeParam("CapsPenStyle", face), kPenStyleClass);
			}
		}

		// ポップアップのキーを PIO へ渡す文字列にする。
		template <typename Key> TXString PopupKey(Key key)
		{
			return TXString(std::to_string(static_cast<int>(key)).c_str());
		}

		// 実数パラメータを読む。**実数で 0 を読んだだけでは 0 と決めない**——数値パラメータが
		// 文字列で保持されている PIO があり（SetParamRealChecked が実際にその経路を持つ）、
		// そのときは実数読みが常に 0 を返して**全数を誤報する**。文字列でも読み直し、数として
		// 読めたらそちらを採る。読めないパラメータを覗くと例外が出るので畳む（1 つの読み損
		// ないで描画を止めない。DrawUtil の PioParamString と同じ扱い）。ok には読めたかを返す。
		//
		// **呼ぶのは `MeasureDrawnMember` だけ**なので開発ビルドだけ（draw/Verify.h）。
#if VW_DRAW_VERIFY
		double ReadParamNumber(const VWParametricObj& pio, const TXString& param, bool& ok)
		{
			ok = false;
			if (param.IsEmpty())
				return 0.0;
			try
			{
				const double value = pio.GetParamReal(param);
				ok = true;
				if (std::abs(value) >= kCollapsedLength)
					return value;
				const std::string text = PioParamString(pio, param.GetStdString().c_str());
				return text.empty() ? value : std::strtod(text.c_str(), nullptr);
			}
			catch (...)
			{
				ok = false;
				return 0.0;
			}
		}
#endif // VW_DRAW_VERIFY

		// パラメータ 1 件を人が読める形で読み出す（実数と、文字列で保持されていればその値）。
		// **読めないパラメータを覗くと例外が出る**ので、ここで 1 件ずつ畳んで "?" を返す
		// ——呼び出し側が 1 件の読み損ないで全体を失わないようにするための粒度である
		// （DrawUtil の PioParamString と同じ扱い）。**DescribeSizeParams だけが使う**ので
		// 開発ビルドだけ（draw/Verify.h）。
#if VW_DRAW_VERIFY
		std::string ReadParamText(const VWParametricObj& pio, const TXString& param)
		{
			if (param.IsEmpty())
				return "?";
			try
			{
				std::array<char, 32> buffer{};
				std::snprintf(buffer.data(), buffer.size(), "%g", pio.GetParamReal(param));
				std::string text(buffer.data());
				const std::string raw = PioParamString(pio, param.GetStdString().c_str());
				if (!raw.empty())
				{
					text += "/\"";
					text += raw;
					text += "\"";
				}
				return text;
			}
			catch (...)
			{
				return "?";
			}
		}
#endif // VW_DRAW_VERIFY

		// 断面基準点 → ポップアップのキー。
		AxisAlignKey AxisAlignOf(StructuralAxisAlign align)
		{
			switch (align)
			{
			case StructuralAxisAlign::Centre:
				return AxisAlignKey::Centre;
			case StructuralAxisAlign::BottomCentre:
				return AxisAlignKey::BottomCentre;
			case StructuralAxisAlign::TopCentre:
			default:
				return AxisAlignKey::TopCentre;
			}
		}

	} // namespace

	// **呼ぶのは要素ごとの描画**（draw/Member・draw/Column・draw/Rafter）で、どれも他の区間の
	// 外から呼ぶので、計測の区間はここで開く（**区間は入れ子にしない**。core/DrawTiming.h
	// 「使う側の作法」／draw/Verify.h）。かつては区間の中から呼ぶ経路——潰れた材のパスを
	// 作り直す自己修復——があったので、区間を開かない本体を分けて持っていたが、その自己修復を
	// 撤去したので分ける理由も無くなった（docs/DEV-NOTES.md「柱が長さ 0 で描かれる（M27）」）。
#if VW_DRAW_VERIFY
	MCObjectHandle CreatePath(const core::Vec2& start, const core::Vec2& end, bool& outAppended,
							  PathProbe* outProbe)
#else
	MCObjectHandle CreatePath(const core::Vec2& start, const core::Vec2& end, bool& outAppended)
#endif
	{
		VW_DRAW_TIME("構造材:パス生成");
		outAppended = false;
		MCObjectHandle path =
			gSDK->CreateNurbsCurve(WorldPt3(start.x, start.y, kPathPlaneZ), false, kPathDegree);
		if (path == nil)
			return nil;

		// **Add3DVertex が VS の AddVertex3D にあたる**（ヘッダ参照）。末尾へ 1 点足して
		// 始端 → 終端の 2 点にする。
		gSDK->Add3DVertex(path, WorldPt3(end.x, end.y, kPathPlaneZ));
		// 頂点が本当に 2 つになったかを読み戻す。ピース索引の起点は 0 / 1 のどちらの
		// 規約もあり得るので両方を見る（**判定に失敗しても曲線はそのまま使う**——ここで
		// 諦めると、索引の規約違いというだけで部材が 1 本も描かれなくなる）。
		const Sint32 piece0 = gSDK->NurbsGetNumPts(path, 0);
		const Sint32 piece1 = gSDK->NurbsGetNumPts(path, 1);
#if VW_DRAW_VERIFY
		PathProbe probe;
		probe.piece0 = piece0;
		probe.piece1 = piece1;
		// **2 点の Z を読み戻す。** 数が 2 でも同じ位置なら部材は実体を持たない
		// （実機で 46 本。draw/StructuralMember.h の PathProbe）。**観測だけ**なので
		// 開発ビルドにしか無い。
		WorldPt3 first(0.0, 0.0, 0.0);
		WorldPt3 second(0.0, 0.0, 0.0);
		if (gSDK->NurbsGetPt3D(path, 0, 0, first) && gSDK->NurbsGetPt3D(path, 0, 1, second))
		{
			probe.pointsRead = true;
			probe.z0 = first.z;
			probe.z1 = second.z;
		}
#endif
		// **座標を明示的に入れ直す。** `Add3DVertex` が足した点が渡した位置にならないことが
		// ある（ヘッダ参照）。うまく足せていたときは同じ値を書くだけで何も変わらない。
		// **これは観測ではなく描画の一部**（外すと実機で潰れた材が戻る）なので、本番でも走る
		// ——開発ビルドだけなのは、その戻り値を控える下の 2 行である。
		if (piece0 >= kPathPointCount)
		{
			[[maybe_unused]] const Boolean startSet =
				gSDK->NurbsSetPt3D(path, 0, 0, WorldPt3(start.x, start.y, kPathPlaneZ));
			[[maybe_unused]] const Boolean endSet =
				gSDK->NurbsSetPt3D(path, 0, 1, WorldPt3(end.x, end.y, kPathPlaneZ));
#if VW_DRAW_VERIFY
			probe.setOk = startSet && endSet;
			WorldPt3 fixed(0.0, 0.0, 0.0);
			if (gSDK->NurbsGetPt3D(path, 0, 1, fixed))
			{
				probe.fixedRead = true;
				probe.fixedZ1 = fixed.z;
			}
#endif
		}
#if VW_DRAW_VERIFY
		if (outProbe != nullptr)
			*outProbe = probe;
#endif
		outAppended = piece0 >= kPathPointCount || piece1 >= kPathPointCount;
		return path;
	}

	StructuralMemberResult DrawStructuralMember(const StructuralMemberSpec& spec)
	{
		StructuralMemberResult result;
		// **空のプロファイルを渡してはならない**（断面が無いのと同じで、PIO は生成できても
		// 実体が描かれず「オブジェクトはあるのに画面に何も出ない」状態になる。
		// draw/DrawUtil の CreateRectangleProfileGroup 参照）。
		if (spec.path == nil || spec.profile == nil)
			return result;

		// **クラスと描画属性は「作る前に文書の既定として立てる」。** 作った後に
		// SetObjectClass と 6 つの Set*ByClass を呼ぶと、構造材 PIO はその 1 回ごとに作り
		// 直され（1 回 9〜12ms）、取り込み全体の半分をここで使っていた（docs/DEV-NOTES.md
		// 「描画の高速化」。SDK リファレンス Findings「Attributes and Classes」）。既定は
		// 生まれる瞬間に読まれるので、スコープは作る 1 行だけを囲む——抜けた時点で既定の
		// クラスは利用者のものへ戻り、この後の ResetObject も従来と同じ既定の下で走る。
		MCObjectHandle object = nil;
		{
			const ScopedCreationClass creationClass(spec.drawClass);
			{
				VW_DRAW_TIME("構造材:オブジェクト生成");
				// **作った時点では作り直させない（doRegen=false）。** 既定の true では「作った
				// 時点で 1 回」＋「この後の ResetObject で 1 回」の計 2 回作り直しが走り、
				// 1 本あたりの所要の約半分が前者だった。false なら ResetObject の 1 回だけになる
				// （SDK リファレンス Findings「Parametric Objects」の「`doRegen=false` は
				// 速い…」。docs/DEV-NOTES.md「描画の高速化」）。
				//
				// **成り立つのはパスの両端の Z が等しいときだけ**——Z の差があると 1 回目の
				// ResetObject が「バウンドの span ＋ 渡した Z の差」を返し、材の高さが狂う
				// （同 Findings。平面上の長さは関わらない）。構造材のパスは必ず CreatePath で
				// 両端とも kPathPlaneZ に作るので、横架材・柱・垂木のどれも条件を満たす。
				// **パスに Z を持たせる作りへ戻すなら、ここも true へ戻すこと。**
				object = gSDK->CreateCustomObjectPath(kStructuralMember, spec.path, spec.profile,
													  false /* doRegen */);
			}
			if (object == nil)
				return result;
			// 既定を継いだかを読み戻し、継いでいなければ従来どおり与え直す（中で区間を開く
			// ので、ここは包まない）。
			result.classInherited = FinishCreatedWithClass(object, creationClass, spec.drawClass);
		}
		// 高さ基準を始端（0）・終端（1）それぞれのストーリレベルへバインドする。これで
		// 構造材ツールの高さ基準が「レイヤの高さ」・offset 0 のまま実ジオメトリと矛盾する
		// ことがなくなり、編集時に高さがリセットされない。水平材の傾斜はこの offset 差で
		// 表れ、鉛直材ではこの差が柱高さを支配する。
		// **戻り値を見る。** 受け取られなければ材は高さを持てない（＝実体が無い材になる）。
		{
			VW_DRAW_TIME("構造材:高さ基準");
			const bool startBoundOk =
				ApplyStoryBound(object, StoryBoundSlot::Start, spec.startBound);
			const bool endBoundOk = ApplyStoryBound(object, StoryBoundSlot::End, spec.endBound);
			result.boundOk = startBoundOk && endBoundOk;
		}

		VWParametricObj pio(object);

		// 【計測のために区間へ割ってある】ここから下は「名前の解決」と「パラメータ書き」が
		// 交互に来る。どちらが重いのかがフェーズの時刻差からは分からないので、**別々の
		// 区間として測れるように**代入を 1 段はさんである（draw/Verify.h の VW_DRAW_TIME。
		// 本番ビルドでは丸ごと畳まれて、残るのは代入 1 つだけになる）。
		//
		// ★**解決を書き込みより前へ動かさない。** パラメータ表が断面形状（矩形/H 形…）で
		// 変わりうるかは分かっていない——**分かるまでは、いまの順序（断面形状を書いた後に
		// B / D を引く）を守る**。SDK リファレンス側で調査中
		// （min-nano/vectorworks-developer-sdk-reference#82）。
		TXString breadth;
		TXString depth;
		TXString profileShape;
		{
			VW_DRAW_TIME("構造材:名前解決");
			breadth = ResolveParamName(pio, kFieldMajorBreadth, kLocalizedBreadth);
			depth = ResolveParamName(pio, kFieldMajorDepth, kLocalizedDepth);
			profileShape = ResolveParamName(pio, kFieldProfileShape, kLocalizedProfileShape);
		}

		bool breadthOk = false;
		bool depthOk = false;
		{
			VW_DRAW_TIME("構造材:パラメータ書き");
			pio.SetParamAsString(profileShape, kProfileShapeRectangle);
			pio.SetParamAsString(kFieldProfileSeries, kProfileSeriesDefault);
			breadthOk = SetParamRealChecked(pio, breadth, spec.width);
			depthOk = SetParamRealChecked(pio, depth, spec.depth);
		}

		// B / D は矩形断面のときの別名。上と同じ値を入れる（存在しなければ無視される）。
		// 2 つの解決をまとめたので `D` を引くのが `B` を書く前になったが、**寸法の値は
		// パラメータ表の顔ぶれを変えない**ので順序の意味は変わらない（変わりうるのは
		// 上の ★ の断面形状の方で、そちらは動かしていない）。
		TXString bAlias;
		TXString dAlias;
		{
			VW_DRAW_TIME("構造材:名前解決");
			bAlias = ResolveParamName(pio, kFieldB, kLocalizedBreadth);
			dAlias = ResolveParamName(pio, kFieldD, kLocalizedDepth);
		}
		{
			VW_DRAW_TIME("構造材:パラメータ書き");
			SetParamRealChecked(pio, bAlias, spec.width);
			SetParamRealChecked(pio, dAlias, spec.depth);
			pio.SetParamAsString(kFieldMemberID, TXString(spec.memberId.c_str()));
			pio.SetParamAsString(kFieldMemberType, PopupKey(MemberTypeKey::Structural));
			pio.SetParamAsString(kFieldStructuralUse, TXString(spec.structuralUse.c_str()));
			pio.SetParamAsString(kFieldAxisAlign, PopupKey(AxisAlignOf(spec.axisAlign)));
			pio.SetParamAsString(kFieldStartCondition, PopupKey(EndConditionKey::Square));
			pio.SetParamAsString(kFieldEndCondition, PopupKey(EndConditionKey::Square));
			ApplyClassStyleAttributes(pio, spec.drawClass);
		}

		// 端部オフセット。**要らない（両端 0）なら触らない**——スタイル既定が 0 なので書く
		// 必要が無く、名前を解決できない構成でも余計な診断を出さずに済む。
		if (spec.startOffset != 0.0 || spec.endOffset != 0.0)
		{
			TXString startOffset;
			TXString endOffset;
			{
				// **端部オフセットの universal 名は候補を順に空振りしてから、ローカライズ名で
				// パラメータ表を端から舐める**（draw/DrawUtil の ResolveParamNameAmong）。
				// 実データでは横架材の 7 割・柱のほぼ全数がここを通る（docs/DEV-NOTES.md
				// M20）ので、名前解決の区間が重く出るならまずここを疑う。
				VW_DRAW_TIME("構造材:名前解決");
				startOffset = ResolveParamNameAmong(pio, kStartOffsetNames, kLocalizedStartOffset);
				endOffset = ResolveParamNameAmong(pio, kEndOffsetNames, kLocalizedEndOffset);
			}
			if (startOffset.IsEmpty() || endOffset.IsEmpty())
			{
				result.endOffsetOk = false;
#if VW_DRAW_VERIFY
				result.offsetParamHint = DescribeParamsContaining(pio, kOffsetParamNeedle);
#endif
			}
			else
			{
				VW_DRAW_TIME("構造材:パラメータ書き");
				const bool startOk = SetParamRealChecked(pio, startOffset, spec.startOffset);
				const bool endOk = SetParamRealChecked(pio, endOffset, spec.endOffset);
				result.endOffsetOk = startOk && endOk;
			}
		}
		{
			VW_DRAW_TIME("構造材:リセット");
			gSDK->ResetObject(object);
		}

		// 【描けたかを読み戻して検算する】PIO は生成できても実体を持たないことがある（パスが
		// 1 点のまま・バウンドの解決に失敗、など。Findings「Parametric Objects」）。そのとき OIP の
		// 高さ・基準・オフセットは命令どおりのままなので、**画面を見ない限り気付けない**。
		// リセット後の実体を読み戻し、0 で潰れていたら呼び出し側の診断へ流す。
		//
		// **まるごと開発ビルドだけ**（draw/Verify.h）。以前は「自己修復の引き金」という本番でも
		// 要る用途があったので本番にも半分残していたが、その自己修復を撤去したので**測る理由は
		// 検算だけ**になった——外しても描かれるものは 1 つも変わらない（docs/DEV-NOTES.md
		// 「柱が長さ 0 で描かれる（M27）」）。本番では材 1 本あたりのパラメータ走査とパス読みが
		// まるごと無くなる。
#if VW_DRAW_VERIFY
		if (spec.expectedLength > kCollapsedLength || spec.checkElevation)
		{
			// **読み戻しはひとまとめの区間**にする（区間は入れ子にしない。draw/Verify.h）。
			VW_DRAW_TIME("構造材:読み戻し");

			const DrawnMemberSize size = MeasureDrawnMember(object, spec.extentKind);
			if (spec.expectedLength > kCollapsedLength)
			{
				// **測れなかったときだけ手掛かりを採る。** 鉛直材なら両端の絶対 Z を、
				// 水平材なら PIO のパスを引けなかったということなので、パラメータの顔ぶれと
				// 図面のパスを両方並べる——原因をどちら側に分けるかは、実機でしか読めない
				// この 1 行にしか手掛かりが無い（ヘッダ extentHint）。
				if (!size.found)
					result.extentHint = DescribeSizeParams(object) + "・図面のパス[" +
										DescribePioPath(object) + "]";
				result.collapsed = size.zero;
				// **潰れていたら証拠を全部採る。** 高さ基準は**どう書いても両端の Z を動かせ
				// なかった**（ストーリ相対・レイヤ基準・VW が記録しているとおり、のいずれでも
				// 実測 0。docs/DEV-NOTES.md「柱が長さ 0 で描かれる（M27）」）ので、残る入力は
				// パスである。**PIO が実際に持っているパスの頂点**まで読み戻して添える。
				//
				// **直しには行かない。** 潰れていたら差し替えて繕う道は撤去した（同メモ）——
				// 差し替えはバウンドの `fOffset` を書き換えてしまい、利用者が階高を編集した
				// 瞬間に長さとして表に出る。ここは**報せるだけ**にする。
				if (result.collapsed)
					result.collapsedProbe = DescribeSizeParams(object) + "・図面の始端基準[" +
											DescribeStoryBound(object, StoryBoundSlot::Start) +
											"]・終端基準[" +
											DescribeStoryBound(object, StoryBoundSlot::End) +
											"]・図面のパス[" + DescribePioPath(object) + "]";
			}

			// 【描かれた高さが命令どおりか】**パスから Z を外したぶんの見張り**である
			// （ヘッダ冒頭「パスは 2D で渡す」）。高さを決めるのがストーリバウンドだけに
			// なった以上、その解決が意図とずれても**本数にも長さにも一切出ない**——材が
			// 揃って違う高さに並ぶだけなので、実機の絵を見るまで気付けない。そこで読み戻した
			// 両端の絶対 Z を命令と引き比べ、ずれた本数と 1 件目の実測を持ち帰る。
			// **読めなかったときは「ずれた」に数えない**（測れていないことを不具合として
			// 報せると、切り分けが逆に遠のく）。
			if (spec.checkElevation && size.elevationRead)
			{
				const double startGap = size.start - spec.expectedStartZ;
				const double endGap = size.end - spec.expectedEndZ;
				if (std::abs(startGap) > kElevationTol || std::abs(endGap) > kElevationTol)
				{
					result.elevationOk = false;
					std::array<char, 192> buffer{};
					std::snprintf(buffer.data(), buffer.size(),
								  "命令の Z %g→%g に対し図面の Z %g→%g（ずれ %g / %g）",
								  spec.expectedStartZ, spec.expectedEndZ, size.start, size.end,
								  startGap, endGap);
					result.elevationProbe = buffer.data();
				}
			}
		}
#endif

		result.object = object;
		result.sectionOk = breadthOk && depthOk;
		return result;
	}

#if VW_DRAW_VERIFY
	DrawnMemberSize MeasureDrawnMember(MCObjectHandle object, StructuralExtentKind kind)
	{
		DrawnMemberSize size;
		if (object == nil)
			return size;
		try
		{
			const VWParametricObj pio(object);
			// **両端の解決済み絶対 Z。** 鉛直材はこの差が実体そのもの。水平材では実体を
			// 測れないが、**描かれた高さが命令どおりか**の検算に要る——パスから Z を
			// 外した（ヘッダ冒頭「パスは 2D で渡す」）いま、高さを言える値はこの 2 つしか
			// 残っていない。
			{
				const TXString startName =
					ResolveParamNameAmong(pio, kStartElevationNames, kNoLocalized);
				const TXString endName =
					ResolveParamNameAmong(pio, kEndElevationNames, kNoLocalized);
				bool startOk = false;
				bool endOk = false;
				const double start = ReadParamNumber(pio, startName, startOk);
				const double end = ReadParamNumber(pio, endName, endOk);
				if (startOk && endOk)
				{
					size.elevationRead = true;
					size.start = start;
					size.end = end;
				}
			}
			if (kind == StructuralExtentKind::Horizontal)
			{
				// 水平材。**PIO が実際に持っているパスの両端の距離**で測る（ヘッダ
				// StructuralExtentKind）。`ResetObject` は解決済みバウンドからパスを作り直し、
				// 水平成分は渡したパスのまま残すので、**リセット後のこのパスが描かれた実体
				// そのもの**である（Findings「Parametric Objects」）。OIP の「スパン」は
				// 実機に無いので引かない——**引けない名前を測りに行くと、パスは正常なのに
				// 全数を「測れなかった」と報せ続けることになる**（以前はそうなっていた）。
				double chord = 0.0;
				if (!PioPathChord(object, chord))
					return size;
				size.found = true;
				size.extent = chord;
				size.zero = size.extent < kCollapsedLength;
				return size;
			}
			if (!size.elevationRead)
				return size;
			size.found = true;
			size.extent = std::abs(size.end - size.start);
			size.zero = size.extent < kCollapsedLength;
		}
		catch (...)
		{
			return DrawnMemberSize{}; // 読めなかった＝測れていない
		}
		return size;
	}
#endif // VW_DRAW_VERIFY

#if VW_DRAW_VERIFY
	std::string DescribeSizeParams(MCObjectHandle object)
	{
		if (object == nil)
			return {};
		try
		{
			const VWParametricObj pio(object);
			std::string found;
			const size_t count = pio.GetParamsCount();
			for (size_t i = 0; i < count; ++i)
			{
				const TXString name = pio.GetParamName(i);
				const std::string universal = name.GetStdString();
				const std::string localized = pio.GetParamLocalizedName(i).GetStdString();
				const bool matches =
					std::ranges::any_of(kSizeParamNeedles,
										[&universal, &localized](const char* needle)
										{
											return universal.find(needle) != std::string::npos ||
												   localized.find(needle) != std::string::npos;
										});
				if (!matches)
					continue;
				if (!found.empty())
					found += ", ";
				found += universal;
				found += "(";
				found += localized;
				found += ")=";
				// **読み出しは 1 件ずつ守る。** 名前に「長さ」「高さ」を含むパラメータが
				// 実数とは限らず（ポップアップや文字列のこともある）、1 件の読み損ないで
				// 例外が出ると、それまでに積んだ他のパラメータごと捨てることになる——
				// **実機の未知の挙動を壊さず観測する**ための行なので、1 件は "?" にして
				// 残りを持ち帰る。
				found += ReadParamText(pio, name);
			}
			return found;
		}
		catch (...)
		{
			return {};
		}
	}
#endif // VW_DRAW_VERIFY

	void StructuralFailures::record(const StructuralMemberResult& result)
	{
		if (!result.sectionOk)
			++section;
		if (!result.boundOk)
			++bound;
		if (!result.endOffsetOk)
		{
			++offset;
#if VW_DRAW_VERIFY
			if (offsetHint.empty())
				offsetHint = result.offsetParamHint;
#endif
		}
#if VW_DRAW_VERIFY
		if (!result.classInherited)
			++classFallback;
		if (result.collapsed)
			++collapsed;
		if (result.collapsed && collapsedProbe.empty())
			collapsedProbe = result.collapsedProbe;
		if (extentHint.empty())
			extentHint = result.extentHint;
		if (!result.elevationOk)
		{
			++elevation;
			if (elevationProbe.empty())
				elevationProbe = result.elevationProbe;
		}
#endif
	}

	std::string DescribeStructuralFailures(const StructuralFailures& failures, const char* subject)
	{
		std::string note;
		// 「<説明><呼び名> N 本。」を積む（0 件は出さない。draw/DrawUtil の AppendCount）。
		const auto count = [&note, subject](const char* what, std::size_t howMany)
		{ AppendCount(note, (std::string(what) + subject).c_str(), howMany, "本"); };

		count("パスが 2 点にならなかった", failures.path);
		count("断面を設定できなかった", failures.section);
		count("高さ基準を図面へ書けなかった", failures.bound);
#if VW_DRAW_VERIFY
		count("長さ 0 で描かれた（実体が無い）", failures.collapsed);
		// 作る前に立てた既定（クラス・描画属性）を継がずに生まれ、作った後に与え直した本数。
		// 0 でなければその本数ぶん高速化が効いていない（絵は従来どおり）。
		count("既定のクラスを継がず作った後に与え直した", failures.classFallback);
		if (!failures.collapsedProbe.empty())
			note += "（1 本目: " + failures.collapsedProbe + "）";
		count("命令と違う高さに描かれた", failures.elevation);
		if (failures.elevation > 0 && !failures.elevationProbe.empty())
			note += "（1 本目: " + failures.elevationProbe + "）";
		// **手掛かりが埋まっていること自体が異常なので、無条件に出す。** 以前は「潰れ・
		// 作り直しがあったときだけ」にしていたが、それは**水平材が実機に無いパラメータ
		// （OIP の「スパン」）を引きに行っていて、引けないのが常態だったから**である
		// （健全な周が毎回「問題あり」になっていた。実機 round 1）。**その常態のほうを
		// 直した**——鉛直材は両端の絶対 Z、水平材は PIO のパスで測れるので、ここが埋まるのは
		// 検査が空振りしているときだけになった（docs/DEV-NOTES.md「水平材の実体は
		// 「スパン」では測れない」）。
		if (!failures.extentHint.empty())
			note += "描き上がった実体を測れませんでした（" + failures.extentHint + "）。";
#endif
		count("端部オフセットを設定できなかった", failures.offset);
#if VW_DRAW_VERIFY
		if (failures.offset > 0 && !failures.offsetHint.empty())
			note += "（候補: " + failures.offsetHint + "）";
#endif
		return note;
	}

} // namespace HomeskzIfcImport::draw
