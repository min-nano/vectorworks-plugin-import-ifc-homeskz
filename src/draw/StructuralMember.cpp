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
//	【スタイルは関連付けだけでは効かない】ISDK の SetPluginObjectStyle はスタイルの関連付け
//	（パラメータ）までで、スタイルが決める描画属性（コンポーネントのクラス／マテリアル）
//	はオブジェクトへプッシュされない。全配置後に UpdateStyledObjects を 1 回呼ぶのは呼び出し側
//	の責務（横架材・柱でスタイルが別なので、ここでは行わない）。個別フィールドはスタイル関連付
//	けの**後**に設定するので、スタイル既定のパラメータは本命令の実測値で上書きされる。
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
		const std::vector<const char*> kStartElevationNames = {"StartElevation"};
		const std::vector<const char*> kEndElevationNames = {"EndElevation"};
		const std::vector<const char*> kNoLocalized = {};
		// **水平材の実体はパラメータでは測らない。** OIP に「スパン」は無く（universal
		// `Span` もローカライズ名「スパン」も実機で引けなかった。round 1）、名前で
		// 引ける `CenterPointLength(長さ)` は部材長ではない（実長 5333 の柱で 100）。
		// 代わりに**PIO が実際に持っているパスの両端の距離**（DrawUtil の `PioPathChord`）で
		// 測る——`ResetObject` が解決済みバウンドから作り直したパスが、そのまま「描かれた
		// 実体」だからである（ヘッダ StructuralExtentKind ／ Findings「Parametric Objects」）。
		//
		// 名前に「長さ」「高さ」「スパン」を含むパラメータを**名前と値で**並べて証拠にする
		// ときの手掛かり（DescribeSizeParams）。どのパラメータが OIP のどの欄なのかを実機で
		// 確かめる手段がほかに無い——実際、round 2 のこの一覧で「長さ」で引ける
		// `CenterPointLength` が部材長ではないと分かり、round 1 のこの一覧に「スパン」が
		// 1 件も出なかったことで、測る相手をパラメータからパスへ変える根拠になった。
		// **診断にしか使わない**ので開発ビルドだけ（draw/Verify.h）。
#if VW_DRAW_VERIFY
		const std::vector<const char*> kSizeParamNeedles = {"長さ", "高さ", "スパン", "Span"};
#endif
		// 「長さ 0」とみなす閾値（mm）。潰れた部材はちょうど 0 を返すので、実部材の長さ
		// （最短でも数十 mm）と取り違える余地は無い。
		constexpr double kCollapsedLength = 0.01;
		// 描かれた絶対 Z が命令と「合っている」とみなす許容（mm）。丸めのぶんだけで、
		// 意味のあるずれ（レイヤ原点へ落ちる・階ぶん動く）とは桁が違う。**高さの検算は
		// 開発ビルドだけ**（draw/Verify.h）。
#if VW_DRAW_VERIFY
		constexpr double kElevationTol = 1.0;
#endif

		// フィールドに渡す値（ポップアップはキーで保持されるため数値文字列）。
		constexpr const char* kProfileShapeRectangle = "Rectangle";
		// 部材種別は横架材（梁）・柱とも "2"（種別の違いは構造用途＝StructuralUse の方に出る）。
		constexpr const char* kMemberTypeStructural = "2";
		// 断面基準点は 3×3 グリッドを 0 始まり・行優先（上段 0,1,2 / 中段 3,4,5 / 下段
		// 6,7,8）で並べたキー。天端中央＝1・中央＝4 は実機確認済みで、中下＝7 はその並びから
		// 採った（ローカル確認の項目。docs/DEV-NOTES.md M16）。
		constexpr const char* kAxisAlignTopCentre = "1"; // 天端中央（3×3 グリッドの上段中央）
		constexpr const char* kAxisAlignCentre = "4";		// 中央（同 0 始まり中央）
		constexpr const char* kAxisAlignBottomCentre = "7"; // 中下（同 下段中央）
		constexpr const char* kEndConditionSquare = "3";	// 直切り
		constexpr const char* kProfileSeriesDefault = "AISC (Inch)";

		// 実数パラメータを読む。**実数で 0 を読んだだけでは 0 と決めない**——数値パラメータが
		// 文字列で保持されている PIO があり（SetParamRealChecked が実際にその経路を持つ）、
		// そのときは実数読みが常に 0 を返して**全数を誤報する**。文字列でも読み直し、数として
		// 読めたらそちらを採る。読めないパラメータを覗くと例外が出るので畳む（1 つの読み損
		// ないで描画を止めない。DrawUtil の PioParamString と同じ扱い）。ok には読めたかを返す。
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

		// 断面基準点 → 構造材ツールのポップアップのキー。
		const char* AxisAlignKey(StructuralAxisAlign align)
		{
			switch (align)
			{
			case StructuralAxisAlign::Centre:
				return kAxisAlignCentre;
			case StructuralAxisAlign::BottomCentre:
				return kAxisAlignBottomCentre;
			case StructuralAxisAlign::TopCentre:
			default:
				return kAxisAlignTopCentre;
			}
		}
	} // namespace

