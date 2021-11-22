#pragma once
//////////////////////////////////////////////////////////////////////
// Wall
// Dark Forces Derived Renderer - Wall functions
//////////////////////////////////////////////////////////////////////
#include <TFE_System/types.h>
#include <TFE_Asset/spriteAsset_Jedi.h>
#include <TFE_Jedi/Level/robject.h>
#include <TFE_Jedi/Math/fixedPoint.h>
#include <TFE_Jedi/Math/core_math.h>
#include "../rlimits.h"
#include "../rwallRender.h"
#include "../rwallSegment.h"

struct RSector;

namespace TFE_Jedi
{
	struct EdgePairFloat;
	struct SectorCached;
	struct WallCached;

	namespace RClassic_Gpu
	{
		void wall_process(WallCached* wallCached);
		s32  wall_mergeSort(RWallSegmentFloat* segOutList, s32 availSpace, s32 start, s32 count);

		void wall_drawSolid(RWallSegmentFloat* wallSegment);
		void wall_drawTransparent(RWallSegmentFloat* wallSegment, EdgePairFloat* edge);
		void wall_drawMask(RWallSegmentFloat* wallSegment);
		void wall_drawBottom(RWallSegmentFloat* wallSegment);
		void wall_drawTop(RWallSegmentFloat* wallSegment);
		void wall_drawTopAndBottom(RWallSegmentFloat* wallSegment);

		void wall_drawSkyTop(RSector* sector);
		void wall_drawSkyTopNoWall(RSector* sector);
		void wall_drawSkyBottom(RSector* sector);
		void wall_drawSkyBottomNoWall(RSector* sector);

		void wall_addAdjoinSegment(s32 length, s32 x0, f32 top_dydx, f32 y1, f32 bot_dydx, f32 y0, RWallSegmentFloat* wallSegment);

		// Sprite code for now because so much is shared.
		void sprite_drawFrame(u8* basePtr, WaxFrame* frame, SecObject* obj, vec3_float* cachedPosVS);
	}
}
