#include "level.h"
#include "geometry.h"
#include "gameObject.h"
#include "player.h"
#include <DXL2_Game/physics.h>
#include <DXL2_System/system.h>
#include <DXL2_System/math.h>
#include <DXL2_System/memoryPool.h>

#include <DXL2_Asset/spriteAsset.h>
#include <DXL2_Asset/textureAsset.h>
#include <DXL2_Asset/paletteAsset.h>
#include <DXL2_Asset/colormapAsset.h>
#include <DXL2_Asset/modelAsset.h>
#include <DXL2_LogicSystem/logicSystem.h>

#include <assert.h>
#include <algorithm>

namespace DXL2_Level
{
	// Allocate 2MB.
	#define DXL2_LEVEL_RUNTIME_SIZE (2 * 1024 * 1024)
	
	static MemoryPool* s_memoryPool;
	static LevelData* s_levelData;
	static Vec2f* s_vertexCache;
	static SectorBaseHeight* s_baseSectorHeight;
	
	static Player* s_player = nullptr;

	static s32 s_floorCount = 0;
	static s32 s_secAltCount = 0;
	static const s32* s_floorId = nullptr;
	static const s32* s_secAltId = nullptr;

	GameObjectList* s_objects;
	SectorObjectList* s_sectorObjects;

	bool init()
	{
		s_memoryPool = new MemoryPool();
		s_memoryPool->init(DXL2_LEVEL_RUNTIME_SIZE, "Level_Runtime_Memory");
		s_memoryPool->setWarningWatermark(DXL2_LEVEL_RUNTIME_SIZE * 3 / 4);

		s_levelData = nullptr;
		s_vertexCache = nullptr;
		s_baseSectorHeight = nullptr;

		return s_memoryPool;
	}

	void shutdown()
	{
		s_memoryPool->clear();
	}

