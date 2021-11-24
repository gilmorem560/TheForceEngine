#pragma once
#include <TFE_System/types.h>

struct SectorInfo
{
	f32 heights[2];			// Floor and Ceiling heights
	Vec2f floorTexOffset;	// Floor texture uv offset.
	Vec2f ceilTexOffset;	// Ceiling texture uv offset.
	s32 textureIds[2];		// Floor and Ceiling texture Ids.
	s32 lightLevel;			// Sector light level.
};

struct WallInfo
{
	Vec2f v0, v1;	// Worldspace XZ positions.
	Vec2f uv0, uv1;	// Vertex U range and wall V range (from top to bottom).
	s32 textureId;  // Wall texture ID.
	s32 lightLevel; // Wall light level (may be different than sector light level).
};

struct CameraGpu
{
	Vec3f pos;
	f32   viewMtx[9];
	f32   projMtx[16];
};

namespace TFE_CommandBuffer
{
	bool init();
	void destroy();

	void startFrame(CameraGpu* camera);
	void endFrame();

	void setSectorInfo(const SectorInfo& sectorInfo);
	void addSolidWall(const WallInfo& wallInfo);
	void addWallPart(const WallInfo& wallInfo, f32 bot, f32 top);
}  // TFE_CommandBuffer
