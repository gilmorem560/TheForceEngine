#include <TFE_System/profiler.h>
#include <TFE_Asset/modelAsset_jedi.h>
#include <TFE_Game/igame.h>
#include <TFE_Jedi/Level/level.h>
#include <TFE_Jedi/Level/rsector.h>
#include <TFE_Jedi/Level/robject.h>
#include <TFE_Jedi/Level/rtexture.h>
#include <TFE_Jedi/Math/fixedPoint.h>
#include <TFE_Jedi/Math/core_math.h>
#include <TFE_System/math.h>
#include <TFE_FrontEndUI/console.h>

#include "cmdBuffer.h"
#include "rclassicGpu.h"
#include "rsectorGpu.h"
#include "rflatGpu.h"
#include "rlightingGpu.h"
#include "rEdgePairGpu.h"
#include "rclassicGpuSharedState.h"
#include "robj3d_gpu/robj3dGpu.h"
#include "../rcommon.h"

// Temp
#include <TFE_Jedi/Renderer/RClassic_Float/rwallFloat.h>
#include <TFE_Jedi/Renderer/RClassic_Float/rsectorFloat.h>

using namespace TFE_Jedi::RClassic_Gpu;
#define PTR_OFFSET(ptr, base) size_t((u8*)ptr - (u8*)base)

namespace TFE_Jedi
{
	namespace
	{
		static TFE_Sectors_Gpu* s_ctx = nullptr;

		s32 wallSortX(const void* r0, const void* r1)
		{
			return ((const RWallSegmentFloat*)r0)->wallX0 - ((const RWallSegmentFloat*)r1)->wallX0;
		}

		s32 sortObjectsGpu(const void* r0, const void* r1)
		{
			SecObject* obj0 = *((SecObject**)r0);
			SecObject* obj1 = *((SecObject**)r1);

			const SectorCached* cached0 = &s_ctx->m_cachedSectors[obj0->sector->index];
			const SectorCached* cached1 = &s_ctx->m_cachedSectors[obj1->sector->index];

			if (obj0->type == OBJ_TYPE_3D && obj1->type == OBJ_TYPE_3D)
			{
				// Both objects are 3D.
				const f32 distSq0 = dotFloat(cached0->objPosVS[obj0->index], cached0->objPosVS[obj0->index]);
				const f32 distSq1 = dotFloat(cached1->objPosVS[obj1->index], cached1->objPosVS[obj1->index]);
				const f32 dist0 = sqrtf(distSq0);
				const f32 dist1 = sqrtf(distSq1);
				
				if (obj0->model->isBridge && obj1->model->isBridge)
				{
					return signZero(dist1 - dist0);
				}
				else if (obj0->model->isBridge == 1)
				{
					return -1;
				}
				else if (obj1->model->isBridge == 1)
				{
					return 1;
				}

				return signZero(dist1 - dist0);
			}
			else if (obj0->type == OBJ_TYPE_3D && obj0->model->isBridge)
			{
				return -1;
			}
			else if (obj1->type == OBJ_TYPE_3D && obj1->model->isBridge)
			{
				return 1;
			}

			// Default case:
			return signZero(cached1->objPosVS[obj1->index].z - cached0->objPosVS[obj0->index].z);
		}

		s32 cullObjects(RSector* sector, SecObject** buffer)
		{
			s32 drawCount = 0;
			SecObject** obj = sector->objectList;
			s32 count = sector->objectCount;

			const SectorCached* cached = &s_ctx->m_cachedSectors[sector->index];

			for (s32 i = count - 1; i >= 0 && drawCount < MAX_VIEW_OBJ_COUNT; i--, obj++)
			{
				// Search for the next allocated object.
				SecObject* curObj = *obj;
				while (!curObj)
				{
					obj++;
					curObj = *obj;
				}

				if (curObj->flags & OBJ_FLAG_NEEDS_TRANSFORM)
				{
					const s32 type = curObj->type;
					if (type == OBJ_TYPE_SPRITE || type == OBJ_TYPE_FRAME)
					{
						if (cached->objPosVS[curObj->index].z >= 1.0f)
						{
							buffer[drawCount++] = curObj;
						}
					}
					else if (type == OBJ_TYPE_3D)
					{
						const f32 radius = fixed16ToFloat(curObj->model->radius);
						const f32 zMax = cached->objPosVS[curObj->index].z + radius;
						// Near plane
						if (zMax < 1.0f) { continue; }

						// Left plane
						const f32 xMax = cached->objPosVS[curObj->index].x + radius;
						if (xMax < -zMax) { continue; }

						// Right plane
						const f32 xMin = cached->objPosVS[curObj->index].x - radius;
						if (xMin > zMax) { continue; }

						// The object straddles the near plane, so add it and move on.
						const f32 zMin = cached->objPosVS[curObj->index].z - radius;
						if (zMin < FLT_EPSILON)
						{
							buffer[drawCount++] = curObj;
							continue;
						}

						// Cull against the current "window."
						const f32 rcpZ = 1.0f / cached->objPosVS[curObj->index].z;
						const s32 x0 = roundFloat((xMin*s_rcgpuState.focalLength)*rcpZ) + s_screenXMid;
						if (x0 > s_windowMaxX_Pixels) { continue; }

						const s32 x1 = roundFloat((xMax*s_rcgpuState.focalLength)*rcpZ) + s_screenXMid;
						if (x1 < s_windowMinX_Pixels) { continue; }

						// Finally add the object to render.
						buffer[drawCount++] = curObj;
					}
				}
			}

			return drawCount;
		}

		void sprite_drawWax(s32 angle, SecObject* obj, vec3_float* cachedPosVS)
		{
			// Angles range from [0, 16384), divide by 512 to get 32 even buckets.
			s32 angleDiff = (angle - obj->yaw) >> 9;
			angleDiff &= 31;	// up to 32 views

			// Get the animation based on the object state.
			Wax* wax = obj->wax;
			WaxAnim* anim = WAX_AnimPtr(wax, obj->anim & 0x1f);
			if (anim)
			{
				// Then get the Sequence from the angle difference.
				WaxView* view = WAX_ViewPtr(wax, anim, 31 - angleDiff);
				// And finall the frame from the current sequence.
				WaxFrame* frame = WAX_FramePtr(wax, view, obj->frame & 0x1f);
				// Draw the frame.
				sprite_drawFrame((u8*)wax, frame, obj, cachedPosVS);
			}
		}
	}

	// Temp
	struct ClipRect
	{
		f32 x0, x1;
		f32 y0, y1;
		Vec3f planeNormal;
		Vec3f planeVertex;
	};

