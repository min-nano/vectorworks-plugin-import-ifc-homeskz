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
		// 水平材の実体（OIP「スパン」）。**書くためではなく読み戻して確かめるため**の名前で、
		// 0 のままなら PIO がパスの長さを取れていない＝画面に何も描かれない。水平材は両端の
		// Z が等しいので上の差では測れない（ヘッダ StructuralExtentKind）。以前は
		// draw/Member.cpp が同じ名前を別に持っていたが、測る口はここ 1 つに寄せた
		// （CLAUDE.md「重複を作らない置き場所」）。
		const std::vector<const char*> kSpanNames = {"Span"};
		const std::vector<const char*> kLocalizedSpan = {"スパン"};
		const std::vector<const char*> kNoLocalized = {};
		// 名前に「長さ」「高さ」を含むパラメータを**名前と値で**並べて証拠にするときの手掛かり
		// （DescribeSizeParams / DescribeParamsContaining）。どのパラメータが OIP のどの欄
		// なのかを実機で確かめる手段がほかに無い——実際、round 2 のこの一覧で「長さ」で引ける
		// `CenterPointLength` が部材長ではないと分かった。**どちらも診断にしか使わない**ので
		// 開発ビルドだけ（draw/Verify.h）。
#if VW_DRAW_VERIFY
		constexpr const char* kLengthParamNeedle = "長さ";
		constexpr const char* kHeightParamNeedle = "高さ";
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

		// **計測の区間を開かないパス生成の本体。** 区間を開くのは下の公開版（CreatePath）
		// だけで、こちらは**既に別の区間の中にいる**呼び出し——潰れた材の自己修復
		// （DrawStructuralMember の「構造材:読み戻しと修復」の中）——が使う。
		// **区間は入れ子にしない**（core/DrawTiming.h「使う側の作法」／draw/Verify.h）
		// ——入れ子にすると自己修復に掛かった時間が「構造材:パス生成」と
		// 「構造材:読み戻しと修復」へ二重に積まれ、合計を読んだ人が必ず取り違える。
		// 実機では潰れた柱が 46 本あった（docs/DEV-NOTES.md M27）ので、この経路は稀ではない。
#if VW_DRAW_VERIFY
		MCObjectHandle CreatePathUntimed(const core::Vec2& start, const core::Vec2& end,
										 bool& outAppended, PathProbe* outProbe = nullptr)
#else
		MCObjectHandle CreatePathUntimed(const core::Vec2& start, const core::Vec2& end,
										 bool& outAppended)
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
	} // namespace

	// 公開版。**区間を開くのはここだけ**（上記 CreatePathUntimed）——呼ぶのは要素ごとの
	// 描画（draw/Member・draw/Column・draw/Rafter）で、どれも他の区間の外から呼ぶ。
#if VW_DRAW_VERIFY
	MCObjectHandle CreatePath(const core::Vec2& start, const core::Vec2& end, bool& outAppended,
							  PathProbe* outProbe)
	{
		VW_DRAW_TIME("構造材:パス生成");
		return CreatePathUntimed(start, end, outAppended, outProbe);
	}
#else
	MCObjectHandle CreatePath(const core::Vec2& start, const core::Vec2& end, bool& outAppended)
	{
		VW_DRAW_TIME("構造材:パス生成");
		return CreatePathUntimed(start, end, outAppended);
	}
