//
//	tests/Fixtures.h
//
//	テスト共通の小道具。フィクスチャの読み込みと実数の近似比較を 1 か所に置く。
//
//	フィクスチャのパス（HOMESKZ_FIXTURES_DIR）は CMake が各テストターゲットへ
//	コンパイル定義で渡す（tests/CMakeLists.txt）。これを使うテストは必ずその定義を
//	受け取るターゲットに登録すること——定義が無ければコンパイルエラーになるので、
//	登録漏れは黙って通らない。
//
//	【無 SDK】ここも他のテストと同じく VectorWorks SDK に触れない（CLAUDE.md「テスト方針」）。
//

#pragma once

#include "parse/Loader.h"

#include <cmath>
#include <string>
#include <vector>

namespace HomeskzIfcTests
{
	// フィクスチャを読む（読み込めなければ ok=false。呼び出し側で CHECK 失敗させる）。
	inline HomeskzIfcImport::parse::Model fixture(const std::string& filename, bool& ok)
	{
		return HomeskzIfcImport::parse::loadIfc(std::string(HOMESKZ_FIXTURES_DIR) + "/" + filename,
												&ok);
	}

	// 2 つの実数が許容誤差内で等しいか。座標・高さは mm 単位なので、既定の 1e-6 は
	// Python 版の round(…, 3) より細かい粒度にあたる。
	inline bool near(double a, double b, double tol = 1e-6)
	{
		return std::abs(a - b) < tol;
	}

	// 検証済みのホームズ君 EX 実 IFC 一式（tests/fixtures/README.md）。**この一覧が唯一の
	// 定義**で、「全フィクスチャに対して回す」テストはここを参照する（各テストが独自の
	// 一覧を持つと、フィクスチャを足したときに一部のテストだけ素通りする）。
	inline const std::vector<std::string>& allFixtures()
	{
		static const std::vector<std::string> names = {
			"サンプル1 (住木邸新築工事).ifc",	"伏図次郎【2階】.ifc",
			"グレー本モデルプラン1【3階】.ifc", "グレー本モデルプラン2【3階】.ifc",
			"スキップフロア_サンプル.ifc",
		};
		return names;
	}
} // namespace HomeskzIfcTests