	static CameraGpu s_camera;
	static s32 s_viewStackDepth = 0;
	static ClipRect s_viewRectStack[MAX_ADJOIN_DEPTH + 1];
	static ClipRect* s_viewRect;
	static bool s_showClipRects = false;
	static bool s_setupCVar = true;

	bool viewRect_push(ClipRect* rect)
	{
		if (s_viewStackDepth > MAX_ADJOIN_DEPTH)
		{
			return false;
		}

		s_viewRectStack[s_viewStackDepth] = *rect;
		s_viewRect = &s_viewRectStack[s_viewStackDepth];
		s_viewStackDepth++;

		TFE_CommandBuffer::setClipRegion((s32)s_viewRect->x0, (s32)s_viewRect->y0, (s32)s_viewRect->x1, (s32)s_viewRect->y1);
		return true;
	}

	void viewRect_pop()
	{
		s_viewStackDepth--;
		s_viewRect = nullptr;
		if (s_viewStackDepth > 0)
		{
			s_viewRect = &s_viewRectStack[s_viewStackDepth - 1];
			TFE_CommandBuffer::setClipRegion((s32)s_viewRect->x0, (s32)s_viewRect->y0, (s32)s_viewRect->x1, (s32)s_viewRect->y1);
		}
	}
	// End

	void TFE_Sectors_Gpu::reset()
	{
		m_cachedSectors = nullptr;
		m_cachedSectorCount = 0;
	}
				
	void TFE_Sectors_Gpu::prepare()
	{
		if (s_setupCVar)
		{
			s_setupCVar = false;
			CVAR_BOOL(s_showClipRects, "show_clip_rects", 0, "Enable to see clip rects for adjoin culling.");
		}

		allocateCachedData();

		EdgePairFloat* flatEdge = &s_rcgpuState.flatEdgeList[s_flatCount];
		s_rcgpuState.flatEdge = flatEdge;
		flat_addEdges(s_screenWidth, s_minScreenX_Pixels, 0, s_rcgpuState.windowMaxY, 0, s_rcgpuState.windowMinY);

		s_camera.pos.x = s_rcgpuState.cameraPos.x;
		s_camera.pos.y = s_rcgpuState.eyeHeight;
		s_camera.pos.z = s_rcgpuState.cameraPos.z;
		
		// The GPU renderer can use proper vertical rotation pitch.
		// Build a compatible view matrix including vertical rotation.
		f32 sinPitch, cosPitch;
		sinCosFlt(-s_rcgpuState.cameraPitch, &sinPitch, &cosPitch);
		s_camera.viewMtx[0] = s_rcgpuState.cosYaw;
		s_camera.viewMtx[1] = 0.0f;
		s_camera.viewMtx[2] = s_rcgpuState.sinYaw;

		s_camera.viewMtx[3] = s_rcgpuState.sinYaw * sinPitch;
		s_camera.viewMtx[4] = cosPitch;
		s_camera.viewMtx[5] = -s_rcgpuState.cosYaw * sinPitch;

		s_camera.viewMtx[6] =  s_rcgpuState.sinYaw * cosPitch;
		s_camera.viewMtx[7] = -sinPitch;
		s_camera.viewMtx[8] = -s_rcgpuState.cosYaw * cosPitch;

		// Use the Jedi projection values instead of proper FOV and aspect ratio.
		Mat4 proj = TFE_Math::computeProjMatrixExplicit(2.0f*s_rcgpuState.focalLength/f32(s_width), 2.0f*s_rcgpuState.focalLenAspect/f32(s_height), 0.01f, 1000.0f);
		memcpy(s_camera.projMtx, proj.m, sizeof(f32) * 16);
		TFE_CommandBuffer::startFrame(&s_camera);

		ClipRect rect;
		rect.x0 = 0;
		rect.x1 = s_width - 1;
		rect.y0 = 0;
		rect.y1 = s_height - 1;
		rect.planeNormal = { 0.0f, 0.0f, -1.0f  };
		rect.planeVertex = { 0.0f, 0.0f, -0.02f };
		viewRect_push(&rect);
	}
	
	void TFE_Sectors_Gpu::endFrame()
	{
		viewRect_pop();
		assert(s_viewRect == nullptr);

		TFE_CommandBuffer::endFrame();
	}

	void transformPointByCameraFixedToFloat_Gpu(vec3_fixed* worldPoint, vec3_float* viewPoint)
	{
		const f32 x = fixed16ToFloat(worldPoint->x);
		const f32 y = fixed16ToFloat(worldPoint->y);
		const f32 z = fixed16ToFloat(worldPoint->z);

		viewPoint->x = x*s_rcgpuState.cosYaw + z*s_rcgpuState.sinYaw + s_rcgpuState.cameraTrans.x;
		viewPoint->y = y - s_rcgpuState.eyeHeight;
		viewPoint->z = z*s_rcgpuState.cosYaw + x*s_rcgpuState.negSinYaw + s_rcgpuState.cameraTrans.z;
	}

	static const ClipRect c_emptyRect = { 0 };

	f32 computePlaneDist(ClipRect* rect, Vec3f* vtx)
	{
		Vec3f r = { vtx->x - rect->planeVertex.x, vtx->y - rect->planeVertex.y, vtx->z - rect->planeVertex.z };
		return r.x*rect->planeNormal.x + r.y*rect->planeNormal.y + r.z*rect->planeNormal.z;
	}

