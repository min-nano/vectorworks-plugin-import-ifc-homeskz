//
//	tests/Fixtures.h
//
//	テスト共通の小道具。フィクスチャの読み込みと実数の近似比較を 1 か所に置く。
//
//	フィクスチャのパス（HOMESKZ_FIXTURES_DIR）は CMake が各テストターゲットへ
//	コンパイル定義で渡す（tests/CMakeLists.txt）。フィクスチャを読むヘルパー
//	（fixture / allFixtures）は**その定義があるときだけ**現れるので、定義を受け取らない
//	ターゲットで使えばコンパイルエラーになる（登録漏れは黙って通らない）。近似比較
//	（near）はフィクスチャに依存しないので、フィクスチャを使わないテスト
//	（CoreRegionTests 等）もこのヘッダから使える。
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
	// 2 つの実数が許容誤差内で等しいか。座標・高さは mm 単位なので、既定の 1e-6 は
	// Python 版の round(…, 3) より細かい粒度にあたる。**この定義が唯一**で、各テストが
	// ローカルの near を持たない（同じ比較の閾値がテストごとに散らばらないようにする）。
	inline bool near(double a, double b, double tol = 1e-6)
	{
		return std::abs(a - b) < tol;
	}

#ifdef HOMESKZ_FIXTURES_DIR
	// フィクスチャのフルパス。**ディレクトリと名前を連結するのはここだけ**（各テストが
	// 自前で連結すると、置き場所を変えたときに直し漏れる）。ローダ自身をテストする
	// ケースのように、Model ではなくパスが要るときに使う（存在しないファイル名も可）。
	inline std::string fixturePath(const std::string& filename)
	{
		return std::string(HOMESKZ_FIXTURES_DIR) + "/" + filename;
	}

	// フィクスチャを読む（読み込めなければ ok=false。呼び出し側で CHECK 失敗させる）。
	inline HomeskzIfcImport::parse::Model fixture(const std::string& filename, bool& ok)
	{
		return HomeskzIfcImport::parse::loadIfc(fixturePath(filename), &ok);
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
#endif // HOMESKZ_FIXTURES_DIR
} // namespace HomeskzIfcTests