	void startLevel(LevelData* level, LevelObjectData* levelObj)
	{
		s_memoryPool->clear();
		if (!level) { return; }

		s_levelData = level;

		// Allocating a cache for all the vertices is a bit of a waste - we really only need it
		// when transforming sectors (move_wall, rotate, etc.) but an extra 2MB is probably worth
		// lower complexity.
		const size_t vtxSize = sizeof(Vec2f) * s_levelData->vertices.size();
		s_vertexCache = (Vec2f*)s_memoryPool->allocate(vtxSize);
		memcpy(s_vertexCache, s_levelData->vertices.data(), vtxSize);

		// Copy base floor and ceiling heights for texturing.
		const u32 sectorCount = (u32)s_levelData->sectors.size();
		s_baseSectorHeight = (SectorBaseHeight*)s_memoryPool->allocate(sectorCount * sizeof(SectorBaseHeight));
		for (u32 s = 0; s < sectorCount; s++)
		{
			s_baseSectorHeight[s].floorAlt = s_levelData->sectors[s].floorAlt;
			s_baseSectorHeight[s].ceilAlt  = s_levelData->sectors[s].ceilAlt;
		}

		s_objects = LevelGameObjects::getGameObjectList();
		s_sectorObjects = LevelGameObjects::getSectorObjectList();

		s_sectorObjects->resize(sectorCount);
		for (u32 i = 0; i < sectorCount; i++)
		{
			(*s_sectorObjects)[i].list.clear();
		}

		// Setup objects
		if (levelObj)
		{
			const u32 objectCount = levelObj->objectCount;
			const LevelObject* object = levelObj->objects.data();
			s_objects->reserve(objectCount + 1024);
			s_objects->resize(objectCount);
			for (u32 i = 0; i < objectCount; i++)
			{
				const ObjectClass oclass = object[i].oclass;
				if (oclass != CLASS_SPRITE && oclass != CLASS_FRAME && oclass != CLASS_3D) { continue; }

				// Get the position and sector.
				Vec3f pos = object[i].pos;
				s32 sectorId = DXL2_Physics::findSector(&pos);
				// Skip objects that are not in sectors.
				if (sectorId < 0) { continue; }

				std::vector<u32>& list = (*s_sectorObjects)[sectorId].list;
				list.push_back(i);

				GameObject* secobject = &(*s_objects)[i];
				secobject->id = i;
				secobject->pos = pos;
				secobject->angles = object[i].orientation;
				secobject->sectorId = sectorId;
				secobject->oclass = oclass;
				secobject->fullbright = false;
				secobject->collisionRadius = 0.0f;
				secobject->collisionHeight = 0.0f;
				secobject->physicsFlags = PHYSICS_GRAVITY;
				secobject->verticalVel = 0.0f;
				secobject->show = true;
				secobject->comFlags = object[i].comFlags;
				secobject->radius = object[i].radius;
				secobject->height = object[i].height;

				secobject->animId = 0;
				secobject->frameIndex = 0;

				if (!object[i].logics.empty() && (object[i].logics[0].type == LOGIC_LIFE || (object[i].logics[0].type >= LOGIC_BLUE && object[i].logics[0].type <= LOGIC_PILE)))
				{
					secobject->fullbright = true;
				}

				if (oclass == CLASS_SPRITE)
				{
					// HACK to fix Detention center, for some reason land-mines are mapped to Ewoks.
					if (!object[i].logics.empty() && object[i].logics[0].type == LOGIC_LAND_MINE)
					{
						secobject->oclass = CLASS_FRAME;
						secobject->frame = DXL2_Sprite::getFrame("WMINE.FME");
					}
					else
					{
						secobject->sprite = DXL2_Sprite::getSprite(levelObj->sprites[object[i].dataOffset].c_str());
					}

					// This is just temporary until Logics are implemented.
					if (!object[i].logics.empty() && object[i].logics[0].type >= LOGIC_I_OFFICER && object[i].logics[0].type <= LOGIC_REE_YEES2)
					{
						secobject->animId = 5;
					}
				}
				else if (oclass == CLASS_FRAME)
				{
					// HACK to fix Detention center, for some reason land-mines are mapped to Ewoks.
					if (!object[i].logics.empty() && object[i].logics[0].type == LOGIC_LAND_MINE)
					{
						secobject->frame = DXL2_Sprite::getFrame("WMINE.FME");
					}
					else
					{
						secobject->frame = DXL2_Sprite::getFrame(levelObj->frames[object[i].dataOffset].c_str());
					}
				}
				else if (oclass == CLASS_3D)
				{
					secobject->model = DXL2_Model::get(levelObj->pods[object[i].dataOffset].c_str());
					// 3D objects, by default, have no gravity since they are likely environmental props (like bridges).
					secobject->physicsFlags = PHYSICS_NONE;
				}

				// Register the object logic.
				DXL2_LogicSystem::registerObjectLogics(secobject, object[i].logics, object[i].generators);
			}
		}

		DXL2_System::logWrite(LOG_MSG, "Level", "Level Runtime used %u bytes.", s_memoryPool->getMemoryUsed());
	}

	void endLevel()
	{
		s_vertexCache = nullptr;
	}

	// Player (for interactions)
	void setPlayerSector(Player* player, s32 floorCount, s32 secAltCount, const s32* floorId, const s32* secAltId)
	{
		s_floorCount = floorCount;
		s_secAltCount = secAltCount;
		s_floorId = floorId;
		s_secAltId = secAltId;
		s_player = player;
	}

	bool playerOnFloor(s32 sectorId)
	{
		for (s32 f = 0; f < s_floorCount; f++)
		{
			if (s_floorId[f] == sectorId) { return true; }
		}
		return false;
	}

	bool playerOnSecAlt(s32 sectorId)
	{
		for (s32 f = 0; f < s_secAltCount; f++)
		{
			if (s_secAltId[f] == sectorId) { return true; }
		}
		return false;
	}
		
	void setObjectFloorHeight(s32 sectorId, f32 newHeight)
	{
		Sector* sector = s_levelData->sectors.data() + sectorId;
		GameObject* objects = s_objects->data();
		
		const std::vector<u32>& list = (*s_sectorObjects)[sectorId].list;
		const u32 objCount = (u32)list.size();
		const u32* indices = list.data();
		for (u32 i = 0; i < objCount; i++)
		{
			GameObject* obj = &objects[indices[i]];
			f32 testHeight = newHeight;
			f32 baseHeight = sector->floorAlt;
			if (sector->secAlt < 0.0f && obj->pos.y < sector->floorAlt + sector->secAlt + 0.1f)
			{
				testHeight += sector->secAlt;
				baseHeight += sector->secAlt;
			}

			// Move the object if it is close to the floor (so it sticks when going down)
			// or below the floor.
			// This way if an object is in the air (jumping, flying) it isn't moved unless necessary.
			if (obj->pos.y >= testHeight || fabsf(obj->pos.y - baseHeight) < 0.1f)
			{
				obj->pos.y = testHeight;
			}
		}
	}