	bool computeClipRect(RSector* sector, RWall* wall, ClipRect* rect)
	{
		RSector* nextSector = wall->nextSector;
		if (!nextSector)
		{
			return false;
		}

		// Calculate the y range (bottom, top).
		f32 y0, y1;
		const s32 df = wall->drawFlags;
		assert(df >= 0);
		if (df <= WDF_MIDDLE)
		{
			if (df == WDF_MIDDLE || (nextSector->flags1 & SEC_FLAGS1_EXT_FLOOR_ADJ))
			{
				y0 = fixed16ToFloat(sector->floorHeight);
				y1 = fixed16ToFloat(sector->ceilingHeight);
			}
			else
			{
				y0 = fixed16ToFloat(nextSector->floorHeight);
				y1 = fixed16ToFloat(sector->ceilingHeight);
			}
		}
		else if (df == WDF_TOP)
		{
			if (nextSector->flags1 & SEC_FLAGS1_EXT_ADJ)
			{
				y0 = fixed16ToFloat(sector->floorHeight);
				y1 = fixed16ToFloat(nextSector->ceilingHeight);
			}
			else
			{
				y0 = fixed16ToFloat(sector->floorHeight);
				y1 = fixed16ToFloat(nextSector->ceilingHeight);
			}
		}
		else if (df == WDF_TOP_AND_BOT)
		{
			if ((nextSector->flags1 & SEC_FLAGS1_EXT_ADJ) && (nextSector->flags1 & SEC_FLAGS1_EXT_FLOOR_ADJ))
			{
				// TODO
				return false;
			}
			else if (nextSector->flags1 & SEC_FLAGS1_EXT_ADJ)
			{
				y0 = fixed16ToFloat(nextSector->floorHeight);
				y1 = fixed16ToFloat(sector->ceilingHeight);
			}
			else if (nextSector->flags1 & SEC_FLAGS1_EXT_FLOOR_ADJ)
			{
				y0 = fixed16ToFloat(sector->floorHeight);
				y1 = fixed16ToFloat(nextSector->ceilingHeight);
			}
			else
			{
				y0 = fixed16ToFloat(nextSector->floorHeight);
				y1 = fixed16ToFloat(nextSector->ceilingHeight);
			}
		}
		else // WDF_BOT
		{
			if (nextSector->flags1 & SEC_FLAGS1_EXT_FLOOR_ADJ)
			{
				// TODO
				return false;
			}
			else
			{
				y0 = fixed16ToFloat(nextSector->floorHeight);
				y1 = fixed16ToFloat(sector->ceilingHeight);
			}
		}

		// build world space quad.
		const f32 x0 = fixed16ToFloat(wall->w0->x);
		const f32 x1 = fixed16ToFloat(wall->w1->x);
		const f32 z0 = fixed16ToFloat(wall->w0->z);
		const f32 z1 = fixed16ToFloat(wall->w1->z);

		Vec3f vtxWS[4];
		vtxWS[0] = { x0, y1, z0 };
		vtxWS[1] = { x1, y1, z1 };
		vtxWS[2] = { x1, y0, z1 };
		vtxWS[3] = { x0, y0, z0 };

		// transform to view space.
		Vec3f vtxVS[4];
		const f32* viewMtx = s_camera.viewMtx;
		for (s32 i = 0; i < 4; i++)
		{
			Vec3f rpos = { vtxWS[i].x - s_camera.pos.x, vtxWS[i].y - s_camera.pos.y,  vtxWS[i].z - s_camera.pos.z };

			vtxVS[i].x = rpos.x*viewMtx[0] + rpos.y*viewMtx[1] + rpos.z*viewMtx[2];
			vtxVS[i].y = rpos.x*viewMtx[3] + rpos.y*viewMtx[4] + rpos.z*viewMtx[5];
			vtxVS[i].z = rpos.x*viewMtx[6] + rpos.y*viewMtx[7] + rpos.z*viewMtx[8];
		}

		// back face culling.
		Vec3f S = { vtxVS[1].x - vtxVS[0].x, vtxVS[1].y - vtxVS[0].y , vtxVS[1].z - vtxVS[0].z };
		Vec3f T = { vtxVS[3].x - vtxVS[0].x, vtxVS[3].y - vtxVS[0].y , vtxVS[3].z - vtxVS[0].z };
		Vec3f N = TFE_Math::cross(&S, &T);
		if (TFE_Math::dot(&vtxVS[0], &N) > -FLT_EPSILON)
		{
			return false;
		}
		rect->planeNormal = TFE_Math::normalize(&N);
		rect->planeNormal = { -rect->planeNormal.x, -rect->planeNormal.y, -rect->planeNormal.z };
		rect->planeVertex = vtxVS[0];

		// determine if the adjoin is completely behind the portal.
		Vec3f vtxVSClipped[16];
		s32 clippedVtx = 0;
		f32 nearPlane = -0.02f;
		for (s32 i = 0; i < 4; i++)
		{
			s32 a = i;
			s32 b = (i + 1) & 3;

			f32 pDistA = computePlaneDist(s_viewRect, &vtxVS[a]);
			f32 pDistB = computePlaneDist(s_viewRect, &vtxVS[b]);

			if (pDistA > 0.0f)
			{
				vtxVSClipped[clippedVtx++] = vtxVS[a];

				if (pDistB < 0.0f)
				{
					f32 s = (0.0f - pDistA) / (pDistB - pDistA);
					vtxVSClipped[clippedVtx].x = (1.0f - s)*vtxVS[a].x + s*vtxVS[b].x;
					vtxVSClipped[clippedVtx].y = (1.0f - s)*vtxVS[a].y + s*vtxVS[b].y;
					vtxVSClipped[clippedVtx].z = (1.0f - s)*vtxVS[a].z + s*vtxVS[b].z;
					clippedVtx++;
				}
			}
			else if (pDistB > 0.0f)
			{
				f32 s = (0.0f - pDistA) / (pDistB - pDistA);
				vtxVSClipped[clippedVtx].x = (1.0f - s)*vtxVS[a].x + s*vtxVS[b].x;
				vtxVSClipped[clippedVtx].y = (1.0f - s)*vtxVS[a].y + s*vtxVS[b].y;
				vtxVSClipped[clippedVtx].z = (1.0f - s)*vtxVS[a].z + s*vtxVS[b].z;
				clippedVtx++;
			}
		}
		if (!clippedVtx)
		{
			return false;
		}

		// finally project any vertices in front of the near plane.
		Vec4f vtxProj[16];
		Vec4f posSS[16];
		rect->x0 = s_width;
		rect->x1 = -1;
		rect->y0 = s_height;
		rect->y1 = -1;

		const f32* projMtx = s_camera.projMtx;
		const f32 halfWidth  = f32(s_width  / 2);
		const f32 halfHeight = f32(s_height / 2);
		for (s32 i = 0; i < clippedVtx; i++)
		{
			vtxProj[i].x = vtxVSClipped[i].x * projMtx[0]  + vtxVSClipped[i].y * projMtx[1]  + vtxVSClipped[i].z * projMtx[2]  + projMtx[3];
			vtxProj[i].y = vtxVSClipped[i].x * projMtx[4]  + vtxVSClipped[i].y * projMtx[5]  + vtxVSClipped[i].z * projMtx[6]  + projMtx[7];
			vtxProj[i].z = vtxVSClipped[i].x * projMtx[8]  + vtxVSClipped[i].y * projMtx[9]  + vtxVSClipped[i].z * projMtx[10] + projMtx[11];

			// pancake it?
			vtxProj[i].z = max(vtxProj[i].z, 0.001f);

			// assert(vtxProj[i].z >= 0.001f);
			//if (vtxProj[i].z >= 0.001f)
			{
				f32 rcpZ = 1.0f / vtxProj[i].z;
				posSS[i].x = vtxProj[i].x * rcpZ * halfWidth  + halfWidth;
				posSS[i].y = vtxProj[i].y * rcpZ * halfHeight + halfHeight;
				posSS[i].z = vtxProj[i].z * rcpZ;

				f32 x0 = floorf(posSS[i].x);
				f32 x1 = floorf(posSS[i].x + 0.5f);
				f32 y0 = floorf(posSS[i].y);
				f32 y1 = floorf(posSS[i].y + 0.5f);

				rect->x0 = min(x0, rect->x0);
				rect->x1 = max(x1, rect->x1);
				rect->y0 = min(y0, rect->y0);
				rect->y1 = max(y1, rect->y1);
			}
		}

		return true;
	}
	
