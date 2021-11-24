#pragma once
//////////////////////////////////////////////////////////////////////
// Sector
//////////////////////////////////////////////////////////////////////
#include <TFE_System/types.h>
#include <TFE_System/memoryPool.h>
#include <TFE_Jedi/Math/fixedPoint.h>
#include <TFE_Jedi/Math/core_math.h>
#include "rwallGpu.h"
#include "rflatGpu.h"
#include "../rsectorRender.h"

struct RWall;
struct SecObject;

namespace TFE_Jedi
{
	struct WallCached;
	struct SectorCached;

	class TFE_Sectors_Gpu : public TFE_Sectors
	{
	public:
		TFE_Sectors_Gpu() : m_cachedSectors(nullptr), m_cachedSectorCount(0) {}

		// Sub-Renderer specific
		void reset() override;
		void prepare() override;
		void endFrame() override;

		void draw(RSector* sector) override;
		void subrendererChanged() override;

	private:
		void saveValues(s32 index);
		void restoreValues(s32 index);
		void adjoin_computeWindowBounds(EdgePairFloat* adjoinEdges);
		void adjoin_setupAdjoinWindow(s32* winBot, s32* winBotNext, s32* winTop, s32* winTopNext, EdgePairFloat* adjoinEdges, s32 adjoinCount);

		void freeCachedData();
		void allocateCachedData();
		void updateCachedSector(SectorCached* cached, u32 flags);
		void updateCachedWalls(SectorCached* cached, u32 flags);

	public:
		SectorCached* m_cachedSectors = nullptr;
		u32 m_cachedSectorCount = 0;
	};
}  // TFE_Jedi
