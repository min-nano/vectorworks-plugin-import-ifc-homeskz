//
//	draw/Rafter.cpp
//
//	垂木描画の実装。命令セット（RafterCommand）を**軸組ツール（FramingMember、部材種別
//	rafter）**のオブジェクトとして配置する。Python 版 vw/rafter.py に対応する。
//	【SDK 依存】PluginPrefix.h（VectorWorks SDK）を include するため、この翻訳単位は
//	プラグインビルド（SDK あり）でのみコンパイルされ、無 SDK の core/parse ライブラリには
//	入れない（CLAUDE.md「依存の向きは厳守する」）。
//
//	描画手順（Python 版 draw_rafter と同じ意図。実現手段は SDK の作法に合わせる）:
//	  1. 命令の軒側 start（＝支持点）・棟側 end の平面座標と両端の天端 Z から、**水平投影長
//	     （スパン）・平面方位角・勾配**を求める。
//	  2. **配置行列**（Z 軸まわりに平面方位角だけ回し、支持点の XY へ平行移動）から軸組ツールの
//	     PIO を生成する。Python 版は「原点に生成 → Rotate3D → Move3D」の 3 手順だが、ISDK には
//	     VectorScript の 3D 変換状態（ResetOrientation3D / Rotate3D）が無く、代わりに配置行列
//	     から直接 PIO を作れる（CreateCustomObjectByMatrix）。勾配は本体の勾配パラメータが担い、
//	     支持点（下端基準）から棟側へ立ち上がる。
//	  3. クラス（小屋組-垂木）を割り当て、描画属性をすべてクラス属性に従わせる。
//	  4. 断面・配置・2D 表示・軒の出・差し込み・仕様ラベル・構造用途・材質の各パラメータを
//	     設定して ResetObject で反映する。
//	  5. 高さを 3D 移動で与える（MoveObject3D。下記）。
//	PIO を生成できない場合は平面投影の直線にフォールバックする（1 本の失敗で全体を止めない）。
//
//	【高さは配置行列ではなく 3D 移動で与える】配置行列のオフセット Z は PIO に**届かない**
//	（ローカル確認: 与えた 6109.0 に対し読み戻しは 2.0、OIP の Z も 0）。高さは ResetObject の
//	後に MoveObject3D で与える（Python 版 Move3D と同じ手）。オブジェクトの Z はレイヤ座標
//	なので、命令の絶対 Z からレイヤの基準高さ（ovLayerHeightInCurrUnits）を引いてレイヤ相対に
//	直してから渡す。Z の計算は 1 か所（DrawOne の MoveObject3D）に集約してある。
//
//	【フィールド名は VW 実機の登録から採る】軸組ツールのパラメータ名は、当初 Python 版の
//	VectorScript エクスポートから推定していたが、ローカル確認で PIO の全パラメータの universal
//	名とローカライズ名を出したところ、勾配（pitch ではなく **PitchAngle**。pitch という名前の
//	内部パラメータが別にあり、そちらは __NNA_DO_NOT_CHANGE）・構造用途（**先頭が小文字**の
//	structuralUse）・ラベル（label ではなく **showLabel** ＋ **labelText** の 2 つ）が名前違いで
//	**黙って無視されていた**ことが判明した。スパン（LineLength）は水平投影長で、部材に沿った
//	実長は別パラメータ（LineLengthReal）。ポップアップの値は表示文字ではなく**キー**を渡す。
//	最終挙動は VW 実機で確認する（ROADMAP.md M6「ローカル確認」）。名前付き定数に集約する。
//

#include "PluginPrefix.h"
#include "draw/Rafter.h"
#include "core/Document.h"