	void TFE_Sectors_Gpu::draw(RSector* sector)
	{
		s_ctx = this;
		s_curSector = sector;
		s_sectorIndex++;
		/*
		s_adjoinIndex++;
		if (s_adjoinIndex > s_maxAdjoinIndex)
		{
			s_maxAdjoinIndex = s_adjoinIndex;
		}
		*/

		SectorCached* cached = &m_cachedSectors[sector->index];

		bool testAdjoins = false;
		if (s_curSector->prevDrawFrame != s_drawFrame)
		{
			testAdjoins = true;
			u32 quadStart = TFE_CommandBuffer::getWallQuadCount();

			s_curSector->prevDrawFrame = s_drawFrame;
			SectorInfo sectorInfo;
			sectorInfo.heights[0] = fixed16ToFloat(sector->floorHeight);
			sectorInfo.heights[1] = fixed16ToFloat(sector->ceilingHeight);
			// sectorInfo.floorTexOffset;
			// sectorInfo.ceilTexOffset;
			// sectorInfo.textureIds[0];
			// sectorInfo.textureIds[1];
			sectorInfo.lightLevel = floor16(sector->ambient);
			TFE_CommandBuffer::setSectorInfo(sectorInfo);

			vec2_fixed* vertices = sector->verticesWS;
			for (s32 w = 0; w < sector->wallCount; w++)
			{
				RWall* wall = &sector->walls[w];
				RSector* nextSector = wall->nextSector;

				if (!nextSector)
				{
					WallInfo wallInfo;
					wallInfo.v0 = { fixed16ToFloat(wall->w0->x), fixed16ToFloat(wall->w0->z) };
					wallInfo.v1 = { fixed16ToFloat(wall->w1->x), fixed16ToFloat(wall->w1->z) };
					wallInfo.lightLevel = max(0, floor16(sector->ambient + wall->wallLight));
					// Stubs
					wallInfo.textureId = 0;
					wallInfo.uv0 = { 0,0 };
					wallInfo.uv1 = { 1,1 };

					TFE_CommandBuffer::addSolidWall(wallInfo);
				}
				else
				{
					WallInfo wallInfo;
					wallInfo.v0 = { fixed16ToFloat(wall->w0->x), fixed16ToFloat(wall->w0->z) };
					wallInfo.v1 = { fixed16ToFloat(wall->w1->x), fixed16ToFloat(wall->w1->z) };
					wallInfo.lightLevel = max(0, floor16(sector->ambient + wall->wallLight));
					// Stubs
					wallInfo.textureId = 0;
					wallInfo.uv0 = { 0,0 };
					wallInfo.uv1 = { 1,1 };

					const s32 df = wall->drawFlags;
					assert(df >= 0);
					if (df <= WDF_MIDDLE)
					{
						if (df == WDF_MIDDLE || (nextSector->flags1 & SEC_FLAGS1_EXT_FLOOR_ADJ))
						{
							// wall_drawMask(wallSegment);
						}
						else
						{
							// wall_drawBottom(wallSegment);
							TFE_CommandBuffer::addWallPart(wallInfo, fixed16ToFloat(sector->floorHeight), fixed16ToFloat(nextSector->floorHeight));
						}
					}
					else if (df == WDF_TOP)
					{
						if (nextSector->flags1 & SEC_FLAGS1_EXT_ADJ)
						{
							// wall_drawMask(wallSegment);
						}
						else
						{
							// wall_drawTop(wallSegment);
							TFE_CommandBuffer::addWallPart(wallInfo, fixed16ToFloat(nextSector->ceilingHeight), fixed16ToFloat(sector->ceilingHeight));
						}
					}
					else if (df == WDF_TOP_AND_BOT)
					{
						if ((nextSector->flags1 & SEC_FLAGS1_EXT_ADJ) && (nextSector->flags1 & SEC_FLAGS1_EXT_FLOOR_ADJ))
						{
							// wall_drawMask(wallSegment);
						}
						else if (nextSector->flags1 & SEC_FLAGS1_EXT_ADJ)
						{
							// wall_drawBottom(wallSegment);
							TFE_CommandBuffer::addWallPart(wallInfo, fixed16ToFloat(sector->floorHeight), fixed16ToFloat(nextSector->floorHeight));
						}
						else if (nextSector->flags1 & SEC_FLAGS1_EXT_FLOOR_ADJ)
						{
							// wall_drawTop(wallSegment);
							TFE_CommandBuffer::addWallPart(wallInfo, fixed16ToFloat(nextSector->ceilingHeight), fixed16ToFloat(sector->ceilingHeight));
						}
						else
						{
							// wall_drawTopAndBottom(wallSegment);
							TFE_CommandBuffer::addWallPart(wallInfo, fixed16ToFloat(sector->floorHeight), fixed16ToFloat(nextSector->floorHeight));
							TFE_CommandBuffer::addWallPart(wallInfo, fixed16ToFloat(nextSector->ceilingHeight), fixed16ToFloat(sector->ceilingHeight));
						}
					}
					else // WDF_BOT
					{
						if (nextSector->flags1 & SEC_FLAGS1_EXT_FLOOR_ADJ)
						{
							// wall_drawMask(wallSegment);
						}
						else
						{
							// wall_drawBottom(wallSegment);
							TFE_CommandBuffer::addWallPart(wallInfo, fixed16ToFloat(sector->floorHeight), fixed16ToFloat(nextSector->floorHeight));
						}
					}
				}
			}

			u32 quadCount = TFE_CommandBuffer::getWallQuadCount() - quadStart + 1;
			cached->quadStart = quadStart;
			cached->quadCount = quadCount;
		}

		TFE_CommandBuffer::drawWalls(cached->quadStart, cached->quadCount);

		// TODO: Next Step - Clip/cull against the window plane.
		//if (testAdjoins)
		for (s32 w = 0; w < sector->wallCount && s_adjoinSegCount < MAX_ADJOIN_SEG; w++)
		{
			RWall* wall = &sector->walls[w];
			RSector* nextSector = wall->nextSector;

			if (nextSector)
			{
				ClipRect adjoinClipRect;
				if (!computeClipRect(sector, wall, &adjoinClipRect))
				{
					continue;
				}

				// The new clip rect is the intersection of the window and adjoin clip rects.
				ClipRect windowRect;
				windowRect.x0 = max(s_viewRect->x0, adjoinClipRect.x0);
				windowRect.x1 = min(s_viewRect->x1, adjoinClipRect.x1);
				windowRect.y0 = max(s_viewRect->y0, adjoinClipRect.y0);
				windowRect.y1 = min(s_viewRect->y1, adjoinClipRect.y1);
				windowRect.planeNormal = adjoinClipRect.planeNormal;
				windowRect.planeVertex = adjoinClipRect.planeVertex;
				if (windowRect.x0 > windowRect.x1 || windowRect.y0 > windowRect.y1)
				{
					continue;
				}

				if (viewRect_push(&windowRect))
				{
					if (s_showClipRects)
					{
						Vec2f w0 = { 2.0f*windowRect.x0 / f32(s_width) - 1.0f, 2.0f*windowRect.y0 / f32(s_height) - 1.0f };
						Vec2f w1 = { 2.0f*windowRect.x1 / f32(s_width) - 1.0f, 2.0f*windowRect.y1 / f32(s_height) - 1.0f };
						TFE_CommandBuffer::addDebugRect(&w0, &w1);
					}

					s_adjoinSegCount++;
					draw(nextSector);
					viewRect_pop();
				}
			}
		}
				
	#if 0
		s32* winTop = &s_windowTop_all[(s_adjoinDepth - 1) * s_width];
		s32* winBot = &s_windowBot_all[(s_adjoinDepth - 1) * s_width];
		s32* winTopNext = &s_windowTop_all[s_adjoinDepth * s_width];
		s32* winBotNext = &s_windowBot_all[s_adjoinDepth * s_width];

		s_rcgpuState.depth1d = &s_rcgpuState.depth1d_all[(s_adjoinDepth - 1) * s_width];

		s32 startWall = s_curSector->startWall;
		s32 drawWallCount = s_curSector->drawWallCnt;

		if (s_flatLighting)
		{
			s_sectorAmbient = s_flatAmbient;
		}
		else
		{
			s_sectorAmbient = round16(s_curSector->ambient);
		}
		s_scaledAmbient = (s_sectorAmbient >> 1) + (s_sectorAmbient >> 2) + (s_sectorAmbient >> 3);
		s_sectorAmbientFraction = s_sectorAmbient << 11;	// fraction of ambient compared to max.

		s_windowTop = winTop;
		s_windowBot = winBot;
		f32* depthPrev = nullptr;
		if (s_adjoinDepth > 1)
		{
			depthPrev = &s_rcgpuState.depth1d_all[(s_adjoinDepth - 2) * s_width];
			memcpy(&s_rcgpuState.depth1d[s_minScreenX_Pixels], &depthPrev[s_minScreenX_Pixels], s_width * 4);
		}

		s_wallMaxCeilY  = s_windowMinY_Pixels;
		s_wallMinFloorY = s_windowMaxY_Pixels;
		SectorCached* cachedSector = &m_cachedSectors[s_curSector->index];

		if (s_drawFrame != s_curSector->prevDrawFrame)
		{
			TFE_ZONE_BEGIN(secUpdateCache, "Update Sector Cache");
				updateCachedSector(cachedSector, s_curSector->dirtyFlags);
			TFE_ZONE_END(secUpdateCache);

			TFE_ZONE_BEGIN(secXform, "Sector Vertex Transform");
				vec2_fixed* vtxWS = s_curSector->verticesWS;
				vec2_float* vtxVS = cachedSector->verticesVS;
				for (s32 v = 0; v < s_curSector->vertexCount; v++)
				{
					const f32 x = fixed16ToFloat(vtxWS->x);
					const f32 z = fixed16ToFloat(vtxWS->z);

					vtxVS->x = x*s_rcgpuState.cosYaw     + z*s_rcgpuState.sinYaw + s_rcgpuState.cameraTrans.x;
					vtxVS->z = x*s_rcgpuState.negSinYaw  + z*s_rcgpuState.cosYaw + s_rcgpuState.cameraTrans.z;
					vtxVS++;
					vtxWS++;
				}
			TFE_ZONE_END(secXform);

			TFE_ZONE_BEGIN(objXform, "Sector Object Transform");
				SecObject** obj = s_curSector->objectList;
				vec3_float* objPosVS = cachedSector->objPosVS;
				for (s32 i = s_curSector->objectCount - 1; i >= 0; i--, obj++)
				{
					SecObject* curObj = *obj;
					while (!curObj)
					{
						obj++;
						curObj = *obj;
					}

					if (curObj->flags & OBJ_FLAG_NEEDS_TRANSFORM)
					{
						transformPointByCameraFixedToFloat_Gpu(&curObj->posWS, &objPosVS[curObj->index]);
					}
				}
			TFE_ZONE_END(objXform);

			TFE_ZONE_BEGIN(wallProcess, "Sector Wall Process");
				startWall = s_nextWall;
				WallCached* wall = cachedSector->cachedWalls;
				for (s32 i = 0; i < s_curSector->wallCount; i++, wall++)
				{
					wall_process(wall);
				}
				drawWallCount = s_nextWall - startWall;

				s_curSector->startWall = startWall;
				s_curSector->drawWallCnt = drawWallCount;
				s_curSector->prevDrawFrame = s_drawFrame;
			TFE_ZONE_END(wallProcess);
		}

		RWallSegmentFloat* wallSegment = &s_rcgpuState.wallSegListDst[s_curWallSeg];
		s32 drawSegCnt = wall_mergeSort(wallSegment, MAX_SEG - s_curWallSeg, startWall, drawWallCount);
		s_curWallSeg += drawSegCnt;

		TFE_ZONE_BEGIN(wallQSort, "Wall QSort");
			qsort(wallSegment, drawSegCnt, sizeof(RWallSegmentFloat), wallSortX);
		TFE_ZONE_END(wallQSort);

		s32 flatCount = s_flatCount;
		EdgePairFloat* flatEdge = &s_rcgpuState.flatEdgeList[s_flatCount];
		s_rcgpuState.flatEdge = flatEdge;

		s32 adjoinStart = s_adjoinSegCount;
		EdgePairFloat* adjoinEdges = &s_rcgpuState.adjoinEdgeList[adjoinStart];
		RWallSegmentFloat* adjoinList[MAX_ADJOIN_DEPTH];

		s_rcgpuState.adjoinEdge = adjoinEdges;
		s_rcgpuState.adjoinSegment = adjoinList;

		// Draw each wall segment in the sector.
		TFE_ZONE_BEGIN(secDrawWalls, "Draw Walls");
		for (s32 i = 0; i < drawSegCnt; i++, wallSegment++)
		{
			RWall* srcWall = wallSegment->srcWall->wall;
			RSector* nextSector = srcWall->nextSector;

			if (!nextSector)
			{
				wall_drawSolid(wallSegment);
			}
			else
			{
				const s32 df = srcWall->drawFlags;
				assert(df >= 0);
				if (df <= WDF_MIDDLE)
				{
					if (df == WDF_MIDDLE || (nextSector->flags1 & SEC_FLAGS1_EXT_FLOOR_ADJ))
					{
						wall_drawMask(wallSegment);
					}
					else
					{
						wall_drawBottom(wallSegment);
					}
				}
				else if (df == WDF_TOP)
				{
					if (nextSector->flags1 & SEC_FLAGS1_EXT_ADJ)
					{
						wall_drawMask(wallSegment);
					}
					else
					{
						wall_drawTop(wallSegment);
					}
				}
				else if (df == WDF_TOP_AND_BOT)
				{
					if ((nextSector->flags1 & SEC_FLAGS1_EXT_ADJ) && (nextSector->flags1 & SEC_FLAGS1_EXT_FLOOR_ADJ))
					{
						wall_drawMask(wallSegment);
					}
					else if (nextSector->flags1 & SEC_FLAGS1_EXT_ADJ)
					{
						wall_drawBottom(wallSegment);
					}
					else if (nextSector->flags1 & SEC_FLAGS1_EXT_FLOOR_ADJ)
					{
						wall_drawTop(wallSegment);
					}
					else
					{
						wall_drawTopAndBottom(wallSegment);
					}
				}
				else // WDF_BOT
				{
					if (nextSector->flags1 & SEC_FLAGS1_EXT_FLOOR_ADJ)
					{
						wall_drawMask(wallSegment);
					}
					else
					{
						wall_drawBottom(wallSegment);
					}
				}
			}
		}
		TFE_ZONE_END(secDrawWalls);

		TFE_ZONE_BEGIN(secDrawFlats, "Draw Flats");
			// Draw flats
			// Note: in the DOS code flat drawing functions are called through function pointers.
			// Since the function pointers always seem to be the same, the functions are called directly in this code.
			// Most likely this was used for testing or debug drawing and may be added back in the future.
			const s32 newFlatCount = s_flatCount - flatCount;
			if (s_curSector->flags1 & SEC_FLAGS1_EXTERIOR)
			{
				if (s_curSector->flags1 & SEC_FLAGS1_NOWALL_DRAW)
				{
					wall_drawSkyTopNoWall(s_curSector);
				}
				else
				{
					wall_drawSkyTop(s_curSector);
				}
			}
			else
			{
				flat_drawCeiling(cachedSector, flatEdge, newFlatCount);
			}
			if (s_curSector->flags1 & SEC_FLAGS1_PIT)
			{
				if (s_curSector->flags1 & SEC_FLAGS1_NOWALL_DRAW)
				{
					wall_drawSkyBottomNoWall(s_curSector);
				}
				else
				{
					wall_drawSkyBottom(s_curSector);
				}
			}
			else
			{
				flat_drawFloor(cachedSector, flatEdge, newFlatCount);
			}
		TFE_ZONE_END(secDrawFlats);

		// Adjoins
		s32 adjoinCount = s_adjoinSegCount - adjoinStart;
		if (adjoinCount && s_adjoinDepth < MAX_ADJOIN_DEPTH)
		{
			adjoin_setupAdjoinWindow(winBot, winBotNext, winTop, winTopNext, adjoinEdges, adjoinCount);
			RWallSegmentFloat** seg = adjoinList;
			RWallSegmentFloat* prevAdjoinSeg = nullptr;
			RWallSegmentFloat* curAdjoinSeg  = nullptr;

			s32 adjoinEnd = adjoinCount - 1;
			for (s32 i = 0; i < adjoinCount; i++, seg++, adjoinEdges++)
			{
				prevAdjoinSeg = curAdjoinSeg;
				curAdjoinSeg = *seg;

				RWall* srcWall = curAdjoinSeg->srcWall->wall;
				RWallSegmentFloat* nextAdjoin = (i < adjoinEnd) ? *(seg + 1) : nullptr;
				RSector* nextSector = srcWall->nextSector;
				if (s_adjoinDepth < MAX_ADJOIN_DEPTH && s_adjoinDepth < s_maxDepthCount)
				{
					s32 index = s_adjoinDepth - 1;
					saveValues(index);

					adjoin_computeWindowBounds(adjoinEdges);
					s_adjoinDepth++;
					if (s_adjoinDepth > s_maxAdjoinDepth)
					{
						s_maxAdjoinDepth = s_adjoinDepth;
					}

					srcWall->drawFrame = s_drawFrame;
					s_windowTop = winTopNext;
					s_windowBot = winBotNext;
					if (prevAdjoinSeg != 0)
					{
						if (prevAdjoinSeg->wallX1 + 1 == curAdjoinSeg->wallX0)
						{
							s_windowX0 = s_windowMinX_Pixels;
						}
					}
					if (nextAdjoin)
					{
						if (curAdjoinSeg->wallX1 == nextAdjoin->wallX0 - 1)
						{
							s_windowX1 = s_windowMaxX_Pixels;
						}
					}

					s_rcgpuState.windowMinZ = min(curAdjoinSeg->z0, curAdjoinSeg->z1);
					draw(nextSector);
					
					if (s_adjoinDepth)
					{
						s32 index = s_adjoinDepth - 2;
						s_adjoinDepth--;
						restoreValues(index);
					}
					srcWall->drawFrame = 0;
					if (srcWall->flags1 & WF1_ADJ_MID_TEX)
					{
						TFE_ZONE("Draw Transparent Walls");
						wall_drawTransparent(curAdjoinSeg, adjoinEdges);
					}
				}
			}
		}

		if (!(s_curSector->flags1 & SEC_FLAGS1_SUBSECTOR) && depthPrev && s_drawFrame != s_prevSector->prevDrawFrame2)
		{
			memcpy(&depthPrev[s_windowMinX_Pixels], &s_rcgpuState.depth1d[s_windowMinX_Pixels], (s_windowMaxX_Pixels - s_windowMinX_Pixels + 1) * sizeof(f32));
		}

		// Objects
		TFE_ZONE_BEGIN(secDrawObjects, "Draw Objects");
		const s32 objCount = cullObjects(s_curSector, s_objBuffer);
		if (objCount > 0)
		{
			// Which top and bottom edges are we going to use to clip objects?
			s_objWindowTop = s_windowTop;
			if (s_windowMinY_Pixels < s_screenYMidFlt || s_windowMaxCeil < s_screenYMidFlt)
			{
				if (s_prevSector && s_prevSector->ceilingHeight <= s_curSector->ceilingHeight)
				{
					s_objWindowTop = s_windowTopPrev;
				}
			}
			s_objWindowBot = s_windowBot;
			if (s_windowMaxY_Pixels > s_screenYMidFlt || s_windowMinFloor > s_screenYMidFlt)
			{
				if (s_prevSector && s_prevSector->floorHeight >= s_curSector->floorHeight)
				{
					s_objWindowBot = s_windowBotPrev;
				}
			}

			// Sort objects in viewspace (generally back to front but there are special cases).
			qsort(s_objBuffer, objCount, sizeof(SecObject*), sortObjectsGpu);

			// Draw objects in order.
			vec3_float* cachedPosVS = cachedSector->objPosVS;
			for (s32 i = 0; i < objCount; i++)
			{
				SecObject* obj = s_objBuffer[i];
				const s32 type = obj->type;
				if (type == OBJ_TYPE_SPRITE)
				{
					TFE_ZONE("Draw WAX");

					f32 dx = s_rcgpuState.cameraPos.x - fixed16ToFloat(obj->posWS.x);
					f32 dz = s_rcgpuState.cameraPos.z - fixed16ToFloat(obj->posWS.z);
					s32 angle = vec2ToAngle(dx, dz);

					sprite_drawWax(angle, obj, &cachedPosVS[obj->index]);
				}
				else if (type == OBJ_TYPE_3D)
				{
					TFE_ZONE("Draw 3DO");

					robj3d_draw(obj, obj->model);
				}
				else if (type == OBJ_TYPE_FRAME)
				{
					TFE_ZONE("Draw Frame");

					sprite_drawFrame((u8*)obj->fme, obj->fme, obj, &cachedPosVS[obj->index]);
				}
			}
		}
		TFE_ZONE_END(secDrawObjects);
	#endif

		s_curSector->flags1 |= SEC_FLAGS1_RENDERED;
		s_curSector->prevDrawFrame2 = s_drawFrame;
	}
		
