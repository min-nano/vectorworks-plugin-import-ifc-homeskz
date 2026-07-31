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
//	PIO を生成できない場合は平面投影の直線にフォールバックする（1 本の失敗で全体を止めない）。
//
//	【高さは配置行列の絶対 Z で与える】配置行列のオフセット Z には命令の elevation（絶対 Z）を
//	そのまま渡す。一度「行列の Z が届いていない」と読み違えて 3D 移動（MoveObject3D）へ
//	切り替えたが、これは誤りだった: 読み戻した行列オフセットは**レイヤ相対**で返るため、
//	絶対 6109 を与えて 2.0 が返るのは「レイヤ高さ 6107 のぶんを引いた正しい値」であって、
//	Z が落ちていたわけではない（レイヤ相対で 3D 移動し直したところ、垂木が絶対 2mm ＝ 地面に
//	並んでしまい確定した）。**行列のオフセットは XY がレイヤ座標・Z が絶対**という混在に注意。
//	Z の計算は 1 か所（DrawOne の SetOffset）に集約してある。
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

		// 【一時的な診断・M6 のローカル確認用。向きが確定したらこのブロックごと削除する】
		//
		// これまでの診断で、パラメータ名（勾配 PitchAngle・構造用途 structuralUse・ラベル
		// showLabel/labelText・材質 Material）と、行列オフセットの読み戻しが**レイヤ相対**で
		// 返ることが確定し、いずれも上の実装へ反映した。残る確認は**向き**（勾配の符号と
		// 方位角の基準）なので、命令の軒側／棟側の 3D 座標と、そこから求めた方位角・勾配を
		// 出して、実際に描かれた向きと突き合わせられるようにする。
		void ShowPlacementDiagnostics(const VWParametricObj& pio, MCObjectHandle object,
									  const core::RafterCommand& rafter, double azimuth,
									  double pitch)
		{
			VWTransformMatrix readBack;
			VWParametricObj(object).GetObjectMatrix(readBack);
			const VWPoint3D offset = readBack.GetOffset();

			std::array<char, 512> buffer{};
			std::snprintf(buffer.data(), buffer.size(),
						  "命令: 軒 (%.1f, %.1f, %.1f) → 棟 (%.1f, %.1f, %.1f) / 方位 %.2f° "
						  "勾配 %.2f°\n読み戻しオフセット (%.1f, %.1f, %.1f)",
						  rafter.start.x, rafter.start.y, rafter.elevation, rafter.end.x,
						  rafter.end.y, rafter.endElevation, azimuth, pitch, offset.x, offset.y,
						  offset.z);
			TXString body(buffer.data());

			// 設定した値の読み戻し（入っていないパラメータがひと目で分かる）。
			const std::array<const char*, 7> checked{
				kFieldType,		  kFieldPitchAngle,		   kFieldStructuralUse, kFieldLabelText,
				kFieldLineLength, kFieldVerticalReference, kFieldMaterial};
			body += "\n\n読み戻し: ";
			for (const char* name : checked)
			{
				body += name;
				body += "=[";
				body += pio.GetParamAsString(TXString(name));
				body += "] ";
			}

			gSDK->AlertInform(body, TXString("垂木配置の診断（一時）"), false);
		}

		// 垂木 1 本を軸組ツールで描く。PIO を作れなければ平面投影の直線でフォールバックする。
		// 何か 1 つでも配置できたら true。diagnose は上記の一時診断を出すかどうか
		// （最初の 1 本だけ true にして呼ぶ）。
		bool DrawOne(const core::RafterCommand& rafter, bool diagnose)
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

			// 配置行列: Z 軸まわりに平面方位角だけ回し、支持点（XY ＋ 天端の**絶対** Z）へ
			// 平行移動する（冒頭「高さは配置行列の絶対 Z で与える」）。
			VWTransformMatrix matrix;
			matrix.SetRotation(azimuth, VWPoint3D(0.0, 0.0, 1.0));
			matrix.SetOffset(rafter.start.x, rafter.start.y, rafter.elevation);

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

			if (diagnose)
				ShowPlacementDiagnostics(pio, object, rafter, azimuth, pitch);
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
			if (DrawOne(rafter, diagnose))
				++drawn;
		}
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