#endif

	StructuralMemberResult DrawStructuralMember(const StructuralMemberSpec& spec, RefNumber style)
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
				// `doRegen` はヘッダ StructuralMemberSpec::regenOnCreate（false なら作り直しは
				// この後の ResetObject の 1 回だけになる）。
				object = gSDK->CreateCustomObjectPath(kStructuralMember, spec.path, spec.profile,
													  spec.regenOnCreate);
			}
			if (object == nil)
				return result;
			// 既定を継いだかを読み戻し、継いでいなければ従来どおり与え直す（中で区間を開く
			// ので、ここは包まない）。
			result.classInherited = FinishCreatedWithClass(object, creationClass, spec.drawClass);
		}
		// スタイルは個別フィールドより**先に**関連付ける（後に設定する実測値で
		// スタイル既定のパラメータを上書きするため）。
		if (style != 0)
		{
			VW_DRAW_TIME("構造材:スタイル関連付け");
			gSDK->SetPluginObjectStyle(object, style);
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
			// **読み戻しと自己修復はひとまとめの区間**にする（区間は入れ子にしない。
			// draw/Verify.h）。潰れた材のパスを作り直す `ResetObject` もここに入るので、
			// 「構造材:リセット」には**1 本につき 1 回ぶんだけ**が積まれる。
			VW_DRAW_TIME("構造材:読み戻しと修復");

			// 本番ビルドでは差し替え後の測り直しを控えないので、そのまま const になる。
#if VW_DRAW_VERIFY
			DrawnMemberSize size = MeasureDrawnMember(object, spec.extentKind);
#else
			const DrawnMemberSize size = MeasureDrawnMember(object, spec.extentKind);
#endif
			if (spec.expectedLength > kCollapsedLength)
			{
#if VW_DRAW_VERIFY
				if (!size.found)
					result.lengthParamHint = DescribeParamsContaining(pio, kLengthParamNeedle);
#endif
				result.collapsed = size.zero;
				// **潰れていたら証拠を全部採る。** 高さ基準は**どう書いても両端の Z を動かせ
				// なかった**（ストーリ相対・レイヤ基準・VW が記録しているとおり、のいずれでも
				// 実測 0。docs/DEV-NOTES.md「柱が長さ 0 で描かれる（M27）」）ので、残る入力は
				// パスである。**PIO が実際に持っているパスの頂点**まで読み戻して添える。
#if VW_DRAW_VERIFY
				if (result.collapsed)
					result.collapsedProbe = DescribeSizeParams(object) + "・図面の始端基準[" +
											DescribeStoryBound(object, StoryBoundSlot::Start) +
											"]・終端基準[" +
											DescribeStoryBound(object, StoryBoundSlot::End) +
											"]・図面のパス[" + DescribePioPath(object) + "]";
#endif
				// **潰れていたらパスを作り直して差し替える。** 渡した曲線が正しくても PIO の中の
				// パスが潰れていることがある（実機 round 7）。直ったかは読み戻して見る。
				if (result.collapsed && spec.retryWithFreshPath)
				{
					bool appended = false;
					// **公開版（CreatePath）ではなく計測を開かない方を呼ぶ。** ここは既に
					// 「構造材:読み戻しと修復」の区間の中なので、公開版を呼ぶと区間が
					// 入れ子になる（上記 CreatePathUntimed）。
					const MCObjectHandle fresh =
						CreatePathUntimed(spec.pathStart, spec.pathEnd, appended);
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
						if (spec.extentKind == StructuralExtentKind::Span)
							std::snprintf(
								buffer.data(), buffer.size(),
								"・パスを作り直した結果 実測 %g（スパン）・作り直したパス[",
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
			// **両端の解決済み絶対 Z は kind に依らず読む。** 鉛直材はこの差が実体そのもの
			// だが、水平材でも**描かれた高さが命令どおりか**の検算に要る——パスから Z を
			// 外した（ヘッダ冒頭「パスは 2D で渡す」）いま、高さを言える値はこの 2 つしか
			// 残っていない。
			const TXString startName =
				ResolveParamNameAmong(pio, kStartElevationNames, kNoLocalized);
			const TXString endName = ResolveParamNameAmong(pio, kEndElevationNames, kNoLocalized);
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
			if (kind == StructuralExtentKind::Span)
			{
				// 水平材。**パラメータが実在するときだけ測る**——ResolveParamNameAmong は
				// 見つからなくても候補の先頭を返し、GetParamReal は存在しない名前に 0 を返す
				// ので、存在確認を落とすと「スパン」という名前が違うだけで**パスは正常なのに
				// 全数を長さ 0 と誤報**する（診断が嘘をつくと切り分けが逆に遠のく）。
				const TXString spanName = ResolveParamNameAmong(pio, kSpanNames, kLocalizedSpan);
				if (spanName.IsEmpty() || pio.GetParamIndex(spanName) == static_cast<size_t>(-1))
					return size;
				bool spanOk = false;
				const double span = ReadParamNumber(pio, spanName, spanOk);
				if (!spanOk)
					return size;
				size.found = true;
				size.extent = std::abs(span);
				size.zero = size.extent < kCollapsedLength;
				return size;
			}
			if (!size.elevationRead)
				return size;
			size.found = true;
			size.extent = std::abs(end - start);
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
				if (universal.find(kLengthParamNeedle) == std::string::npos &&
					localized.find(kLengthParamNeedle) == std::string::npos &&
					universal.find(kHeightParamNeedle) == std::string::npos &&
					localized.find(kHeightParamNeedle) == std::string::npos)
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
		if (result.repairedByPath)
			++repaired;
		if ((result.collapsed || result.repairedByPath) && collapsedProbe.empty())
			collapsedProbe = result.collapsedProbe;
		if (lengthHint.empty())
			lengthHint = result.lengthParamHint;
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
		count("パスを作り直して直った", failures.repaired);
		// 作る前に立てた既定（クラス・描画属性）を継がずに生まれ、作った後に与え直した本数。
		// 0 でなければその本数ぶん高速化が効いていない（絵は従来どおり）。
		count("既定のクラスを継がず作った後に与え直した", failures.classFallback);
		if (!failures.collapsedProbe.empty())
			note += "（1 本目: " + failures.collapsedProbe + "）";
		count("命令と違う高さに描かれた", failures.elevation);
		if (failures.elevation > 0 && !failures.elevationProbe.empty())
			note += "（1 本目: " + failures.elevationProbe + "）";
		// **手掛かりは、説明すべき失敗があるときだけ出す。** 実体を測るパラメータ名は水平材
		// （OIP の「スパン」）では実機に無く、引けないのが常態である——無条件に出すと健全な
		// 周が毎回「問題あり」になる（実機 round 1。CLAUDE.md「異常は diagnostics・平常でも
		// 出る記録は notes」）。潰れ・作り直しと同じ条件で添えれば、M27 のように名前を突き
		// 止めたい場面では従来どおり出る。
		if ((failures.collapsed > 0 || failures.repaired > 0) && !failures.lengthHint.empty())
			note += "「長さ」パラメータを引けませんでした（候補: " + failures.lengthHint + "）。";
#endif
		count("端部オフセットを設定できなかった", failures.offset);
#if VW_DRAW_VERIFY
		if (failures.offset > 0 && !failures.offsetHint.empty())
			note += "（候補: " + failures.offsetHint + "）";
#endif
		return note;
	}

} // namespace HomeskzIfcImport::draw
