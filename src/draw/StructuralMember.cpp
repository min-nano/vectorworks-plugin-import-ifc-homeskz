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

		// 鉛直パス（NURBS 曲線）の次数。直線 1 本なので 1。byCtrlPts=false ＝ 通過点で定義す
		// る。
		constexpr short kPathDegree = 1;
		// 鉛直パスに必要な頂点数（下端・上端）。読み戻して診断に使う。
		constexpr Sint32 kPathPointCount = 2;

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
		constexpr const char* kOffsetParamNeedle = "オフセット";

		// 描き上がった部材の長さ（OIP の「長さ」）。**端部オフセットと同じく universal 名が
		// SDK ヘッダに無い**ので候補を並べて引き、引けなければ手掛かりを診断へ持ち帰る。
		// 読むだけで、書きはしない（PIO がリセットのたびに入れる値）。
		// **両端の解決済み絶対 Z**（実機 round 2 で判明。ヘッダ DrawnMemberSize 参照）。
		// **ローカライズ名で引かない**——「始端高さオフセット」は設定ダイアログ側の
		// `DialogStartElevation`（別物。レイヤの高さ基準で 0 を返す）とぶつかる。
		const std::vector<const char*> kStartElevationNames = {"StartElevation"};
		const std::vector<const char*> kEndElevationNames = {"EndElevation"};
		const std::vector<const char*> kNoLocalized = {};
		constexpr const char* kLengthParamNeedle = "長さ";
		// 潰れていたときは、名前に「長さ」「高さ」を含むパラメータを**名前と値で**並べて
		// 証拠にする（DescribeSizeParams）。どのパラメータが OIP のどの欄なのかを実機で
		// 確かめる手段がほかに無い——実際、round 2 のこの一覧で「長さ」で引ける
		// `CenterPointLength` が部材長ではないと分かった。
		constexpr const char* kHeightParamNeedle = "高さ";
		// 「長さ 0」とみなす閾値（mm）。潰れた部材はちょうど 0 を返すので、実部材の長さ
		// （最短でも数十 mm）と取り違える余地は無い。
		constexpr double kCollapsedLength = 0.01;

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
		// （DrawUtil の PioParamString と同じ扱い）。
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

	MCObjectHandle CreatePath(const core::Vec3& start, const core::Vec3& end, bool& outAppended,
							  PathProbe* outProbe)
	{
		outAppended = false;
		MCObjectHandle path =
			gSDK->CreateNurbsCurve(WorldPt3(start.x, start.y, start.z), false, kPathDegree);
		if (path == nil)
			return nil;

		// **Add3DVertex が VS の AddVertex3D にあたる**（ヘッダ参照）。末尾へ 1 点足して
		// 始端 → 終端の 2 点にする。
		gSDK->Add3DVertex(path, WorldPt3(end.x, end.y, end.z));
		// 頂点が本当に 2 つになったかを読み戻す。ピース索引の起点は 0 / 1 のどちらの
		// 規約もあり得るので両方を見る（**判定に失敗しても曲線はそのまま使う**——ここで
		// 諦めると、索引の規約違いというだけで部材が 1 本も描かれなくなる）。
		const Sint32 piece0 = gSDK->NurbsGetNumPts(path, 0);
		const Sint32 piece1 = gSDK->NurbsGetNumPts(path, 1);
		if (outProbe != nullptr)
			*outProbe = PathProbe{piece0, piece1};
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
		gSDK->SetObjectStoryBound(object, kStartBoundID, StoryBoundData(spec.startBound));
		gSDK->SetObjectStoryBound(object, kEndBoundID, StoryBoundData(spec.endBound));

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
				result.offsetParamHint = DescribeParamsContaining(pio, kOffsetParamNeedle);
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
		if (spec.expectedLength > kCollapsedLength)
		{
			const DrawnMemberSize size = MeasureDrawnMember(object);
			if (!size.found)
				result.lengthParamHint = DescribeParamsContaining(pio, kLengthParamNeedle);
			result.collapsed = size.zero;
			if (result.collapsed)
				result.collapsedProbe = DescribeSizeParams(object);
		}

		result.object = object;
		result.sectionOk = breadthOk && depthOk;
		return result;
	}

	DrawnMemberSize MeasureDrawnMember(MCObjectHandle object)
	{
		DrawnMemberSize size;
		if (object == nil)
			return size;
		try
		{
			const VWParametricObj pio(object);
			const TXString startName =
				ResolveParamNameAmong(pio, kStartElevationNames, kNoLocalized);
			const TXString endName = ResolveParamNameAmong(pio, kEndElevationNames, kNoLocalized);
			bool startOk = false;
			bool endOk = false;
			const double start = ReadParamNumber(pio, startName, startOk);
			const double end = ReadParamNumber(pio, endName, endOk);
			if (!startOk || !endOk)
				return size;
			size.found = true;
			size.start = start;
			size.end = end;
			size.extent = std::abs(end - start);
			size.zero = size.extent < kCollapsedLength;
		}
		catch (...)
		{
			return DrawnMemberSize{}; // 読めなかった＝測れていない
		}
		return size;
	}

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

	bool RetryCollapsedMember(MCObjectHandle object, const core::StoryBoundCommand& startBound,
							  const core::StoryBoundCommand& endBound)
	{
		if (object == nil)
			return false;

		// 高さ基準を**同じ値で**入れ直してから解かせ直す（ヘッダ参照）。指定は正しいので、
		// 値を変える必要は無い——変えれば OIP に残る値まで変わってしまう。
		gSDK->SetObjectStoryBound(object, kStartBoundID, StoryBoundData(startBound));
		gSDK->SetObjectStoryBound(object, kEndBoundID, StoryBoundData(endBound));
		gSDK->ResetObject(object);

		const DrawnMemberSize size = MeasureDrawnMember(object);
		return size.found && !size.zero; // 測れないなら直ったと言わない
	}
} // namespace HomeskzIfcImport::draw
