//
//	core/FloorScript.cpp
//
//	床の VectorScript 組み立ての実装。方針・Python 版との対応は FloorScript.h を参照。
//	SDK 非依存（core/ は VectorWorks SDK を一切 include しない）。
//

#include "core/FloorScript.h"

#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

namespace HomeskzIfcImport::core
{
	namespace
	{
		// 生成するスクリプトの手続き名。VW 上の他のスクリプトと衝突しないよう接頭辞を付ける。
		constexpr const char* kProcedureName = "HomeskzImportDrawFloor";

		// 座標・寸法の小数桁数。mm 単位系なので 6 桁あれば丸め誤差は無視できる
		// （Python 版が渡す float と同じ精度で VW に届く）。
		constexpr int kDecimals = 6;

		// 外形ポリゴン（閉じたポリゴン）を描く命令列。Python 版の
		// ClosePoly → BeginPoly → MoveTo/LineTo… → EndPoly に対応する。
		void appendBoundaryPolygon(std::ostringstream& out, const FloorCommand& floor,
								   const char* indent)
		{
			out << indent << "ClosePoly;\n";
			out << indent << "BeginPoly;\n";
			bool first = true;
			for (const Vec2& point : floor.boundary)
			{
				// 1 点目だけ MoveTo（描き始め）、以降は LineTo（Python 版と同じ）。
				const char* const command = first ? "MoveTo(" : "LineTo(";
				const std::string x = formatScriptNumber(point.x);
				const std::string y = formatScriptNumber(point.y);
				out << indent << command << x << ", " << y << ");\n";
				first = false;
			}
			out << indent << "EndPoly;\n";
		}

		// 描画属性（線幅・色・パターン・透明度等）をすべてクラス属性に従わせる命令列。
		// SetClass はクラスを割り当てるだけで各属性は by-instance の既定値のまま残るため、
		// 属性ごとの by-class 設定を個別に呼ぶ（Python 版 _set_all_attributes_by_class）。
		void appendAttributesByClass(std::ostringstream& out, const char* indent)
		{
			for (const char* call :
				 {"SetPenColorByClass", "SetFillColorByClass", "SetLWByClass", "SetLSByClass",
				  "SetFPatByClass", "SetMarkerByClass", "SetOpacityByClass"})
				out << indent << call << "(h);\n";
		}
	} // namespace

	std::string formatScriptNumber(double value)
	{
		std::ostringstream out;
		// クラシックロケール固定（小数点が ',' になる環境でもスクリプトが壊れない）。
		out.imbue(std::locale::classic());
		out << std::fixed << std::setprecision(kDecimals) << value;
		return out.str();
	}

	std::string quoteScriptString(const std::string& text)
	{
		// ' を含まない一般的なケース（クラス名・レベル名）は走査せず囲むだけ。
		if (text.find('\'') == std::string::npos)
			return "'" + text + "'";

		std::string out;
		out.reserve(text.size() + 2);
		out.push_back('\'');
		for (const char c : text)
		{
			if (c == '\'')
				out.push_back('\''); // '' が ' のエスケープ
			out.push_back(c);
		}
		out.push_back('\'');
		return out;
	}

	std::string buildFloorScript(const FloorCommand& floor)
	{
		std::ostringstream out;
		out.imbue(std::locale::classic());

		out << "PROCEDURE " << kProcedureName << ";\n";
		out << "VAR\n";
		out << "\th : HANDLE;\n";
		out << "BEGIN\n";

		// 床ツール: BeginFloor(厚み) で開始し、平面外形を閉じたポリゴンとして描いて
		// EndGroup で床オブジェクトを確定する。
		out << "\tBeginFloor(" << formatScriptNumber(floor.thickness) << ");\n";
		appendBoundaryPolygon(out, floor, "\t");
		out << "\tEndGroup;\n";
		out << "\th := LNewObj;\n";
		out << "\tIF h <> NIL THEN\n";
		out << "\tBEGIN\n";
		// 床下端を IFC の床位置（絶対 Z）へ。床ツールは床を作成した層平面（Z=0）に置くため、
		// Move3D で実際の高さへ移動する（構造材の Move3D と同じ規約）。
		out << "\t\tMove3D(0, 0, " << formatScriptNumber(floor.elevation) << ");\n";
		out << "\t\tSetClass(h, " << quoteScriptString(floor.drawClass) << ");\n";
		appendAttributesByClass(out, "\t\t");
		// 高さ基準を標準の床高＝横架材天端レベルへバインドする（第 2 引数 0＝下端、
		// 第 3 引数 2＝ストーリレベル基準）。offset は床下端と横架材天端の差分で、
		// 段差＝スキップフロアはここに表れる。
		const std::string level = quoteScriptString(floor.bound.level);
		const std::string boundOffset = formatScriptNumber(floor.bound.offset);
		out << "\t\tSetObjectStoryBound(h, 0, 2, " << floor.bound.storyOffset << ", " << level
			<< ", " << boundOffset << ");\n";
		out << "\t\tResetObject(h);\n";
		out << "\tEND\n";
		out << "\tELSE\n";
		out << "\tBEGIN\n";
		// フォールバック: 床が作れなければ外形ポリゴンを残してクラス分けする
		// （1 枚の失敗で全体を止めない。Python 版の寛容さ）。
		appendBoundaryPolygon(out, floor, "\t\t");
		out << "\t\th := LNewObj;\n";
		out << "\t\tIF h <> NIL THEN\n";
		out << "\t\tBEGIN\n";
		out << "\t\t\tSetClass(h, " << quoteScriptString(floor.drawClass) << ");\n";
		appendAttributesByClass(out, "\t\t\t");
		out << "\t\tEND;\n";
		out << "\tEND;\n";
		out << "END;\n";
		out << "RUN(" << kProcedureName << ");\n";
		return out.str();
	}
} // namespace HomeskzIfcImport::core