	void TFE_Sectors_Gpu::adjoin_setupAdjoinWindow(s32* winBot, s32* winBotNext, s32* winTop, s32* winTopNext, EdgePairFloat* adjoinEdges, s32 adjoinCount)
	{
		TFE_ZONE("Setup Adjoin Window");

		// Note: This is pretty inefficient, especially at higher resolutions.
		// The column loops below can be adjusted to do the copy only in the required ranges.
		memcpy(&winTopNext[s_minScreenX_Pixels], &winTop[s_minScreenX_Pixels], s_width * 4);
		memcpy(&winBotNext[s_minScreenX_Pixels], &winBot[s_minScreenX_Pixels], s_width * 4);

		// Loop through each adjoin and setup the column range based on the edge pair and the parent
		// column range.
		for (s32 i = 0; i < adjoinCount; i++, adjoinEdges++)
		{
			const s32 x0 = adjoinEdges->x0;
			const s32 x1 = adjoinEdges->x1;

			const f32 ceil_dYdX = adjoinEdges->dyCeil_dx;
			f32 y = adjoinEdges->yCeil0;
			for (s32 x = x0; x <= x1; x++, y += ceil_dYdX)
			{
				s32 yPixel = roundFloat(y);
				s32 yBot = winBotNext[x];
				s32 yTop = winTop[x];
				if (yPixel > yTop)
				{
					winTopNext[x] = (yPixel <= yBot) ? yPixel : yBot + 1;
				}
			}
			const f32 floor_dYdX = adjoinEdges->dyFloor_dx;
			y = adjoinEdges->yFloor0;
			for (s32 x = x0; x <= x1; x++, y += floor_dYdX)
			{
				s32 yPixel = roundFloat(y);
				s32 yTop = winTop[x];
				s32 yBot = winBot[x];
				if (yPixel < yBot)
				{
					winBotNext[x] = (yPixel >= yTop) ? yPixel : yTop - 1;
				}
			}
		}
	}