	// Floor, ceiling, second height
	void setFloorHeight(s32 sectorId, f32 height, bool addSectorMotion)
	{
		if (addSectorMotion && playerOnSecAlt(sectorId))
		{
			s_player->pos.y = height + s_levelData->sectors[sectorId].secAlt;
		}
		else if (addSectorMotion && playerOnFloor(sectorId))
		{
			s_player->pos.y = height;
		}

		// Go through list of objects that are standing on the floor of the sector and add the motion to them.
		setObjectFloorHeight(sectorId, height);

		s_levelData->sectors[sectorId].floorAlt = height;
	}

	void setCeilingHeight(s32 sectorId, f32 height)
	{
		s_levelData->sectors[sectorId].ceilAlt = height;
	}

	void setSecondHeight(s32 sectorId, f32 height, bool addSectorMotion)
	{
		if (addSectorMotion && playerOnSecAlt(sectorId))
		{
			// Go through list of objects that are standing on the floor of the sector and add the motion to them.
			s_player->pos.y = s_levelData->sectors[sectorId].floorAlt + height;
		}

		s_levelData->sectors[sectorId].secAlt = height;
	}

	f32 getFloorHeight(s32 sectorId)
	{
		return s_levelData->sectors[sectorId].floorAlt;
	}

	f32 getCeilingHeight(s32 sectorId)
	{
		return s_levelData->sectors[sectorId].ceilAlt;
	}

	f32 getSecondHeight(s32 sectorId)
	{
		return s_levelData->sectors[sectorId].secAlt;
	}

	const SectorBaseHeight* getBaseSectorHeight(s32 sectorId)
	{
		return &s_baseSectorHeight[sectorId];
	}

	// Morph
	void moveWalls(s32 sectorId, f32 angle, f32 distance, f32 deltaDist, bool addSectorMotion, bool addSecMotionSecondAlt, bool useVertexCache)
	{
		const f32 angleRad = angle * PI / 180.0f - PI * 0.5f;
		const f32 ca =  cosf(angleRad);
		const f32 sa = -sinf(angleRad);

		if ((addSectorMotion && playerOnFloor(sectorId)) || (addSecMotionSecondAlt && playerOnSecAlt(sectorId)))
		{
			// Go through list of objects that are standing on the floor of the sector and add the motion to them.
			s_player->pos.x += ca * deltaDist;
			s_player->pos.z += sa * deltaDist;
		}

		// gather vertex indices.
		u32 indices[1024];
		u32 indexCount = 0;

		Sector* sector = &s_levelData->sectors[sectorId];
		SectorWall* walls = s_levelData->walls.data();
		const u32 wallCount = sector->wallCount;
		for (u32 w = 0; w < wallCount; w++)
		{
			SectorWall* wall = &walls[sector->wallOffset + w];
			if (wall->flags[0] & WF1_WALL_MORPHS)
			{
				indices[indexCount++] = wall->i0 + sector->vtxOffset;

				if (wall->adjoin >= 0)
				{
					Sector* adjoinedSec = &s_levelData->sectors[wall->adjoin];
					SectorWall* adjoined = &walls[adjoinedSec->wallOffset + wall->mirror];
					if (adjoined->flags[0] & WF1_WALL_MORPHS)
					{
						indices[indexCount++] = adjoined->i0 + adjoinedSec->vtxOffset;
					}
				}
			}
		}

		const Vec2f* srcVtx = s_vertexCache;
		Vec2f* dstVtx = s_levelData->vertices.data();
		if (useVertexCache)
		{
			for (u32 i = 0; i < indexCount; i++)
			{
				const u32 idx = indices[i];
				dstVtx[idx].x = srcVtx[idx].x + ca * distance;
				dstVtx[idx].z = srcVtx[idx].z + sa * distance;
			}
		}
		else
		{
			for (u32 i = 0; i < indexCount; i++)
			{
				const u32 idx = indices[i];
				dstVtx[idx].x += ca * distance;
				dstVtx[idx].z += sa * distance;
			}
		}
	}