#if VW_DRAW_VERIFY
	MCObjectHandle CreatePath(const core::Vec2& start, const core::Vec2& end, bool& outAppended,
							  PathProbe* outProbe)
#else
	MCObjectHandle CreatePath(const core::Vec2& start, const core::Vec2& end, bool& outAppended)
#endif
	{
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

	StructuralMemberResult DrawStructuralMember(const StructuralMemberSpec& spec, RefNumber style)
	{
		StructuralMemberResult result;
		// **空のプロファイルを渡してはならない**（断面が無いのと同じで、PIO は生成できても
		// 実体が描かれず「オブジェクトはあるのに画面に何も出ない」状態になる。
		// draw/DrawUtil の CreateRectangleProfileGroup 参照）。
		if (spec.path == nil || spec.profile == nil)
			return result;

		MCObjectHandle object =
			gSDK->CreateCustomObjectPath(kStructuralMember, spec.path, spec.profile, true);
		if (object == nil)
			return result;

		SetClassByName(object, spec.drawClass);
		SetAllAttributesByClass(object);
		// スタイルは個別フィールドより**先に**関連付ける（後に設定する実測値で
		// スタイル既定のパラメータを上書きするため）。
		if (style != 0)
			gSDK->SetPluginObjectStyle(object, style);

		// 高さ基準を始端（0）・終端（1）それぞれのストーリレベルへバインドする。これで
		// 構造材ツールの高さ基準が「レイヤの高さ」・offset 0 のまま実ジオメトリと矛盾する
		// ことがなくなり、編集時に高さがリセットされない。水平材の傾斜はこの offset 差で
		// 表れ、鉛直材ではこの差が柱高さを支配する。
		// **戻り値を見る。** 受け取られなければ材は高さを持てない（＝実体が無い材になる）。
		const bool startBoundOk = ApplyStoryBound(object, kStartBoundID, spec.startBound);
		const bool endBoundOk = ApplyStoryBound(object, kEndBoundID, spec.endBound);
		result.boundOk = startBoundOk && endBoundOk;

		VWParametricObj pio(object);
		const TXString breadth = ResolveParamName(pio, kFieldMajorBreadth, kLocalizedBreadth);
		const TXString depth = ResolveParamName(pio, kFieldMajorDepth, kLocalizedDepth);

		pio.SetParamAsString(ResolveParamName(pio, kFieldProfileShape, kLocalizedProfileShape),
							 kProfileShapeRectangle);
		pio.SetParamAsString(kFieldProfileSeries, kProfileSeriesDefault);
		const bool breadthOk = SetParamRealChecked(pio, breadth, spec.width);
		const bool depthOk = SetParamRealChecked(pio, depth, spec.depth);
		// B / D は矩形断面のときの別名。上と同じ値を入れる（存在しなければ無視される）。
		SetParamRealChecked(pio, ResolveParamName(pio, kFieldB, kLocalizedBreadth), spec.width);
		SetParamRealChecked(pio, ResolveParamName(pio, kFieldD, kLocalizedDepth), spec.depth);
		pio.SetParamAsString(kFieldMemberID, TXString(spec.memberId.c_str()));
		pio.SetParamAsString(kFieldMemberType, kMemberTypeStructural);
		pio.SetParamAsString(kFieldStructuralUse, TXString(spec.structuralUse.c_str()));
		pio.SetParamAsString(kFieldAxisAlign, AxisAlignKey(spec.axisAlign));
		pio.SetParamAsString(kFieldStartCondition, kEndConditionSquare);
		pio.SetParamAsString(kFieldEndCondition, kEndConditionSquare);

		// 端部オフセット。**要らない（両端 0）なら触らない**——スタイル既定が 0 なので書く
		// 必要が無く、名前を解決できない構成でも余計な診断を出さずに済む。
		if (spec.startOffset != 0.0 || spec.endOffset != 0.0)
		{
			const TXString startOffset =
				ResolveParamNameAmong(pio, kStartOffsetNames, kLocalizedStartOffset);
			const TXString endOffset =
				ResolveParamNameAmong(pio, kEndOffsetNames, kLocalizedEndOffset);
			if (startOffset.IsEmpty() || endOffset.IsEmpty())
			{
				result.endOffsetOk = false;
#if VW_DRAW_VERIFY
				result.offsetParamHint = DescribeParamsContaining(pio, kOffsetParamNeedle);
#endif
			}
			else
			{
				const bool startOk = SetParamRealChecked(pio, startOffset, spec.startOffset);
				const bool endOk = SetParamRealChecked(pio, endOffset, spec.endOffset);
				result.endOffsetOk = startOk && endOk;
			}
		}
		gSDK->ResetObject(object);

		// 【描けたかを読み戻す】PIO は生成できても実体を持たないことがある（パスが 1 点の
		// まま・バウンドの解決に失敗、など。Findings「Parametric Objects」）。そのとき OIP の
		// 高さ・基準・オフセットは命令どおりのままなので、**画面を見ない限り気付けない**。
		// リセット後の「長さ」を読み戻し、0 で潰れていたら呼び出し側の診断へ流す。
		//
		// **測る理由が 2 つある**ので、本番ビルドに残すのは片方だけである（draw/Verify.h）。
		//   * 検算（潰れ・高さのずれを件数と実測で持ち帰る）… 開発ビルドだけ。
		//   * **自己修復の引き金**（潰れていたらパスを作り直して差し替える）… 本番でも要る
		//     ——外すと実機で潰れた材がそのまま残る＝利用者の絵が変わる。
		// したがって本番は `retryWithFreshPath` の材だけを測り、結果は診断へ出さない。
#if VW_DRAW_VERIFY
		const bool measureDrawn = spec.expectedLength > kCollapsedLength || spec.checkElevation;
#else
		const bool measureDrawn = spec.retryWithFreshPath && spec.expectedLength > kCollapsedLength;
#endif
		if (measureDrawn)
		{
			// 本番ビルドでは差し替え後の測り直しを控えないので、そのまま const になる。
#if VW_DRAW_VERIFY
			DrawnMemberSize size = MeasureDrawnMember(object, spec.extentKind);
#else
			const DrawnMemberSize size = MeasureDrawnMember(object, spec.extentKind);
#endif
			if (spec.expectedLength > kCollapsedLength)
			{
#if VW_DRAW_VERIFY
				// **測れなかったときだけ手掛かりを採る。** 鉛直材なら両端の絶対 Z を、
				// 水平材なら PIO のパスを引けなかったということなので、パラメータの顔ぶれと
				// 図面のパスを両方並べる——原因をどちら側に分けるかは、実機でしか読めない
				// この 1 行にしか手掛かりが無い（ヘッダ extentHint）。
				if (!size.found)
					result.extentHint = DescribeSizeParams(object) + "・図面のパス[" +
										DescribePioPath(object) + "]";
#endif
				result.collapsed = size.zero;
				// **潰れていたら証拠を全部採る。** 高さ基準は**どう書いても両端の Z を動かせ
				// なかった**（ストーリ相対・レイヤ基準・VW が記録しているとおり、のいずれでも
				// 実測 0。docs/DEV-NOTES.md「柱が長さ 0 で描かれる（M27）」）ので、残る入力は
				// パスである。**PIO が実際に持っているパスの頂点**まで読み戻して添える。
#if VW_DRAW_VERIFY
				if (result.collapsed)
					result.collapsedProbe = DescribeSizeParams(object) + "・図面の始端基準[" +
											DescribeStoryBound(object, kStartBoundID) +
											"]・終端基準[" +
											DescribeStoryBound(object, kEndBoundID) +
											"]・図面のパス[" + DescribePioPath(object) + "]";
#endif
				// **潰れていたらパスを作り直して差し替える。** 渡した曲線が正しくても PIO の中の
				// パスが潰れていることがある（実機 round 7）。直ったかは読み戻して見る。
				if (result.collapsed && spec.retryWithFreshPath)
				{
					bool appended = false;
					const MCObjectHandle fresh = CreatePath(spec.pathStart, spec.pathEnd, appended);
					if (fresh != nil && gSDK->SetCustomObjectPath(object, fresh))
					{
						gSDK->ResetObject(object);
						// **作り直した曲線の後始末。** `CreateNurbsCurve` は曲線を**図面へ**作るので、
						// PIO がそれを引き取らなかったなら、消さない限り図面に残り続ける——潰れる柱は
						// 実機で 46 本あるので、放っておけば取り込みのたびにその数だけ原点に立った
						// 線が積み上がる。**引き取ったかどうかは推測しない**——PIO がいま持っている
						// パス（`GetCustomObjectPath`）が渡した曲線そのものなら引き取られており、
						// 消せば材のパスを消すことになる。違う実体なら VW が複製したということで、
						// 渡した曲線はこちらの後始末である。どちらだったかは診断にも残す（実機で
						// しか分からない挙動なので、次の周が答えを持ち帰る）。
						const MCObjectHandle adopted = gSDK->GetCustomObjectPath(object);
						const bool taken = adopted == fresh;
						if (!taken)
							gSDK->DeleteObject(fresh, true /* useUndo: 取り込みのイベントへ登録 */);
						const DrawnMemberSize retried = MeasureDrawnMember(object, spec.extentKind);
#if VW_DRAW_VERIFY
						std::array<char, 160> buffer{};
						// **測り方によって添える値を変える**（水平材の Z は両端が等しいのが
						// 正常なので、並べても読む側を惑わせるだけ。ヘッダ
						// StructuralExtentKind）。
						if (spec.extentKind == StructuralExtentKind::Horizontal)
							std::snprintf(
								buffer.data(), buffer.size(),
								"・パスを作り直した結果 実測 %g（パス長）・作り直したパス[",
								retried.extent);
						else
							std::snprintf(
								buffer.data(), buffer.size(),
								"・パスを作り直した結果 実測 %g（Z %g→%g）・作り直したパス[",
								retried.extent, retried.start, retried.end);
						result.collapsedProbe +=
							std::string(buffer.data()) + DescribePioPath(object) +
							(taken ? "]・作り直した曲線は PIO が引き取った"
								   : "]・作り直した曲線は複製されたので消した");
#endif
						if (retried.found && !retried.zero)
						{
							result.repairedByPath = true;
							result.collapsed = false;
						}
#if VW_DRAW_VERIFY
						// 下の高さの検算は**差し替えたあとの図面**を見る（差し替えで Z も
						// 変わりうるので、古い実測で判定すると診断が嘘をつく）。
						if (retried.found || retried.elevationRead)
							size = retried;
#endif
					}
					else
					{
						// 差し替えられなかったときは、作った曲線が確実に**こちらのもの**として
						// 図面に残る（PIO は受け取っていない）ので必ず消す。
						if (fresh != nil)
							gSDK->DeleteObject(fresh, true);
#if VW_DRAW_VERIFY
						result.collapsedProbe += "・パスを作り直して差し替えられなかった";
#endif
					}
				}
			}

			// 【描かれた高さが命令どおりか】**パスから Z を外したぶんの見張り**である
			// （ヘッダ冒頭「パスは 2D で渡す」）。高さを決めるのがストーリバウンドだけに
			// なった以上、その解決が意図とずれても**本数にもスパンにも一切出ない**——材が
			// 揃って違う高さに並ぶだけなので、実機の絵を見るまで気付けない。そこで読み戻した
			// 両端の絶対 Z を命令と引き比べ、ずれた本数と 1 件目の実測を持ち帰る。
			// **読めなかったときは「ずれた」に数えない**（測れていないことを不具合として
			// 報せると、切り分けが逆に遠のく）。**検算そのものなので開発ビルドだけ**
			// （draw/Verify.h）。
#if VW_DRAW_VERIFY
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
#endif
		}

		result.object = object;
		result.sectionOk = breadthOk && depthOk;
		return result;
	}

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
			//
			// **本番ビルドの水平材では読まない。** そこでは高さの検算が畳まれていて使い道が
			// 無いのに、名前の解決はパラメータ表を 1 本につき 2 回舐める（材は数百本ある）。
#if VW_DRAW_VERIFY
			constexpr bool kElevationAlways = true; // 開発ビルドは kind に関わらず読む
#else
			constexpr bool kElevationAlways = false;
#endif
			if (kElevationAlways || kind == StructuralExtentKind::Vertical)
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
					std::any_of(kSizeParamNeedles.begin(), kSizeParamNeedles.end(),
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

} // namespace HomeskzIfcImport::draw