	void TFE_Sectors_Gpu::adjoin_computeWindowBounds(EdgePairFloat* adjoinEdges)
	{
		s32 yC = adjoinEdges->yPixel_C0;
		if (yC > s_windowMinY_Pixels)
		{
			s_windowMinY_Pixels = yC;
		}
		s32 yF = adjoinEdges->yPixel_F0;
		if (yF < s_windowMaxY_Pixels)
		{
			s_windowMaxY_Pixels = yF;
		}
		yC = adjoinEdges->yPixel_C1;
		if (yC > s_windowMaxCeil)
		{
			s_windowMaxCeil = yC;
		}
		yF = adjoinEdges->yPixel_F1;
		if (yF < s_windowMinFloor)
		{
			s_windowMinFloor = yF;
		}
		s_wallMaxCeilY = s_windowMinY_Pixels - 1;
		s_wallMinFloorY = s_windowMaxY_Pixels + 1;
		s_windowMinX_Pixels = adjoinEdges->x0;
		s_windowMaxX_Pixels = adjoinEdges->x1;
		s_windowTopPrev = s_windowTop;
		s_windowBotPrev = s_windowBot;
		s_prevSector = s_curSector;
	}

	void TFE_Sectors_Gpu::saveValues(s32 index)
	{
		SectorSaveValues* dst = &s_sectorStack[index];
		dst->curSector = s_curSector;
		dst->prevSector = s_prevSector;
		dst->depth1d = s_rcgpuState.depth1d;
		dst->windowX0 = s_windowX0;
		dst->windowX1 = s_windowX1;
		dst->windowMinY = s_windowMinY_Pixels;
		dst->windowMaxY = s_windowMaxY_Pixels;
		dst->windowMaxCeil = s_windowMaxCeil;
		dst->windowMinFloor = s_windowMinFloor;
		dst->wallMaxCeilY = s_wallMaxCeilY;
		dst->wallMinFloorY = s_wallMinFloorY;
		dst->windowMinX = s_windowMinX_Pixels;
		dst->windowMaxX = s_windowMaxX_Pixels;
		dst->windowTop = s_windowTop;
		dst->windowBot = s_windowBot;
		dst->windowTopPrev = s_windowTopPrev;
		dst->windowBotPrev = s_windowBotPrev;
		dst->sectorAmbient = s_sectorAmbient;
		dst->scaledAmbient = s_scaledAmbient;
		dst->sectorAmbientFraction = s_sectorAmbientFraction;
	}

