//
//	tests/StepText.h
//
//	合成 IFC（STEP テキスト）を組み立てるテスト共通の小道具。**#id を採番しながら STEP 行を
//	溜める器（StepText）と、行の部品を作る最小ヘルパー（num / ref / point3 / point2 /
//	direction / makeStorey / contain）はここが唯一の定義**で、横架材・柱・アンカーボルト・
//	床束のテストはいずれもこれを使う。
//
//	【なぜ 1 か所に置くか】かつては StepText / num / ref が 3 ファイル、点・向き・ストーリ・
//	所属のヘルパーが 2 ファイルに逐語的な複製として置かれ、床束のテストは同じものをローカル
//	ラムダで作り直していた。複製のままだと 1 か所だけ直したときに「同じ STEP 断片のはずが
//	要素ごとに前提が違う」テストになり得る（RoofSample.h / Fixtures.h に一本化したのと同じ
//	理由）。逆に **要素固有の組み立て（BeamSpec / ColumnSpec / makeFastener 等）はここへ
//	寄せない**——それはその要素のテストの都合であり、共有すると要素を跨いだ暗黙の結合が生まれる。
//
//	【無 SDK】ここも他のテストと同じく VectorWorks SDK に触れない（CLAUDE.md「テスト方針」）。
//

#pragma once

#include "parse/Loader.h"

#include <string>

namespace HomeskzIfcTests
{
	// #id を採番しながら STEP 行を溜めるだけの器。
	class StepText
	{
	public:
		int add(const std::string& body)
		{
			const int id = fNext++;
			fText += "#" + std::to_string(id) + "=" + body + ";\n";
			return id;
		}

		HomeskzIfcImport::parse::Model build() const
		{
			return HomeskzIfcImport::parse::loadIfcFromText(fText);
		}

	private:
		int fNext = 1;
		std::string fText;
	};

	// 実数を STEP のリテラルへ（std::to_string の固定小数 6 桁）。各テストの期待値は
	// この表記で書かれた IFC を読んだ結果に合わせてある。
	inline std::string num(double value)
	{
		return std::to_string(value);
	}

	// エンティティ参照（#id）。
	inline std::string ref(int id)
	{
		return "#" + std::to_string(id);
	}

	inline int point3(StepText& step, double x, double y, double z)
	{
		return step.add("IFCCARTESIANPOINT((" + num(x) + "," + num(y) + "," + num(z) + "))");
	}

	inline int point2(StepText& step, double x, double y)
	{
		return step.add("IFCCARTESIANPOINT((" + num(x) + "," + num(y) + "))");
	}

	inline int direction(StepText& step, double x, double y, double z)
	{
		return step.add("IFCDIRECTION((" + num(x) + "," + num(y) + "," + num(z) + "))");
	}

	// IfcBuildingStorey(GlobalId, OwnerHistory, Name=2, Description, ObjectType,
	// ObjectPlacement, Representation, LongName, CompositionType, Elevation=9)。
	inline int makeStorey(StepText& step, const std::string& name, double elevation)
	{
		return step.add("IFCBUILDINGSTOREY('s',$,'" + name + "',$,$,$,$,$,.ELEMENT.," +
						num(elevation) + ")");
	}

	// 要素を階へ所属させる（IfcRelContainedInSpatialStructure）。
	inline void contain(StepText& step, int storey, int element)
	{
		step.add("IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(" + ref(element) + ")," +
				 ref(storey) + ")");
	}
} // namespace HomeskzIfcTests
