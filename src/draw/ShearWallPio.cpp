//
//	draw/ShearWallPio.cpp
//
//	耐力壁 PIO のリセット本体（意図は draw/ShearWallPio.h・Extensions/ExtShearWall.h）。
//	**Extensions/ExtShearWall.cpp から本体（ペイロード）側へ移したもの**で、中身は移設前と
//	同じ。リセットのたびに、
//	  1. 自分の両端（線分 PIO の 2 点＝柱芯）をローカル座標へ落とし、
//	  2. パラメータの対象レイヤ（";" 区切り）から**両端の柱**を探して内側面を求め、
//	  3. その内側面と下端・上端で決まる「軸組内法」へ、伏図の記号（2D）と軸組図の面（3D）を
//	     描く
//	という流れで作図する。**内法は毎回実物の柱から導く**ので、柱を動かしても（リセットが
//	走れば）耐力壁が追随して伸縮する。柱が見つからなければ控えの内法（ClearSpan）を両端の
//	中央へ置く。
//
//	使用する SDK API は ci-debug の sdk-grep で実在を確認したもの:
//	  gSDK->GetNamedLayer / FirstMemberObj / NextObject / GetObjectBounds / CreateLine /
//	  CreateOval / AddObjectToContainer、VWParametricObj（GetLinearObjectPos /
//	  GetObjectToWorldTransform / パラメータの読み）、VWPolygon2DObj、VWPolygon3D ＋
//	  VWPolygon3DObj。
//
//	【座標系】PIO のジオメトリは**PIO 自身のローカル座標**で持たれる。線分 PIO なので
//	ローカル X が壁の向き（始点→終点）、ローカル Y が壁面の法線（**+Y が表**）、
//	ローカル Z が高さになる。柱はワールド座標で見つかるので、描く前に必ず
//	InversePointTransform でローカルへ落とす（落とさないと PIO を動かした量だけ絵がずれ、
//	リセットしても同じ相対位置に描き直すので直らない。draw/ColumnMarkPio.cpp で実証済み）。
//
//	【実際の見え方はローカルで確認する】記号の大きさ・ハッチングの向き・断面ビューポートで
//	3D の面がどう出るかは CI では検証できない（CLAUDE.md「テスト方針」）。純計算に落とせる
//	部分——筋かいの帯を内法で切る形——は core::shearWallBracePolygon に置いて無 SDK で
//	テストしてある。
//

#include "PluginPrefix.h"
#include "BuildConfig.h"
#include "draw/ShearWallPio.h"
#include "Extensions/ExtShearWall.h"
#include "draw/DrawUtil.h"

#include "core/Document.h"
#include "core/Geometry.h"
#include "core/Trace.h"