	void TFE_Sectors_Gpu::restoreValues(s32 index)
	{
		const SectorSaveValues* src = &s_sectorStack[index];
		s_curSector = src->curSector;
		s_prevSector = src->prevSector;
		s_rcgpuState.depth1d = (f32*)src->depth1d;
		s_windowX0 = src->windowX0;
		s_windowX1 = src->windowX1;
		s_windowMinY_Pixels = src->windowMinY;
		s_windowMaxY_Pixels = src->windowMaxY;
		s_windowMaxCeil = src->windowMaxCeil;
		s_windowMinFloor = src->windowMinFloor;
		s_wallMaxCeilY = src->wallMaxCeilY;
		s_wallMinFloorY = src->wallMinFloorY;
		s_windowMinX_Pixels = src->windowMinX;
		s_windowMaxX_Pixels = src->windowMaxX;
		s_windowTop = src->windowTop;
		s_windowBot = src->windowBot;
		s_windowTopPrev = src->windowTopPrev;
		s_windowBotPrev = src->windowBotPrev;
		s_sectorAmbient = src->sectorAmbient;
		s_scaledAmbient = src->scaledAmbient;
		s_sectorAmbientFraction = src->sectorAmbientFraction;
	}

	void TFE_Sectors_Gpu::freeCachedData()
	{
		level_free(m_cachedSectors);
		m_cachedSectors = nullptr;
		m_cachedSectorCount = 0;
	}
		
