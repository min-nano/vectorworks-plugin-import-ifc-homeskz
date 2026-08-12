//
//	draw/ObjectHandles.h
//
//	「命令インデックス → 描いたオブジェクトのハンドル」の対応表。
//
//	**SDK ハンドルは Document に載せられない**（フェーズ間で運べない。CLAUDE.md
//	「所有権」）ので、あるものを描いた側と、それを後から参照する側は、命令の並びの
//	インデックスで受け渡す。いま 2 か所が使う:
//	  * 立上り（drawWalls が記録）→ 壁結合（drawWallJoins が a / b で引く）
//	  * 柱（drawColumns が記録）→ 伏図記号（drawColumnPlanMarks がデータタグの
//	    関連付け先として引く）
//	M13 の断面寸法データタグ（横架材ハンドル → タグ）も同じ形になる。
//
//	【SDK 非依存のヘッダ】draw/*.h は Extensions/ExtMenu からも include されるので
//	SDK 型を持てない（draw/DrawUtil.h 冒頭）。そこで**中身（MCObjectHandle の表）は
//	draw/DrawUtil.h 側に置き**、ここは所有者だけを宣言する（pimpl）。表の実体を触るのは
//	SDK 込みの draw/*.cpp だけ。
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