#include "VWFC/Math/VWPolygon3D.h"
#include "VWFC/VWObjects/VWParametricObj.h"
#include "VWFC/VWObjects/VWPolygon3DObj.h"
#include "VWFC/VWObjects/VWSymbolObj.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace HomeskzIfcImport
{
	namespace draw
	{
		namespace
		{
			// 柱を「この耐力壁の端の柱」とみなす許容（mm）。壁の軸から法線方向にこれ以上
			// 離れた柱は別の通りのものなので採らない。半柱幅（〜75）＋モデリング誤差を見込む。
			constexpr double kColumnOffAxisTol = 300.0;

			// 端の柱とみなす軸方向の許容（mm）。柱芯は耐力壁の端点そのものなので本来 0 だが、
			// 手で動かした耐力壁でも拾えるだけの余裕を取る。
			constexpr double kColumnAlongTol = 300.0;

			// PIO の実数パラメータを読む。**長さフィールドでも文字列で保持されていることがある**
			// ので、実数で 0 が返ったら文字列でも試す（書き手側の SetParamRealChecked が実数／
			// 文字列のどちらでも入れるのと対。draw/DrawUtil.h）。
			//
			// **「読めて 0 だった」と「そもそも読めなかった」を区別する。** 前者は正しい 0
			// （耐力壁の下端は土台天端＝0 が普通）なので 0 を返し、後者だけ fallback へ落とす。
			// 混同すると、値が 0 の正常なパラメータに既定値が化けて入る。
			double ParamReal(const VWParametricObj& pio, const char* name, double fallback = 0.0)
			{
				bool read = false;
				double value = 0.0;
				try
				{
					value = pio.GetParamReal(TXString(name));
					read = true;
				}
				catch (...)
				{
					read = false; // 実数として読めないパラメータ。下の文字列読みへ回す。
				}
				if (read && value != 0.0)
					return value;

				// 文字列としてなら読めることがある（単位付きの表記など）。読めた数だけ採る。
				const std::string text = draw::PioParamString(pio, name);
				if (!text.empty())
				{
					bool parsed = false;
					double number = 0.0;
					try
					{
						number = std::stod(text);
						parsed = true;
					}
					catch (...)
					{
						parsed = false; // 数値に見えない文字列。下の判断へ落とす。
					}
					if (parsed)
						return number;
				}
				return read ? value : fallback;
			}

			// ";" 区切りのレイヤ名を分解する（空の要素は落とす）。
			std::vector<std::string> SplitLayers(const std::string& joined)
			{
				std::vector<std::string> names;
				std::size_t begin = 0;
				while (begin <= joined.size())
				{
					const std::size_t end = joined.find(';', begin);
					const std::string name = joined.substr(
						begin, end == std::string::npos ? std::string::npos : end - begin);
					if (!name.empty())
						names.push_back(name);
					if (end == std::string::npos)
						break;
					begin = end + 1;
				}
				return names;
			}

			// 見つけた柱 1 本の、PIO ローカル座標での広がり。
			struct ColumnRange
			{
				double loX = 0.0;	  // 軸方向の最小（＝始点側の面）
				double hiX = 0.0;	  // 軸方向の最大（＝終点側の面）
				double offAxis = 0.0; // 軸からの法線方向の離れ（中心）
			};

			// 柱のワールド外接矩形を PIO ローカルへ落として広がりを返す。
			ColumnRange LocalRangeOf(MCObjectHandle column, const VWTransformMatrix& toWorld)
			{
				WorldRect bounds;
				gSDK->GetObjectBounds(column, bounds);

				ColumnRange range;
				range.loX = std::numeric_limits<double>::max();
				range.hiX = std::numeric_limits<double>::lowest();
				double loY = std::numeric_limits<double>::max();
				double hiY = std::numeric_limits<double>::lowest();
				// 外接矩形の 4 隅をローカルへ落とす（壁が斜めでも広がりを取り違えない）。
				const std::array<double, 2> xs = {bounds.left, bounds.right};
				const std::array<double, 2> ys = {bounds.bottom, bounds.top};
				for (const double x : xs)
				{
					for (const double y : ys)
					{
						const VWPoint2D local = toWorld.InversePointTransform(VWPoint2D(x, y));
						range.loX = std::min(range.loX, local.x);
						range.hiX = std::max(range.hiX, local.x);
						loY = std::min(loY, local.y);
						hiY = std::max(hiY, local.y);
					}
				}
				range.offAxis = (loY + hiY) / 2.0;
				return range;
			}

			// 対象レイヤを走査して、軸の両端に最も近い柱の**内側面**を求める。両端とも
			// 見つかれば true（outStart < outEnd）。
			bool ClearSpanFromColumns(const std::vector<std::string>& layers,
									  const VWTransformMatrix& toWorld, double startX, double endX,
									  double& outStart, double& outEnd)
			{
				bool haveStart = false;
				bool haveEnd = false;
				double bestStart = kColumnAlongTol;
				double bestEnd = kColumnAlongTol;

				for (const std::string& name : layers)
				{
					const MCObjectHandle layer = gSDK->GetNamedLayer(TXString(name.c_str()));
					if (layer == nil)
						continue; // その階の柱レイヤが生成されていない

					for (MCObjectHandle h = gSDK->FirstMemberObj(layer); h != nil;
						 h = gSDK->NextObject(h))
					{
						if (draw::StructuralUseOf(h) != core::kStructuralUseColumn)
							continue; // 柱（構造用途 4）だけを見る（小屋束・梁は取らない）

						const ColumnRange range = LocalRangeOf(h, toWorld);
						if (std::abs(range.offAxis) > kColumnOffAxisTol)
							continue; // 別の通りの柱

						const double centre = (range.loX + range.hiX) / 2.0;
						if (const double toStart = std::abs(centre - startX); toStart < bestStart)
						{
							bestStart = toStart;
							outStart = range.hiX; // 始点側の柱の**終点寄りの面**が内法の始まり
							haveStart = true;
						}
						if (const double toEnd = std::abs(centre - endX); toEnd < bestEnd)
						{
							bestEnd = toEnd;
							outEnd = range.loX; // 終点側の柱の**始点寄りの面**が内法の終わり
							haveEnd = true;
						}
					}
				}
				return haveStart && haveEnd && outEnd > outStart;
			}

			// 壁面内の 2D 点列（x＝軸方向・y＝高さ）を、法線方向 offset の鉛直面へ置いた
			// 3D ポリゴンとして PIO のジオメトリに加える。className が空ならクラスを与えない
			// （PIO 本体のクラスがそのまま効く）。
			void AddPolygon3D(MCObjectHandle host, const std::vector<core::Vec2>& points,
							  double offset, const char* className)
			{
				if (points.size() < 3)
					return;
				VWPolygon3D shape;
				for (const core::Vec2& point : points)
					shape.AddVertex(point.x, offset, point.y);
				VWPolygon3DObj poly(shape);
				const MCObjectHandle handle = poly.GetThisObject();
				if (handle == nil)
					return;
				poly.SetClosed(true);
				gSDK->AddObjectToContainer(handle, host);
				if (className != nullptr && className[0] != '\0')
				{
					draw::SetClassByName(handle, className);
					draw::SetAllAttributesByClass(handle);
				}
			}

			// 伏図記号 1 つ（シンボルの配置）。定義は draw/ShearWall の EnsureMarkSymbols が
			// 用意しておく（Extensions/ExtShearWall.h の kShearMark*Symbol）。
			//
			// ★**VWFC で作ったシンボルインスタンスはどのコンテナにも入らない**ので
			//   AddObjectToContainer で PIO（host）へ入れ直す（draw/Symbol.cpp の 1 番目の作法）。
			//   同じ場所で使っている gSDK->CreateLine / CreateOval は PIO のジオメトリへ自動で
			//   入るが、**シンボルは入らない**——ここを揃えて書くと静かに消える。
			// ★**非 nil を成功と読まない**（PlaceSymbol は定義が無くても非 nil を返す。
			//   draw/Symbol.cpp の 2 番目の作法）。定義が用意できていない図面では、記号を
			//   置かずに黙って通す——ログには EnsureMarkSymbols が理由を残している。
			void AddMarkSymbol(MCObjectHandle host, const char* name, double x, double y,
							   double scaleX, double scaleY)
			{
				const TXString symbol(name);
				VWSymbolObj instance(symbol, VWPoint2D(x, y), 0.0);
				const MCObjectHandle handle = instance.GetThisObject();
				if (handle == nil || !VWSymbolObj::IsSymbolObject(handle, symbol))
					return;
				// **反転は負の倍率で与える**（VW が反転したシンボルを表す唯一の形。中身は
				// ovSymbolXScaleFactor / ovSymbolYScaleFactor）。X と Y を別々に持たせるには
				// 倍率の種別を**非対称**にしておく必要があるので、先に立てる。
				instance.SetScaleType(kScaleTypeAsymmetric);
				instance.SetScaleFactorX(scaleX);
				instance.SetScaleFactorY(scaleY);
				gSDK->AddObjectToContainer(handle, host);
				draw::SetClassByName(handle, kShearMarkClass);
			}

			// 伏図の筋かい記号 1 つ（**直角三角形**）。斜辺の傾きがそのまま筋かいの向き
			// （足元→頂部）を表す。たすき掛けは risesToEnd を反転してもう 1 つ重ねる
			// （同じ矩形の中で斜辺が交差する＝たすきに見える）。置き場所は内法の中央で、
			// 三角は**記号を寄せた側へさらに外側**へ伸びる。
			//
			// **定義は 1 つで、4 通りの向きは軸ごとの反転で作る**（対応表は
			// Extensions/ExtShearWall.h）。斜辺の向きが X の反転、寄せる側が Y の反転。
			void AddBraceTriangle(MCObjectHandle host, double centre, double offset,
								  bool risesToEnd)
			{
				AddMarkSymbol(host, kShearMarkBraceSymbol, centre, offset, risesToEnd ? 1.0 : -1.0,
							  offset >= 0.0 ? 1.0 : -1.0);
			}

			// 伏図の丸印 1 つ（面材の記号。壁に平行な線の中央に置く）。
			void AddPanelCircle(MCObjectHandle host, double centre, double offset)
			{
				AddMarkSymbol(host, kShearMarkPanelSymbol, centre, offset, 1.0, 1.0);
			}

			// 伏図の面材記号 1 つ。壁に平行な線と、その中央の丸。
			void AddPanelMark(MCObjectHandle host, double clearStart, double clearEnd,
							  double offset)
			{
				const MCObjectHandle line =
					gSDK->CreateLine(WorldPt(clearStart, offset), WorldPt(clearEnd, offset));
				if (line != nil)
				{
					draw::SetClassByName(line, kShearMarkClass);
					draw::SetAllAttributesByClass(line);
				}

				AddPanelCircle(host, (clearStart + clearEnd) / 2.0, offset);
			}

			// 軸組図の面材 1 枚（軸組内法を埋める矩形）。
			//
			// ★**面は壁芯（法線方向 0）へ置く。実物の離れ（板の中心面）へは置かない。**
			// 軸組図は**通り芯＝壁芯で切った断面ビューポート**で、切断面より奥は表示しない
			// 設定にしてある（draw/Section）。実物どおり壁芯から 58.5mm 外した面は、表側は
			// 手前で切り落とされ裏側は奥に隠れて、**どちらも図に出ない**（実機で確認。M19）。
			// 筋かいの帯が出るのは offset 0＝切断面の上に載っているからで、面材も同じ扱いにする。
			// 両面のときは 2 枚が同じ位置に重なるが、ハッチングは重ねて見える（クラス属性の
			// 塗りが透ける場合。表裏の見分けはそこに委ねる）。
			void AddPanelFace(MCObjectHandle host, double clearStart, double clearEnd,
							  double bottom, double top, const char* className)
			{
				AddPolygon3D(host,
							 {core::Vec2{clearStart, bottom}, core::Vec2{clearEnd, bottom},
							  core::Vec2{clearEnd, top}, core::Vec2{clearStart, top}},
							 0.0, className);
			}

			// 診断ログ用の数値（小数 1 桁）。
			std::string Number(double value)
			{
				std::ostringstream out;
				out.precision(1);
				out << std::fixed << value;
				return out.str();
			}

			// **PIO が実際に持っているパラメータを診断ログへ 1 度だけ書き出す。**
			//
			// パラメータが登録されていなければ setter も getter も黙って通らず、絵が痩せる
			// （最悪は何も描かれない）——実機でしか起きないうえ、症状からは原因が
			// 「解析が値を出していない」のか「PIO に届いていない」のか区別できない。
			// 登録済みの名前を並べておけば、そのどちらかが 1 行で分かる。
			// ログが開いていなければ何もしない（dev ビルドと HOMESKZ_IFC_TRACE のときだけ。
			// core/Trace.h）。
			void TraceParameters(const VWParametricObj& pio)
			{
				static bool logged = false;
				if (logged || !core::trace::isOpen())
					return;
				logged = true;
				try
				{
					const std::size_t count = pio.GetParamsCount();
					std::string names;
					for (std::size_t i = 0; i < count; ++i)
					{
						if (!names.empty())
							names += ", ";
						names += pio.GetParamName(i).GetStdString();
					}
					core::trace::log("shearwall: PIO のパラメータ " + std::to_string(count) +
									 " 個: " + names);
				}
				catch (...)
				{
					core::trace::log("shearwall: PIO のパラメータ一覧を読めない");
				}
			}
		} // namespace

		// -------------------------------------------------------------------
		EObjectEvent recalculateShearWall(MCObjectHandle object)
		{
			// リセット以外の経路で空のまま呼ばれても落とさないよう nil を見ておく。
			if (object == nil)
				return kObjectEventNoErr;

			try
			{
				const VWParametricObj self(object);
				TraceParameters(self);

				// 両端（柱芯）をローカルへ落とす。線分 PIO のローカル X が壁の向き、
				// +Y が表側になる（ヘッダ「座標系」）。
				VWTransformMatrix toWorld;
				self.GetObjectToWorldTransform(toWorld);

				// ★**両端が取れないことを想定する。** 線分として置けていれば
				// GetLinearObjectPos が 2 点を返すが、1 点のオブジェクトとして置かれていれば
				// 例外か縮退した 2 点が返る。そこで諦めると図面から耐力壁が丸ごと消える
				// （症状からは何が起きたか分からない）ので、**控えの内法から軸を組み直す**——
				// 挿入点は始端の柱芯・ローカル +X は壁の向き（draw/ShearWall が角度も与える）
				// なので、原点から控えの内法ぶん伸ばせば柱の探索窓としては十分に近い。
				double startX = 0.0;
				double endX = 0.0;
				bool haveAxis = false;
				try
				{
					VWPoint2D worldStart;
					VWPoint2D worldEnd;
					self.GetLinearObjectPos(worldStart, worldEnd);
					const VWPoint2D localStart = toWorld.InversePointTransform(worldStart);
					const VWPoint2D localEnd = toWorld.InversePointTransform(worldEnd);
					startX = localStart.x;
					endX = localEnd.x;
					haveAxis = (endX - startX) >= core::kPointEps;
					core::trace::log("  shearwall: 線分 world=[(" + Number(worldStart.x) + ", " +
									 Number(worldStart.y) + "), (" + Number(worldEnd.x) + ", " +
									 Number(worldEnd.y) + ")] local x=[" + Number(startX) + ", " +
									 Number(endX) + "]");
				}
				catch (...)
				{
					core::trace::log("  shearwall: 線分の両端を読めない（1 点として置かれている）");
				}

				const double fallbackSpan = ParamReal(self, kParamShearClearSpan);
				if (!haveAxis)
				{
					if (fallbackSpan <= 0.0)
					{
						core::trace::log("  shearwall: 軸も控えの内法も無いので描かない");
						return kObjectEventNoErr;
					}
					startX = 0.0;
					endX = fallbackSpan;
					core::trace::log("  shearwall: 軸を控えの内法から組み直す x=[0, " +
									 Number(endX) + "]");
				}

				// 軸組内法。**実物の柱から引くのが本筋**で、見つからないときだけ控えを使う。
				double clearStart = 0.0;
				double clearEnd = 0.0;
				const std::string targets = draw::PioParamString(self, kParamShearTargetLayers);
				const bool fromColumns = ClearSpanFromColumns(SplitLayers(targets), toWorld, startX,
															  endX, clearStart, clearEnd);
				if (!fromColumns)
				{
					if (fallbackSpan <= 0.0)
					{
						core::trace::log("  shearwall: 柱も控えの内法も無いので描かない");
						return kObjectEventNoErr;
					}
					const double centre = (startX + endX) / 2.0;
					clearStart = centre - (fallbackSpan / 2.0);
					clearEnd = centre + (fallbackSpan / 2.0);
				}

				const double span = clearEnd - clearStart;
				if (span <= 0.0)
					return kObjectEventNoErr;
				core::trace::log(std::string("  shearwall: 内法 ") +
								 (fromColumns ? "柱から" : "控え") + " x=[" + Number(clearStart) +
								 ", " + Number(clearEnd) + "]");

				// 記号を壁芯からどれだけ離すか（**図面 mm**）。記号そのものの大きさは
				// シンボル定義が持つので、ここで扱うのは置き場所だけ。
				const double markOffset =
					ParamReal(self, kParamShearMarkOffset, kShearMarkOffsetDefault);
				core::trace::log("  shearwall: 記号の離れ " + Number(markOffset) + "mm");

				// 軸組内法の高さ。**ここが取れなくても伏図の記号は描く**——記号は平面だけで
				// 決まるので、高さの取りこぼしで図面から耐力壁が丸ごと消えるのは割に合わない
				// （ヘッダ「絵を全部止めない」）。
				const double bottom = ParamReal(self, kParamShearBottom);
				const double top = ParamReal(self, kParamShearTop);
				const bool hasHeight = top > bottom;
				core::trace::log("  shearwall: 高さ z=[" + Number(bottom) + ", " + Number(top) +
								 "]" + (hasHeight ? "" : " ← 取れないので 3D は描かない"));

				if (draw::PioParamString(self, kParamShearKind) == kShearKindPanel)
				{
					// 面材。表＝+Y・裏＝−Y（ヘッダ「座標系」）。離れが分からなければ記号の
					// 大きさで代用する（伏図で線が壁芯に重なって読めなくなるのを避ける）。
					const std::string side = draw::PioParamString(self, kParamShearPanelSide);
					const bool front = side != kShearSideBack;
					const bool back = side == kShearSideBack || side == kShearSideBoth;

					// 伏図の記号は**実物の離れ（板の中心面）ではなく MarkOffset で置く**。実物の
					// 離れは半柱幅ほどしかなく、壁芯に載る横架材（土台・胴差）の下へ必ず潜る。
					// 表・裏の区別は「どちら側へ寄せるか」で保たれる。
					// 軸組図の面は逆に**壁芯へ置く**（AddPanelFace の doc コメント）。
					if (front)
					{
						AddPanelMark(object, clearStart, clearEnd, markOffset);
						if (hasHeight)
							AddPanelFace(object, clearStart, clearEnd, bottom, top,
										 kShearPanelFrontClass);
					}
					if (back)
					{
						AddPanelMark(object, clearStart, clearEnd, -markOffset);
						if (hasHeight)
							AddPanelFace(object, clearStart, clearEnd, bottom, top,
										 kShearPanelBackClass);
					}
					return kObjectEventNoErr;
				}

				// 筋かい。帯は内法の対角線に沿い、はみ出した角は内法で切る
				// （形は core::shearWallBracePolygon が決める＝無 SDK でテスト済み）。
				const bool risesToEnd =
					draw::PioParamString(self, kParamShearBraceRise) != kShearRiseStart;
				const bool doubleBrace =
					draw::PioParamString(self, kParamShearBraceStyle) == kShearBraceDouble;
				const double width = ParamReal(self, kParamShearWidth);

				// 伏図: 内法の中央へ直角三角形を 1 つ。たすき掛けは**同じ場所へ反転して重ねる**
				// （斜辺が交差してたすきに見える）。記号は壁芯に載る横架材を避けて表側（+Y）へ
				// 寄せる。
				const double markCentre = (clearStart + clearEnd) / 2.0;
				AddBraceTriangle(object, markCentre, markOffset, risesToEnd);
				if (doubleBrace)
					AddBraceTriangle(object, markCentre, markOffset, !risesToEnd);

				// 軸組図: 形状どおりの帯。見付け幅が取れないと帯にならないので、そのときは
				// 伏図の記号だけで済ませる。
				if (hasHeight && width > 0.0)
				{
					AddPolygon3D(object,
								 core::shearWallBracePolygon(clearStart, clearEnd, bottom, top,
															 width, risesToEnd),
								 0.0, "");
					if (doubleBrace)
						AddPolygon3D(object,
									 core::shearWallBracePolygon(clearStart, clearEnd, bottom, top,
																 width, !risesToEnd),
									 0.0, "");
				}
			}
			catch (...)
			{
				// 1 枚の異常で耐力壁全体を落とさない（CLAUDE.md「エラーハンドリング」）。
				// kObjectEventHadError を返すと VW がオブジェクトをエラー表示にするので、
				// ここまでに描けたものを残したまま正常終了として抜ける（柱記号 PIO と同じ）。
				core::trace::log("  shearwall: 例外（ここまでに描けたものを残して抜ける）");
				return kObjectEventNoErr;
			}
			return kObjectEventNoErr;
		}
	} // namespace draw
} // namespace HomeskzIfcImport
