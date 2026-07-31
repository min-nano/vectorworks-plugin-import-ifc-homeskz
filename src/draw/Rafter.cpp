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
//	     （LineLength）・平面方位角・勾配（pitch）**を求める。
//	  2. **配置行列**（Z 軸まわりに平面方位角だけ回し、支持点へ平行移動）から軸組ツールの PIO を
//	     生成する。Python 版は「原点に生成 → Rotate3D → Move3D」の 3 手順だが、ISDK には
//	     VectorScript の 3D 変換状態（ResetOrientation3D / Rotate3D / Move3D）が無く、代わりに
//	     配置行列から直接 PIO を作れる（CreateCustomObjectByMatrix）。結果は同じで、作図状態に
//	     依存しないぶん堅い。勾配は本体の pitch パラメータが担い、支持点（下端基準）から棟側へ
//	     立ち上がる。
//	  3. クラス（小屋組-垂木）を割り当て、描画属性をすべてクラス属性に従わせる。
//	  4. 断面・配置・2D 表示・軒の出・差し込み・仕様ラベル・構造用途・材質の各パラメータを
//	     設定して ResetObject で反映する。
//	PIO を生成できない場合は平面投影の直線にフォールバックする（1 本の失敗で全体を止めない）。
//
//	【高さの与え方（ローカル確認項目）】配置行列のオフセット Z には命令の elevation（絶対 Z）を
//	そのまま渡す。Python 版 vw/rafter.py も Move3D に絶対 Z を渡して実機で確認済みで、まずは
//	その挙動に合わせる。ただし屋根では Python 版が「レイヤ基準で扱わないとレイヤ高さぶん二重に
//	持ち上がる」ことを確認している（#113）ため、垂木でも二重に持ち上がる可能性は残る。VW 実機で
//	高さがずれていたら、ここでレイヤの高さを引く（＝レイヤ相対へ直す）ことで直せるよう、
//	オフセットの計算は 1 か所（DrawOne の SetOffset）に集約してある。
//
//	【フィールド名・値は VW の登録に一致させる必要がある】軸組ツールのパラメータ名（type /
//	width / height / LineLength / pitch / verticalReference / 2DDisplay / overhang /
//	bearinginset / label / StructuralUse / Material）と値は VectorWorks の FramingMember 登録に
//	合わせる（Python 版が実オブジェクトの VectorScript エクスポートで確認した名前。差し込みの
//	登録名は overhang と違って **bearinginset** で、既定 88.9mm＝3.5inch を上書きしないと軒先が
//	外へずれる）。最終挙動（高さ・向き・pitch・各パラメータ）は VW 実機で確認する
//	（ROADMAP.md M6「ローカル確認」）。名前付き定数に集約する。
//

#include "PluginPrefix.h"
#include "draw/Rafter.h"
#include "core/Document.h"

// 配置行列（VWTransformMatrix）と PIO パラメータ設定（VWParametricObj）。フォールバックの
// 直線は draw/Grid.cpp と同じ VWPolygon2DObj で描く。
#include "VWFC/Math/VWTransformMatrix.h"
#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWPolygon2DObj.h"