// 配置行列（VWTransformMatrix）と PIO パラメータ設定（VWParametricObj）。フォールバックの
// 直線は draw/Grid.cpp と同じ VWPolygon2DObj で描く。
#include "VWFC/Math/VWTransformMatrix.h"
#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWPolygon2DObj.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <numbers>
#include <utility>
#include <vector>
#include <string>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// ラジアン → 度（M_PI は MSVC で既定では未定義なので C++20 の std::numbers を使う）。
		constexpr double kDegreesPerRadian = 180.0 / std::numbers::pi;

		// 勾配の読み戻し確認の許容差（度）。これを超えて食い違えば実数で入らなかったとみなす。
		constexpr double kPitchTolerance = 1e-3;

		// 軸組ツールの PIO 名と部材種別（垂木は 'rafter'）。VW の登録名に一致させる。
		const TXString kFramingMember("FramingMember");

		// PIO 生成時に「オブジェクトの設定」ダイアログを出さない設定（Python 版
		// CreateCustomObjectN(showPref=False) 相当）。ISDK の CreateCustomObject 系には
		// VectorScript のような showPref 引数が無く、代わりに PIO ごとの「設定ダイアログを
		// いつ出すか」を DefineCustomObject で切り替える（kCustomObjectPrefNever＝
		// 「生成時に決して出さない」。Kernel/API/MiniCadCallBacks.h）。これを呼ばないと
		// インポート中に垂木 1 本ごとにダイアログが開いて手入力を求められる（ローカル確認で判明）。
		void SuppressSettingsDialog()
		{
			gSDK->DefineCustomObject(kFramingMember, kCustomObjectPrefNever);
		}
		constexpr const char* kMemberTypeRafter = "rafter";

		// 垂直基準（verticalReference）。軒側（下端基準）から棟へ立ち上がる。
		constexpr const char* kVerticalReferenceBottom = "bottom";
		// 2D 表示（2DDisplay）。要件により「幅」表示にする。
		constexpr const char* k2DDisplayWidth = "width";

		// 以下のフィールド名は VW 実機の FramingMember 登録から採った（ローカル確認で PIO の
		// 全パラメータの universal 名とローカライズ名をダイアログに出して確定させた。それまでは
		// Python 版の VectorScript エクスポートから推定した名前を使っており、勾配・構造用途・
		// ラベルが**名前違いで黙って無視されていた**）。括弧内は実機のローカライズ名。
		constexpr const char* kFieldType = "type";			   // タイプ
		constexpr const char* kFieldWidth = "width";		   // 幅
		constexpr const char* kFieldHeight = "height";		   // 高さ
		constexpr const char* kFieldLineLength = "LineLength"; // スパン（＝水平投影長）
		constexpr const char* kFieldPitchAngle = "PitchAngle"; // 勾配
		constexpr const char* kFieldVerticalReference = "verticalReference"; // 垂直配置基準
		constexpr const char* kField2DDisplay = "2DDisplay";				 // 2D 表示
		// 軒の出（支持点より軒側＝低い部分）。壁外面から軒先までの距離。
		constexpr const char* kFieldOverhang = "overhang";
		// 支持部分の差し込み（既定 88.9mm＝3.5inch を上書きしないと軒先が外へずれる）。
		constexpr const char* kFieldEmbedment = "bearinginset";
		// ラベルは「表示するか（bool）」と「文字（string）」の 2 つに分かれている。
		constexpr const char* kFieldShowLabel = "showLabel"; // ラベルを表示
		constexpr const char* kFieldLabelText = "labelText"; // ラベル文字
		constexpr const char* kFieldStructuralUse = "structuralUse"; // 構造用途（**先頭は小文字**）
		// 材質の universal 名は診断ダイアログが下端で切れて読み取れていない。universal 名で
		// 見つからなければローカライズ名「材質」で引き直す（ResolveParam）。
		constexpr const char* kFieldMaterial = "Material";
		constexpr const char* kLocalizedMaterial = "材質";
		// ポップアップの選択肢は**キー（英語）で保持され、表示だけがローカライズ**される
		// （タイプに "rafter" を渡すと OIP に「垂木」と出たことで確認済み）。したがって
		// 構造用途・材質も表示文字ではなくキーを渡す必要がある。キーは実機の登録次第なので、
		// 選択肢を列挙して「表示が目的の文字と一致するもの」のキーを使い、列挙できないときだけ
		// 下のフォールバックのキーを使う（SetPopupByLocalized）。
		constexpr const char* kStructuralUseRafterText = "垂木";
		constexpr const char* kStructuralUseRafterKey = "rafter";
		constexpr const char* kMaterialWoodText = "木";
		constexpr const char* kMaterialWoodKey = "wood";

		// オブジェクトのクラスを名前で設定する（draw/Grid.cpp・draw/Floor.cpp と同じヘルパー）。
		void SetClassByName(MCObjectHandle object, const std::string& className)
		{
			if (className.empty())
				return;
			const InternalIndex classID = gSDK->AddClass(TXString(className.c_str()));
			gSDK->SetObjectClass(object, classID);
		}

		// 描画属性（線幅・色・パターン・矢印・透明度）をすべてクラス属性に従わせる
		// （draw/Floor.cpp と同じ規約。SetObjectClass だけでは by-instance の既定値が残る）。
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

		// 角度を PIO の角度パラメータへ渡す文字列にする（度記号付き。Python 版 f'{pitch}°'）。
		// "%g" で余分な 0 を落とす。
		TXString AngleText(double degrees)
		{
			std::array<char, 32> buffer{};
			std::snprintf(buffer.data(), buffer.size(), "%g°", degrees);
			return {buffer.data()};
		}

		// パラメータ名を解決する。universal 名で見つかればそれを使い、見つからなければ
		// ローカライズ名（OIP に出る日本語）で引き直す。名前が 1 つ違うだけで setter が
		// **黙って無視される**ため、確実に見つかる方を選ぶ。どちらでも見つからなければ
		// universal 名をそのまま返す（設定は無視されるが害は無い）。
		TXString ResolveParam(const VWParametricObj& pio, const char* universalName,
							  const char* localizedName)
		{
			const TXString universal(universalName);
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

		// ポップアップのパラメータを「表示文字」で設定する。選択肢はキー（英語）で保持され
		// 表示だけがローカライズされるので、選択肢を列挙して表示が一致するもののキーを渡す。
		// 列挙できない（静的登録で動的選択肢が空）ときは fallbackKey を渡す。
		void SetPopupByLocalized(VWParametricObj& pio, const TXString& param,
								 const char* localizedChoice, const char* fallbackKey)
		{
			std::vector<std::pair<TXString, TXString>> choices;
			pio.PopupGetChoices(param, choices);

			const TXString wanted(localizedChoice);
			for (const std::pair<TXString, TXString>& choice : choices)
			{
				if (choice.second == wanted)
				{
					pio.SetParamAsString(param, choice.first);
					return;
				}
			}
			pio.SetParamAsString(param, TXString(fallbackKey));
		}

		// デザインレイヤの基準高さ（レイヤ平面の Z）。オブジェクトの Z はレイヤ座標なので、
		// 絶対 Z へ置きたい命令はここを引いてレイヤ相対に直す必要がある。
		// ovLayerHeightInCurrUnits は「現在の単位でのレイヤの基準高さ」（Kernel/API/
		// ObjectVariables.h）。本プラグインが扱う図面は mm 単位なので WorldCoord と一致する。
		double LayerElevation(MCObjectHandle layer)
		{
			TVariableBlock value;
			if (!gSDK->GetObjectVariable(layer, ovLayerHeightInCurrUnits, value))
				return 0.0;
			double elevation = 0.0;
			if (!value.GetReal64(elevation))
				return 0.0;
			return elevation;
		}

		// 【一時的な診断・M6 のローカル確認用。値が揃ったらこのブロックごと削除する】
		//
		// 1 回目の診断（全パラメータの universal 名とローカライズ名の一覧）で、勾配・構造用途・
		// ラベルが**名前違いのまま黙って無視されていた**ことと、行列に与えた Z が PIO に
		// 届いていないことが判明した。名前は上の定数へ反映済み。2 回目のこの診断は、設定した
		// 値が実際に入ったかを**読み戻して**確かめるためのもので、一覧は「材質」の universal 名
		// を確定させるために内部パラメータ（ローカライズ名が __NNA_DO_NOT_CHANGE のもの）を
		// 除いて出す（1 回目は全件出したためダイアログが画面下端で切れた）。
		void ShowPlacementDiagnostics(const VWParametricObj& pio, MCObjectHandle object,
									  const core::RafterCommand& rafter, double layerElevation,
									  double dz)
		{
			VWTransformMatrix readBack;
			VWParametricObj(object).GetObjectMatrix(readBack);
			const VWPoint3D offset = readBack.GetOffset();

			std::array<char, 512> buffer{};
			std::snprintf(buffer.data(), buffer.size(),
						  "高さ: 命令 %.1f - レイヤ %.1f = 移動量 %.1f / 読み戻しオフセット "
						  "(%.1f, %.1f, %.1f)",
						  rafter.elevation, layerElevation, dz, offset.x, offset.y, offset.z);
			TXString body(buffer.data());

			// 設定した値の読み戻し（入っていないパラメータがひと目で分かる）。
			const std::array<const char*, 6> checked{kFieldType,		  kFieldPitchAngle,
													 kFieldStructuralUse, kFieldLabelText,
													 kFieldLineLength,	  kFieldVerticalReference};
			body += "\n\n読み戻し: ";
			for (const char* name : checked)
			{
				body += name;
				body += "=[";
				body += pio.GetParamAsString(TXString(name));
				body += "] ";
			}

			// 内部パラメータを除いた一覧（材質の universal 名を確定させるため）。
			const TXString hidden("__NNA_DO_NOT_CHANGE");
			TXString names;
			const size_t count = pio.GetParamsCount();
			for (size_t i = 0; i < count; ++i)
			{
				const TXString localized = pio.GetParamLocalizedName(i);
				if (localized == hidden)
					continue;
				if (!names.IsEmpty())
					names += ", ";
				names += pio.GetParamName(i);
				names += "(";
				names += localized;
				names += ")";
			}
			body += "\n\nパラメータ（内部を除く）: ";
			body += names;
			gSDK->AlertInform(body, TXString("垂木配置の診断（一時）"), false);
		}

		// 垂木 1 本を軸組ツールで描く。PIO を作れなければ平面投影の直線でフォールバックする。
		// 何か 1 つでも配置できたら true。diagnose は上記の一時診断を出すかどうか
		// （最初の 1 本だけ true にして呼ぶ）。
		bool DrawOne(const core::RafterCommand& rafter, double layerElevation, bool diagnose)
		{
			const double dx = rafter.end.x - rafter.start.x;
			const double dy = rafter.end.y - rafter.start.y;
			const double run = std::hypot(dx, dy); // 平面投影長 = LineLength（支持点→棟）
			if (run <= 0.0)
				return false;

			// 軒（支持点）→棟の平面方位角と勾配（度）。勾配は両端の天端 Z の差から。
			const double azimuth = std::atan2(dy, dx) * kDegreesPerRadian;
			const double pitch =
				std::atan2(rafter.endElevation - rafter.elevation, run) * kDegreesPerRadian;

			// 配置行列: Z 軸まわりに平面方位角だけ回し、支持点の XY へ平行移動する。
			// **Z はここでは与えない**。ローカル確認で、行列に Z を入れても PIO には反映されず
			// （与えた 6109.0 に対し読み戻しは 2.0、OIP の Z も 0）、高さは別途 3D 移動で
			// 与える必要があることが分かった（下の MoveObject3D。Python 版 Move3D と同じ手）。
			VWTransformMatrix matrix;
			matrix.SetRotation(azimuth, VWPoint3D(0.0, 0.0, 1.0));
			matrix.SetOffset(rafter.start.x, rafter.start.y, 0.0);

			MCObjectHandle object = gSDK->CreateCustomObjectByMatrix(kFramingMember, matrix);
			if (object == nil)
			{
				// フォールバック: 平面投影の直線（クラス付き）を残す。
				VWPolygon2DObj line({VWPoint2D(rafter.start.x, rafter.start.y),
									 VWPoint2D(rafter.end.x, rafter.end.y)});
				line.SetClosed(false);
				const MCObjectHandle lineHandle = line.GetThisObject();
				if (lineHandle == nil)
					return false;
				SetClassByName(lineHandle, rafter.drawClass);
				return true;
			}

			SetClassByName(object, rafter.drawClass);
			SetAllAttributesByClass(object);

			// パラメータの渡し方はローカル確認で次のように絞り込んだ:
			//   * 寸法・長さ（幅／せい／スパン／軒の出／差し込み）は **SetParamReal**。
			//     文字列で渡していたときは既定値（幅 100・せい 300・長さ 254）のままだった。
			//   * ポップアップ・文字列は **SetParamAsString**（SetParamString は「文字列型の
			//     パラメータ」専用で、ポップアップには効かない）。ポップアップの値は表示文字
			//     ではなく**キー**なので SetPopupByLocalized で選択肢から引く。
			//   * 勾配は角度なので degrees を **SetParamReal**。効かなければ度記号付きの
			//     文字列で入れ直す（下記の読み戻し確認）。
			VWParametricObj pio(object);
			pio.SetParamAsString(kFieldType, kMemberTypeRafter);
			// 支持点（下端基準）から棟側へ立ち上がる。
			pio.SetParamAsString(kFieldVerticalReference, kVerticalReferenceBottom);
			pio.SetParamAsString(kField2DDisplay, k2DDisplayWidth);
			// ラベルは「表示するか」と「文字」の 2 つ。文字だけ入れても表示は既定のままなので
			// 両方設定する。
			pio.SetParamBool(kFieldShowLabel, true);
			pio.SetParamAsString(kFieldLabelText, TXString(rafter.label.c_str()));
			SetPopupByLocalized(pio, TXString(kFieldStructuralUse), kStructuralUseRafterText,
								kStructuralUseRafterKey);
			SetPopupByLocalized(pio, ResolveParam(pio, kFieldMaterial, kLocalizedMaterial),
								kMaterialWoodText, kMaterialWoodKey);
			// 勾配（度）。角度パラメータは実数で保持されるので degrees をそのまま入れ、
			// 反映されていなければ度記号付きの文字列で入れ直す（登録の型に依らず入るように）。
			pio.SetParamReal(kFieldPitchAngle, pitch);
			if (std::abs(pio.GetParamReal(kFieldPitchAngle) - pitch) > kPitchTolerance)
				pio.SetParamAsString(kFieldPitchAngle, AngleText(pitch));
			// 寸法・長さ（文書単位＝mm）。
			pio.SetParamReal(kFieldWidth, rafter.width);
			pio.SetParamReal(kFieldHeight, rafter.height);
			pio.SetParamReal(kFieldLineLength, run);
			pio.SetParamReal(kFieldOverhang, rafter.overhang);
			pio.SetParamReal(kFieldEmbedment, rafter.embedment);
			gSDK->ResetObject(object);

			// 高さ。オブジェクトの Z は**レイヤ座標**（レイヤ平面が Z=0）なので、命令の絶対 Z
			// からレイヤの基準高さを引いてレイヤ相対に直してから 3D 移動する。ResetObject の
			// **後**に動かす（リセットが形状を作り直すため、先に動かすと戻される）。
			// Z の計算はこの 1 か所に集約してある（ローカルで高さがずれたらここだけ直せばよい）。
			const double dz = rafter.elevation - layerElevation;
			gSDK->MoveObject3D(object, 0.0, 0.0, dz);

			if (diagnose)
				ShowPlacementDiagnostics(pio, object, rafter, layerElevation, dz);
			return true;
		}
	} // namespace

	std::size_t drawRafters(const core::Document& document)
	{
		if (document.rafters.empty())
			return 0;

		// 生成のたびに設定ダイアログが開かないよう、最初に 1 度だけ抑止しておく。
		SuppressSettingsDialog();

		std::size_t drawn = 0;
		for (const core::RafterCommand& rafter : document.rafters)
		{
			// 配置先レイヤ（"n-垂木"）が無い命令はスキップする（レイヤは story 命令が作る）。
			MCObjectHandle layer = gSDK->GetNamedLayer(TXString(rafter.layer.c_str()));
			if (layer == nil)
				continue;
			gSDK->SetCurrentLayer(layer);

			// 一時診断は最初に描けた 1 本だけで出す（垂木の本数ぶんダイアログが開かないように）。
			const bool diagnose = drawn == 0;
			if (DrawOne(rafter, LayerElevation(layer), diagnose))
				++drawn;
		}
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