	void rotate(s32 sectorId, f32 angle, f32 angleDelta, const Vec2f* center, bool addSectorMotion, bool addSecMotionSecondAlt, bool useVertexCache)
	{
		const f32 angleRad = -angle * PI / 180.0f;
		const f32 ca = cosf(angleRad);
		const f32 sa = sinf(angleRad);

		if ((addSectorMotion && playerOnFloor(sectorId)) || (addSecMotionSecondAlt && playerOnSecAlt(sectorId)))
		{
			// Go through list of objects that are standing on the floor of the sector and add the motion to them.
			const f32 angleDeltaRad = -angleDelta * PI / 180.0f;
			const f32 dca = cosf(angleDeltaRad);
			const f32 dsa = sinf(angleDeltaRad);

			const f32 x = s_player->pos.x - center->x;
			const f32 z = s_player->pos.z - center->z;
			s_player->pos.x =  dca * x + dsa * z + center->x;
			s_player->pos.z = -dsa * x + dca * z + center->z;
			s_player->m_yaw += angleDelta * PI / 180.0f;
		}

		// gather vertex indices.
		u32 indices[1024];
		u32 indexCount = 0;

		Sector* sector = &s_levelData->sectors[sectorId];
		SectorWall* walls = s_levelData->walls.data();
		const u32 wallCount = sector->wallCount;
		for (u32 w = 0; w < wallCount; w++)
		{
			SectorWall* wall = &walls[sector->wallOffset + w];
			if (wall->flags[0] & WF1_WALL_MORPHS)
			{
				indices[indexCount++] = wall->i0 + sector->vtxOffset;

				if (wall->adjoin >= 0)
				{
					Sector* adjoinedSec = &s_levelData->sectors[wall->adjoin];
					SectorWall* adjoined = &walls[adjoinedSec->wallOffset + wall->mirror];
					if (adjoined->flags[0] & WF1_WALL_MORPHS)
					{
						indices[indexCount++] = adjoined->i0 + adjoinedSec->vtxOffset;
					}
				}
			}
		}

		const Vec2f* srcVtx = s_vertexCache;
		Vec2f* dstVtx = s_levelData->vertices.data();
		if (useVertexCache)
		{
			for (u32 i = 0; i < indexCount; i++)
			{
				const u32 idx = indices[i];
				const f32 x = srcVtx[idx].x - center->x;
				const f32 z = srcVtx[idx].z - center->z;

				dstVtx[idx].x =  ca*x + sa*z + center->x;
				dstVtx[idx].z = -sa*x + ca*z + center->z;
			}
		}
		else
		{
			for (u32 i = 0; i < indexCount; i++)
			{
				const u32 idx = indices[i];
				const f32 x = dstVtx[idx].x - center->x;
				const f32 z = dstVtx[idx].z - center->z;

				dstVtx[idx].x =  ca*x + sa*z + center->x;
				dstVtx[idx].z = -sa*x + ca*z + center->z;
			}
		}
	}

	// Textures.
	void moveTextureOffset(s32 sectorId, SectorPart part, const Vec2f* offsetDelta, bool addSectorMotion, bool addSecMotionSecondAlt)
	{
		if ((addSectorMotion && playerOnFloor(sectorId)) || (addSecMotionSecondAlt && playerOnSecAlt(sectorId)))
		{
			// Go through list of objects that are standing on the floor of the sector and add the motion to them.
			s_player->pos.x += offsetDelta->x;
			s_player->pos.z += offsetDelta->z;
			s_player->m_onScrollFloor = true;
		}

		Sector* sector = &s_levelData->sectors[sectorId];
		if (part == SP_FLOOR)
		{
			sector->floorTexture.offsetX += offsetDelta->x;
			sector->floorTexture.offsetY += offsetDelta->z;
		}
		else if (part == SP_CEILING)
		{
			sector->ceilTexture.offsetX += offsetDelta->x;
			sector->ceilTexture.offsetY += offsetDelta->z;
		}
		else if (part == SP_WALL)
		{
			for (u32 w = 0; w < sector->wallCount; w++)
			{
				SectorWall* wall = &s_levelData->walls[w + sector->wallOffset];
				if (wall->flags[0] & WF1_SCROLL_TOP_TEX)
				{
					wall->top.offsetX += offsetDelta->x;
					wall->top.offsetY += offsetDelta->z;
				}
				if (wall->flags[0] & WF1_SCROLL_MID_TEX)
				{
					wall->mid.offsetX += offsetDelta->x;
					wall->mid.offsetY += offsetDelta->z;
				}
				if (wall->flags[0] & WF1_SCROLL_BOT_TEX)
				{
					wall->bot.offsetX += offsetDelta->x;
					wall->bot.offsetY += offsetDelta->z;
				}
			}
		}
		else if (part == SP_SIGN)
		{
			for (u32 w = 0; w < sector->wallCount; w++)
			{
				SectorWall* wall = &s_levelData->walls[w + sector->wallOffset];
				if (wall->flags[0] & WF1_SCROLL_SIGN_TEX)
				{
					wall->sign.offsetX += offsetDelta->x;
					wall->sign.offsetY += offsetDelta->z;
				}
			}
		}
	}

