//
//	draw/HostServices.cpp
//
//	殻から借りた機能の置き場所（意図は draw/HostServices.h 参照）。**本体の中の 1 つきり**
//	で、入れるのも捨てるのも payload/PayloadMain.cpp だけ。
//

#include "draw/HostServices.h"

namespace HomeskzIfcImport::draw
{
	namespace
	{
		// 関数ローカル static。**名前空間スコープの変数にしない**——本体はアンロードされて
		// 再読み込みされるので、静的初期化の順序に依存させない（core/ の作法と同じ）。
		HostServices& storage()
		{
			static HostServices services;
			return services;
		}
	} // namespace

	void setHostServices(const HostServices& services)
	{
		storage() = services;
	}

	void clearHostServices()
	{
		storage() = HostServices{};
	}

	const HostServices& hostServices()
	{
		return storage();
	}
} // namespace HomeskzIfcImport::draw
