//
//	draw/ObjectHandles.h
//
//	「命令インデックス → 描画したオブジェクトのハンドル」の対応表。
//
//	**SDK ハンドルは Document に載せられない**（フェーズ間で運べない。CLAUDE.md
//	「所有権」）ので、ある要素を描画した側と、それを後から参照する側は、命令の並びの
//	インデックスで受け渡す。現在 4 か所が使う:
//	  * 立上り（drawWalls が記録）→ 壁結合（drawWallJoins が a / b で参照する）
//	  * 横架材（drawMembers が記録）→ 断面寸法データタグ（伏図・軸組図が関連付け先として
//	    参照する。draw/Tag）
//	  * 柱（drawColumns が記録）→ 取り込み後の再測定（recheckColumns。開発ビルドだけ）
//	  * 耐力壁（drawShearWalls が記録）→ 取り込み後の再測定（recheckShearWalls。同上）
//
//	【SDK 非依存のヘッダ】要素ごとの draw/*.h は SDK 型を持たない約束なので
//	（draw/DrawUtil.h 冒頭）、**中身（MCObjectHandle の表）は
//	draw/DrawUtil.h 側に置き**、ここは所有者だけを宣言する（pimpl）。表の実体にアクセスするのは
//	SDK を include する draw/*.cpp だけ。
//

#pragma once

#include <memory>

namespace HomeskzIfcImport::draw
{
	// 対応表の実体。定義は draw/DrawUtil.h（SDK 型 MCObjectHandle を持つため）。
	struct ObjectHandleTable;

	// 対応表の所有者。executeDocument が要素ごとに 1 つ作り、書く側（draw*）と読む側へ
	// 渡す。コピー不可（1 回のインポートで 1 つの表を共有する意図を型で示す）。
	class ObjectHandles
	{
	public:
		ObjectHandles();
		~ObjectHandles();
		ObjectHandles(const ObjectHandles&) = delete;
		ObjectHandles& operator=(const ObjectHandles&) = delete;

		ObjectHandleTable& table()
		{
			return *fTable;
		}
		const ObjectHandleTable& table() const
		{
			return *fTable;
		}

	private:
		std::unique_ptr<ObjectHandleTable> fTable;
	};
} // namespace HomeskzIfcImport::draw