	void TFE_Sectors_Gpu::updateCachedWalls(SectorCached* cached, u32 flags)
	{
		if (!(flags & SDF_WALL_CHANGE)) { return; }

		RSector* srcSector = cached->sector;
		if (flags & SDF_INIT_SETUP)
		{
			cached->cachedWalls = (WallCached*)level_alloc(sizeof(WallCached) * srcSector->wallCount);
			memset(cached->cachedWalls, 0, sizeof(WallCached) * srcSector->wallCount);
		}

		for (s32 w = 0; w < srcSector->wallCount; w++)
		{
			WallCached* wcached = &cached->cachedWalls[w];
			RWall* srcWall = &srcSector->walls[w];

			if (flags & SDF_INIT_SETUP)
			{
				wcached->wall = srcWall;
				wcached->sector = cached;
				wcached->v0 = &cached->verticesVS[PTR_OFFSET(srcWall->v0, srcSector->verticesVS) / sizeof(vec2_fixed)];
				wcached->v1 = &cached->verticesVS[PTR_OFFSET(srcWall->v1, srcSector->verticesVS) / sizeof(vec2_fixed)];
			}

			if (flags & SDF_HEIGHTS)
			{
				wcached->topTexelHeight = fixed16ToFloat(srcWall->topTexelHeight);
				wcached->midTexelHeight = fixed16ToFloat(srcWall->midTexelHeight);
				wcached->botTexelHeight = fixed16ToFloat(srcWall->botTexelHeight);
			}

			if (flags & SDF_WALL_OFFSETS)
			{
				wcached->texelLength = fixed16ToFloat(srcWall->texelLength);

				wcached->topOffset.x = fixed16ToFloat(srcWall->topOffset.x);
				wcached->topOffset.z = fixed16ToFloat(srcWall->topOffset.z);
				wcached->midOffset.x = fixed16ToFloat(srcWall->midOffset.x);
				wcached->midOffset.z = fixed16ToFloat(srcWall->midOffset.z);
				wcached->botOffset.x = fixed16ToFloat(srcWall->botOffset.x);
				wcached->botOffset.z = fixed16ToFloat(srcWall->botOffset.z);
				wcached->signOffset.x = fixed16ToFloat(srcWall->signOffset.x);
				wcached->signOffset.z = fixed16ToFloat(srcWall->signOffset.z);
			}

			if (flags & SDF_WALL_SHAPE)
			{
				wcached->wallDir.x = fixed16ToFloat(srcWall->wallDir.x);
				wcached->wallDir.z = fixed16ToFloat(srcWall->wallDir.z);
				wcached->length = fixed16ToFloat(srcWall->length);
			}
		}
	}

	void TFE_Sectors_Gpu::updateCachedSector(SectorCached* cached, u32 flags)
	{
		RSector* srcSector = cached->sector;

		if (flags & SDF_INIT_SETUP)
		{
			cached->verticesVS = (vec2_float*)level_alloc(sizeof(vec2_float) * srcSector->vertexCount);
		}

		if (flags & SDF_HEIGHTS)
		{
			cached->floorHeight = fixed16ToFloat(srcSector->floorHeight);
			cached->ceilingHeight = fixed16ToFloat(srcSector->ceilingHeight);
		}

		if (flags & SDF_FLAT_OFFSETS)
		{
			cached->floorOffset.x = fixed16ToFloat(srcSector->floorOffset.x);
			cached->floorOffset.z = fixed16ToFloat(srcSector->floorOffset.z);
			cached->ceilOffset.x = fixed16ToFloat(srcSector->ceilOffset.x);
			cached->ceilOffset.z = fixed16ToFloat(srcSector->ceilOffset.z);
		}

		if (cached->objectCapacity < srcSector->objectCapacity)
		{
			cached->objectCapacity = srcSector->objectCapacity;
			cached->objPosVS = (vec3_float*)level_realloc(cached->objPosVS, sizeof(vec3_float) * cached->objectCapacity);
		}

		updateCachedWalls(cached, flags);
		srcSector->dirtyFlags = 0;
	}

	void TFE_Sectors_Gpu::allocateCachedData()
	{
		if (m_cachedSectorCount && m_cachedSectorCount != s_sectorCount)
		{
			freeCachedData();
		}

		if (!m_cachedSectors)
		{
			m_cachedSectorCount = s_sectorCount;
			m_cachedSectors = (SectorCached*)level_alloc(sizeof(SectorCached) * m_cachedSectorCount);
			memset(m_cachedSectors, 0, sizeof(SectorCached) * m_cachedSectorCount);

			for (u32 i = 0; i < m_cachedSectorCount; i++)
			{
				m_cachedSectors[i].sector = &s_sectors[i];
				updateCachedSector(&m_cachedSectors[i], SDF_ALL);
			}
		}
	}

	// Switch from float to fixed.
	void TFE_Sectors_Gpu::subrendererChanged()
	{
		freeCachedData();
	}
}