	void setTextureOffset(s32 sectorId, SectorPart part, const Vec2f* offset, bool addSectorMotion, bool addSecMotionSecondAlt)
	{
		Sector* sector = &s_levelData->sectors[sectorId];
		if (part == SP_FLOOR)
		{
			const Vec2f prevPos = { sector->floorTexture.offsetX, sector->floorTexture.offsetY };
			sector->floorTexture.offsetX = offset->x + sector->floorTexture.baseOffsetX;
			sector->floorTexture.offsetY = offset->z + sector->floorTexture.baseOffsetY;

			if ((addSectorMotion && playerOnFloor(sectorId)) || (addSecMotionSecondAlt && playerOnSecAlt(sectorId)))
			{
				const Vec2f move = { sector->floorTexture.offsetX - prevPos.x, sector->floorTexture.offsetY - prevPos.z };

				// Go through list of objects that are standing on the floor of the sector and add the motion to them.
				s_player->pos.x += move.x;
				s_player->pos.z += move.z;
				s_player->m_onScrollFloor = true;
			}
		}
		else if (part == SP_CEILING)
		{
			sector->ceilTexture.offsetX = offset->x + sector->ceilTexture.baseOffsetX;
			sector->ceilTexture.offsetY = offset->z + sector->ceilTexture.baseOffsetY;
		}
		else if (part == SP_WALL)
		{
			for (u32 w = 0; w < sector->wallCount; w++)
			{
				SectorWall* wall = &s_levelData->walls[w + sector->wallOffset];
				if (wall->flags[0] & WF1_SCROLL_TOP_TEX)
				{
					wall->top.offsetX = offset->x + wall->top.baseOffsetX;
					wall->top.offsetY = offset->z + wall->top.baseOffsetY;
				}
				if (wall->flags[0] & WF1_SCROLL_MID_TEX)
				{
					wall->mid.offsetX = offset->x + wall->mid.baseOffsetX;
					wall->mid.offsetY = offset->z + wall->mid.baseOffsetY;
				}
				if (wall->flags[0] & WF1_SCROLL_BOT_TEX)
				{
					wall->bot.offsetX = offset->x + wall->bot.baseOffsetX;
					wall->bot.offsetY = offset->z + wall->bot.baseOffsetY;
				}
			}
		}
		else if (part == SP_SIGN)
		{
			for (u32 w = 0; w < sector->wallCount; w++)
			{
				SectorWall* wall = &s_levelData->walls[w + sector->wallOffset];
				if (wall->flags[0] & WF1_SCROLL_SIGN_TEX)
				{
					wall->sign.offsetX = offset->x + wall->sign.baseOffsetX;
					wall->sign.offsetY = offset->z + wall->sign.baseOffsetY;
				}
			}
		}
	}

	void setTextureFrame(s32 sectorId, SectorPart part, WallSubPart subpart, u32 frame, s32 wallId)
	{
		Sector* sector = &s_levelData->sectors[sectorId];
		if (part == SP_FLOOR)
		{
			sector->floorTexture.frame = frame;
		}
		else if (part == SP_CEILING)
		{
			sector->ceilTexture.frame = frame;
		}
		else if (part == SP_SIGN)
		{
			if (wallId < 0) { return; }
			s_levelData->walls[sector->wallOffset + wallId].sign.frame = frame;
		}
		else if (part == SP_WALL)
		{
			if (wallId < 0) { return; }
			if (subpart == WSP_TOP)
			{
				s_levelData->walls[sector->wallOffset + wallId].top.frame = frame;
			}
			else if (subpart == WSP_MID)
			{
				s_levelData->walls[sector->wallOffset + wallId].mid.frame = frame;
			}
			else if (subpart == WSP_BOT)
			{
				s_levelData->walls[sector->wallOffset + wallId].bot.frame = frame;
			}
		}
	}

