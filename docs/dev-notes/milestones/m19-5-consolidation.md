# 共通化で寄せたもの（M19.5）

寄せ先の一覧は[「置き場所の一覧（重複を作らない）」](../../development/placement-index.md)が正で、ここには**迷った判断と、
再調査しないための記録**だけを残す。

- **ローカル配置原点の 4 段の鎖**（ObjectPlacement → IFCLOCALPLACEMENT → RelativePlacement →
  IFCAXIS2PLACEMENT3D → Location）は `parse/IfcGeometry` の `resolveLocalPlacementOrigin` に
  統一した。かつて柱・横架材・ストーリが各々書いており、**Location の型チェック
  （IFCCARTESIANPOINT）はストーリだけが持っていた**。統一では**厳格側（型チェックあり）に
  揃えた**——IfcDirection も同じ属性位置に実数リストを持つため、型を確認しないと方向比を座標
  として誤読する（`ParseStoryTests` の `get_local_placement_z_false_when_location_not_cartesian`
  が守っている仕様）。柱・横架材はこの統一で従来より厳格になったが、ホームズ君 IFC の
  実データでは挙動は同一（全フィクスチャで確認）。
- **同一直線マージの平行許容は要素間で意図的に違うまま**にしてある: 大引の継手統合
  （`parse/FloorPost.h` の `kCollinearAngleTol` = 1e-6）と立上りの統合（`parse/Footing.h` の
  `kWallMergeAngleTol` = 1e-3）で 1000 倍違う。共通化したのは骨格（`core/UnionFind` の
  `connectedComponents` と `core/Geometry` の `collinearSpan`）だけで、**許容値は統合していない**
  ——値を揃えると片方のチューニングがもう片方を巻き添えにする。1e-3 が意図的な緩さか単なる
  歴史かは未確認（フィクスチャではどちらでも結果が変わらない）。揃えたくなったら、先に
  実データで立上りの向きの分布を測ること。
- **Z 重なり判定・平行判定などの許容値も統合していない**（`kJointZOverlapTol` /
  `kNoboribariZOverlapTol` / `Member.cpp` の `kZOverlapTol` はいずれも今たまたま 1.0）。
  式は `core/Document.h` の `zRangesOverlap` に 1 つだが、**tol は呼び出し側の定数のまま**。
  意味が違う（取り付き・受け・食い込み）ので、片方だけ調整できる形を保つ。
- **Member.cpp のローカル `MemberGeom` と `parse/Joint.h` の公開 `MemberGeom` は敢えて
  統合しなかった**。同名・同形に見えるが関門が違う——食い込み調整側（`geomOf`）は傾斜梁を
  除外し長さ 0 だけ拒否、仕口側（`memberGeom`）は傾斜梁を受け入れ長さ < 1mm を拒否。統合すると
  この差分がポリシー引数になって読みにくくなるうえ、閾値の差で挙動が変わる。
