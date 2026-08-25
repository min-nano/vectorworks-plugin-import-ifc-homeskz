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
//	  2. **配置行列**（Z 軸まわりに回し、支持点へ平行移動）から軸組ツールの PIO を生成する。
//	     Python 版は「原点に生成 → Rotate3D → Move3D」の 3 手順だが、ISDK には VectorScript の
//	     3D 変換状態（ResetOrientation3D / Rotate3D）が無く、代わりに配置行列から直接 PIO を
//	     作れる（CreateCustomObjectByMatrix）。勾配は本体の勾配パラメータが担い、支持点
//	     （下端基準）から棟側へ立ち上がる。
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
//	【方位角は符号を反転して渡す】VWTransformMatrix::SetRotation の角度の符号は、部材が実際に
//	伸びる向きと**逆**である。方位 +90°（＝+Y へ伸ばしたい）を渡したとき OIP の「角度」が
//	−90° になり、部材は −Y へ伸びた。これが「勾配が逆」「軒の出が棟側へ出る」として同時に
//	現れていた正体で、勾配の符号や挿入点の取り方は正しかった。したがって挿入点は**軒側の
//	支持点**（命令の start）、勾配は上りの角度（正）のままで、**方位角だけ −direction を渡す**。
//	部材は挿入点から遠端（棟側）へ向かって上り、差し込み・軒の出はその手前（軒先側）へ伸びる。
//
//	検証: 命令 軒 (-9805, -5940, 6109) → 棟 (-9805, 3440, 7985) に対し、方位 −90° を渡すと
//	実際の範囲は Y[3387.5, 12820.0] Z[7985.0, 9906.9]（＝挿入点から +Y へ上る）になり、
//	渡した角度の符号が反転して効いていることが数値で確認できた。
//
//	【フィールド名は VW 実機の登録から採る】軸組ツールのパラメータ名は、当初 Python 版の
//	VectorScript エクスポートから推定していたが、ローカル確認で PIO の全パラメータの universal
//	名とローカライズ名を出したところ、勾配（pitch ではなく **PitchAngle**。pitch という名前の
//	内部パラメータが別にあり、そちらは __NNA_DO_NOT_CHANGE）・構造用途（**先頭が小文字**の
//	structuralUse）・ラベル（label ではなく **showLabel** ＋ **labelText** の 2 つ）が名前違いで
//	**黙って無視されていた**ことが判明した。スパン（LineLength）は水平投影長で、部材に沿った
//	実長は別パラメータ（LineLengthReal）。ポップアップの値は表示文字ではなく**キー**を渡す。
//	最終挙動は VW 実機で確認する（docs/DEV-NOTES.md M6「ローカル確認」）。名前付き定数に集約する。
//

#include "PluginPrefix.h"
#include "draw/Rafter.h"
#include "draw/DrawUtil.h"
#include "core/Document.h"
#include "core/Progress.h"

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

		// 垂直基準（verticalReference）。挿入点の Z を部材の**下端**として扱う。
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
		// 材質。universal 名が実機で "Material" と確認できたが、念のため見つからなければ
		// ローカライズ名「材質」で引き直す（draw/DrawUtil の ResolveParamName）。
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

		// 角度を PIO の角度パラメータへ渡す文字列にする（度記号付き。Python 版 f'{pitch}°'）。
		// "%g" で余分な 0 を落とす。
		TXString AngleText(double degrees)
		{
			std::array<char, 32> buffer{};
			std::snprintf(buffer.data(), buffer.size(), "%g°", degrees);
			return {buffer.data()};
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

		// 垂木 1 本を軸組ツールで描く。PIO を作れなければ平面投影の直線でフォールバックする。
		// 何か 1 つでも配置できたら true。
		bool DrawOne(const core::RafterCommand& rafter)
		{
			const double dx = rafter.end.x - rafter.start.x;
			const double dy = rafter.end.y - rafter.start.y;
			const double run = std::hypot(dx, dy); // 平面投影長 = LineLength（スパン）
			if (run <= 0.0)
				return false;

			// 軒（支持点）→棟の平面方位角と勾配（度）。勾配は両端の天端 Z の差から求めた
			// **上り**の角度で、正の値が「挿入点から遠端へ向かって上る」を意味する。
			const double direction = std::atan2(dy, dx) * kDegreesPerRadian;
			const double pitch =
				std::atan2(rafter.endElevation - rafter.elevation, run) * kDegreesPerRadian;

			// 配置行列: Z 軸まわりに回し、支持点（XY ＋ 天端の**絶対** Z）へ平行移動する
			// （冒頭「高さは配置行列の絶対 Z で与える」）。**角度は符号を反転して渡す**
			// （冒頭「方位角は符号を反転して渡す」）。
			VWTransformMatrix matrix;
			matrix.SetRotation(-direction, VWPoint3D(0.0, 0.0, 1.0));
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
			// 挿入点の Z を部材の下端として扱う。
			pio.SetParamAsString(kFieldVerticalReference, kVerticalReferenceBottom);
			pio.SetParamAsString(kField2DDisplay, k2DDisplayWidth);
			// ラベルは「表示するか」と「文字」の 2 つ。文字だけ入れても表示は既定のままなので
			// 両方設定する。
			pio.SetParamBool(kFieldShowLabel, true);
			pio.SetParamAsString(kFieldLabelText, TXString(rafter.label.c_str()));
			SetPopupByLocalized(pio, TXString(kFieldStructuralUse), kStructuralUseRafterText,
								kStructuralUseRafterKey);
			SetPopupByLocalized(pio, ResolveParamName(pio, kFieldMaterial, kLocalizedMaterial),
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
			return true;
		}
	} // namespace

	std::size_t drawRafters(const core::Document& document, core::ProgressReporter& progress)
	{
		if (document.rafters.empty())
			return 0;

		// 生成のたびに設定ダイアログが開かないよう、最初に 1 度だけ抑止しておく。
		SuppressSettingsDialog();

		std::size_t drawn = 0;
		for (const core::RafterCommand& rafter : document.rafters)
		{
			// 中止（進捗ダイアログのキャンセル）は残りを描かずに抜ける。進捗は本数で報告し、
			// 描画の前に 1 件進める（＝「いま何本目を描いているか」が見える）。
			if (progress.cancelled())
				break;
			progress.step();

			// 配置先レイヤ（"n-垂木"）が無い命令はスキップする（規約は ActivateExistingLayer）。
			if (ActivateExistingLayer(rafter.layer) == nil)
				continue;

			if (DrawOne(rafter))
				++drawn;
		}
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