	void toggleTextureFrame(s32 sectorId, SectorPart part, WallSubPart subpart, s32 wallId)
	{
		Sector* sector = &s_levelData->sectors[sectorId];
		if (part == SP_FLOOR)
		{
			sector->floorTexture.frame = !sector->floorTexture.frame;
		}
		else if (part == SP_CEILING)
		{
			sector->ceilTexture.frame = !sector->ceilTexture.frame;
		}
		else if (part == SP_SIGN)
		{
			if (wallId < 0) { return; }
			s_levelData->walls[sector->wallOffset + wallId].sign.frame = !s_levelData->walls[sector->wallOffset + wallId].sign.frame;
		}
		else if (part == SP_WALL)
		{
			if (wallId < 0) { return; }
			if (subpart == WSP_TOP)
			{
				s_levelData->walls[sector->wallOffset + wallId].top.frame = !s_levelData->walls[sector->wallOffset + wallId].top.frame;
			}
			else if (subpart == WSP_MID)
			{
				s_levelData->walls[sector->wallOffset + wallId].mid.frame = !s_levelData->walls[sector->wallOffset + wallId].mid.frame;
			}
			else if (subpart == WSP_BOT)
			{
				s_levelData->walls[sector->wallOffset + wallId].bot.frame = !s_levelData->walls[sector->wallOffset + wallId].bot.frame;
			}
		}
	}

	void setTextureId(s32 sectorId, SectorPart part, WallSubPart subpart, u32 textureId, s32 wallId)
	{
		Sector* sector = &s_levelData->sectors[sectorId];
		if (part == SP_FLOOR)
		{
			sector->floorTexture.texId = textureId;
		}
		else if (part == SP_CEILING)
		{
			sector->ceilTexture.texId = textureId;
		}
		else if (part == SP_SIGN)
		{
			if (wallId < 0) { return; }
			s_levelData->walls[sector->wallOffset + wallId].sign.texId = textureId;
		}
		else if (part == SP_WALL)
		{
			if (wallId < 0) { return; }
			if (subpart == WSP_TOP)
			{
				s_levelData->walls[sector->wallOffset + wallId].top.texId = textureId;
			}
			else if (subpart == WSP_MID)
			{
				s_levelData->walls[sector->wallOffset + wallId].mid.texId = textureId;
			}
			else if (subpart == WSP_BOT)
			{
				s_levelData->walls[sector->wallOffset + wallId].bot.texId = textureId;
			}
		}
	}

	Vec2f getTextureOffset(s32 sectorId, SectorPart part, WallSubPart subpart, s32 wallId)
	{
		const Sector* sector = &s_levelData->sectors[sectorId];
		if (part == SP_FLOOR)
		{
			return { sector->floorTexture.offsetX, sector->floorTexture.offsetY };
		}
		else if (part == SP_CEILING)
		{
			return { sector->ceilTexture.offsetX, sector->ceilTexture.offsetY };
		}
		else if (part == SP_SIGN && wallId >= 0)
		{
			return { s_levelData->walls[sector->wallOffset + wallId].sign.offsetX, s_levelData->walls[sector->wallOffset + wallId].sign.offsetY };
		}
		else if (part == SP_WALL && wallId >= 0)
		{
			if (part == WSP_TOP)
			{
				return { s_levelData->walls[sector->wallOffset + wallId].top.offsetX, s_levelData->walls[sector->wallOffset + wallId].top.offsetY };
			}
			else if (part == WSP_MID)
			{
				return { s_levelData->walls[sector->wallOffset + wallId].mid.offsetX, s_levelData->walls[sector->wallOffset + wallId].mid.offsetY };
			}
			else if (part == WSP_BOT)
			{
				return { s_levelData->walls[sector->wallOffset + wallId].bot.offsetX, s_levelData->walls[sector->wallOffset + wallId].bot.offsetY };
			}
		}
		return { 0.0f, 0.0 };
	}