#include <cmath>
#include <cstddef>
#include <numbers>
#include <string>

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// ラジアン → 度（M_PI は MSVC で既定では未定義なので C++20 の std::numbers を使う）。
		constexpr double kDegreesPerRadian = 180.0 / std::numbers::pi;

		// 軸組ツールの PIO 名と部材種別（垂木は 'rafter'）。VW の登録名に一致させる。
		const TXString kFramingMember("FramingMember");
		constexpr const char* kMemberTypeRafter = "rafter";

		// 垂直基準（verticalReference）。軒側（下端基準）から棟へ立ち上がる。
		constexpr const char* kVerticalReferenceBottom = "bottom";
		// 2D 表示（2DDisplay）。要件により「幅」表示にする。
		constexpr const char* k2DDisplayWidth = "width";

		// 以下のフィールド名・値は VW の FramingMember 登録に一致させる（冒頭コメント参照）。
		constexpr const char* kFieldType = "type";
		constexpr const char* kFieldWidth = "width";
		constexpr const char* kFieldHeight = "height";
		constexpr const char* kFieldLineLength = "LineLength";
		constexpr const char* kFieldPitch = "pitch";
		constexpr const char* kFieldVerticalReference = "verticalReference";
		constexpr const char* kField2DDisplay = "2DDisplay";
		// 軒の出（支持点より軒側＝低い部分）。壁外面から軒先までの距離。
		constexpr const char* kFieldOverhang = "overhang";
		// 支持部分の差し込み。VW の登録名は 'bearinginset'（既定 88.9mm を上書きする）。
		constexpr const char* kFieldEmbedment = "bearinginset";
		constexpr const char* kFieldLabel = "label";
		constexpr const char* kFieldStructuralUse = "StructuralUse";
		constexpr const char* kStructuralUseRafter = "垂木";
		constexpr const char* kFieldMaterial = "Material";
		constexpr const char* kMaterialWood = "木";

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

		// 垂木 1 本を軸組ツールで描く。PIO を作れなければ平面投影の直線でフォールバックする。
		// 何か 1 つでも配置できたら true。
		bool DrawOne(const core::RafterCommand& rafter)
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

			// 配置行列: Z 軸まわりに平面方位角だけ回し、支持点（XY ＋ 天端 Z）へ平行移動する
			// （Python 版 Rotate3D → Move3D と同じ結果を 1 手で与える。高さの基準は冒頭
			// 「高さの与え方（ローカル確認項目）」参照）。
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

			// パラメータは**型ごとのセッタで渡す**。文字列セッタ（SetParamString）は文字列
			// パラメータ専用で、寸法・角度のような数値パラメータには効かない（ローカル確認で
			// 幅 100・せい 300・長さ 254＝すべて PIO の既定値のまま描かれ、文字列で渡した
			// type だけが反映されていた）。寸法・角度は SetParamReal で渡す。
			VWParametricObj pio(object);
			// 文字列（ポップアップ・テキスト）のパラメータ。
			pio.SetParamString(kFieldType, kMemberTypeRafter);
			// 支持点（下端基準）から棟側へ立ち上がる。
			pio.SetParamString(kFieldVerticalReference, kVerticalReferenceBottom);
			pio.SetParamString(kField2DDisplay, k2DDisplayWidth);
			pio.SetParamString(kFieldLabel, TXString(rafter.label.c_str()));
			pio.SetParamString(kFieldStructuralUse, kStructuralUseRafter);
			pio.SetParamString(kFieldMaterial, kMaterialWood);
			// 数値（寸法・長さ・角度）のパラメータ。長さは文書単位（mm）、角度は度。
			pio.SetParamReal(kFieldWidth, rafter.width);
			pio.SetParamReal(kFieldHeight, rafter.height);
			pio.SetParamReal(kFieldLineLength, run);
			pio.SetParamReal(kFieldPitch, pitch);
			pio.SetParamReal(kFieldOverhang, rafter.overhang);
			pio.SetParamReal(kFieldEmbedment, rafter.embedment);
			gSDK->ResetObject(object);
			return true;
		}
	} // namespace

	std::size_t drawRafters(const core::Document& document)
	{
		std::size_t drawn = 0;
		for (const core::RafterCommand& rafter : document.rafters)
		{
			// 配置先レイヤ（"n-垂木"）が無い命令はスキップする（レイヤは story 命令が作る）。
			MCObjectHandle layer = gSDK->GetNamedLayer(TXString(rafter.layer.c_str()));
			if (layer == nil)
				continue;
			gSDK->SetCurrentLayer(layer);

			if (DrawOne(rafter))
				++drawn;
		}
		return drawn;
	}
} // namespace HomeskzIfcImport::draw