	u32 getTextureFrame(s32 sectorId, SectorPart part, WallSubPart subpart, s32 wallId)
	{
		const Sector* sector = &s_levelData->sectors[sectorId];
		if (part == SP_FLOOR)
		{
			return sector->floorTexture.frame;
		}
		else if (part == SP_CEILING)
		{
			return sector->ceilTexture.frame;
		}
		else if (part == SP_SIGN && wallId >= 0)
		{
			return s_levelData->walls[sector->wallOffset + wallId].sign.frame;
		}
		else if (part == SP_WALL && wallId >= 0)
		{
			if (part == WSP_TOP)
			{
				return s_levelData->walls[sector->wallOffset + wallId].top.frame;
			}
			else if (part == WSP_MID)
			{
				return s_levelData->walls[sector->wallOffset + wallId].mid.frame;
			}
			else if (part == WSP_BOT)
			{
				return s_levelData->walls[sector->wallOffset + wallId].bot.frame;
			}
		}
		return 0;
	}

	u32 getTextureId(s32 sectorId, SectorPart part, WallSubPart subpart, s32 wallId)
	{
		const Sector* sector = &s_levelData->sectors[sectorId];
		if (part == SP_FLOOR)
		{
			return sector->floorTexture.texId;
		}
		else if (part == SP_CEILING)
		{
			return sector->ceilTexture.texId;
		}
		else if (part == SP_SIGN && wallId >= 0)
		{
			return s_levelData->walls[sector->wallOffset + wallId].sign.texId;
		}
		else if (part == SP_WALL && wallId >= 0)
		{
			if (part == WSP_TOP)
			{
				return s_levelData->walls[sector->wallOffset + wallId].top.texId;
			}
			else if (part == WSP_MID)
			{
				return s_levelData->walls[sector->wallOffset + wallId].mid.texId;
			}
			else if (part == WSP_BOT)
			{
				return s_levelData->walls[sector->wallOffset + wallId].bot.texId;
			}
		}
		return 0;
	}

	// Flags.
	void setFlagBits(s32 sectorId, SectorPart part, u32 flagIndex, u32 flagBits, s32 wallId)
	{
		Sector* sector = &s_levelData->sectors[sectorId];
		if (part == SP_FLOOR || part == SP_CEILING)
		{
			sector->flags[flagIndex] |= flagBits;
		}
		else if (part == SP_SIGN || part == SP_WALL)
		{
			if (wallId < 0) { return; }
			s_levelData->walls[sector->wallOffset + wallId].flags[flagIndex] = flagBits;
		}
	}

	void clearFlagBits(s32 sectorId, SectorPart part, u32 flagIndex, u32 flagBits, s32 wallId)
	{
		Sector* sector = &s_levelData->sectors[sectorId];
		if (part == SP_FLOOR || part == SP_CEILING)
		{
			sector->flags[flagIndex] &= ~flagBits;
		}
		else if (part == SP_SIGN || part == SP_WALL)
		{
			if (wallId < 0) { return; }
			s_levelData->walls[sector->wallOffset + wallId].flags[flagIndex] &= ~flagBits;
		}
	}

	u32 getFlagBits(s32 sectorId, SectorPart part, u32 flagIndex, s32 wallId)
	{
		Sector* sector = &s_levelData->sectors[sectorId];
		if (part == SP_FLOOR || part == SP_CEILING)
		{
			return sector->flags[flagIndex];
		}
		else if (part == SP_SIGN || part == SP_WALL)
		{
			if (wallId < 0) { return 0; }
			return s_levelData->walls[sector->wallOffset + wallId].flags[flagIndex];
		}
		return 0;
	}

	// Lighting.
	void setAmbient(s32 sectorId, u8 ambient)
	{
		s_levelData->sectors[sectorId].ambient = ambient;
	}

	u8 getAmbient(s32 sectorId)
	{
		return s_levelData->sectors[sectorId].ambient;
	}

	void turnOnTheLights()
	{
		const size_t count = s_levelData->sectors.size();
		Sector* sector = s_levelData->sectors.data();
		for (size_t i = 0; i < count; i++, sector++)
		{
			sector->ambient = sector->flags[2];
		}
	}

	// Ajoins
	void changeAdjoin(s32 sector1, s32 wall1, s32 sector2, s32 wall2)
	{
		const size_t count = s_levelData->sectors.size();
		Sector* sectors    = s_levelData->sectors.data();
		SectorWall* walls  = s_levelData->walls.data();

		walls[sectors[sector1].wallOffset + wall1].adjoin = sector2;
		walls[sectors[sector1].wallOffset + wall1].mirror = wall2;

		walls[sectors[sector2].wallOffset + wall2].adjoin = sector1;
		walls[sectors[sector2].wallOffset + wall2].mirror = wall1;
	}
}
