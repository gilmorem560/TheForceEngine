#include "cmdBuffer.h"
#include <TFE_Jedi/Level/level.h>
#include <TFE_Jedi/Level/rtexture.h>
#include <TFE_Jedi/Math/fixedPoint.h>
#include <TFE_Jedi/Math/core_math.h>
#include <TFE_Jedi/Renderer/virtualFramebuffer.h>
#include <TFE_Game/igame.h>
#include "rclassicGpuSharedState.h"
#include "rlightingGpu.h"
#include "rflatGpu.h"
#include "../redgePair.h"
#include "rsectorGpu.h"
#include "../rcommon.h"

// GPU
#include <TFE_RenderBackend/renderBackend.h>
#include <TFE_RenderBackend/vertexBuffer.h>
#include <TFE_RenderBackend/indexBuffer.h>
#include <TFE_RenderBackend/textureGpu.h>
#include <TFE_RenderBackend/shader.h>

namespace TFE_CommandBuffer
{
	enum Constants
	{
		MAX_QUADS_PER_DRAW = 16384,	// 16k quads
		MAX_SECTOR_QUADS = 16384,	// For now up to 16k sector quads are supported, but this can be increased.
		MAX_DEBUG_RECTS = 1024,
	};

	enum Command
	{
		CMD_DRAW_WALLS = 0,
		CMD_SET_CLIP_RECT,
	};

	enum
	{
		CMD_MAX = 1024
	};

	struct DrawCommand
	{
		Command cmd;
		u32 quadStart;
		u32 quadCount;
		s32 clipRect[4];
	};
		
	// 24 bytes.
	const AttributeMapping c_sectorAttribMap[] =
	{
		{ ATTR_POS,   ATYPE_FLOAT, 3, 0, false },
		{ ATTR_UV,    ATYPE_FLOAT, 2, 0, false },
		{ ATTR_COLOR, ATYPE_UINT8, 4, 0, false },
	};

	// 8 bytes.
	const AttributeMapping c_debugAttribMap[] =
	{
		{ ATTR_POS, ATYPE_FLOAT, 2, 0, false },
		{ ATTR_UV,  ATYPE_FLOAT, 2, 0, false },
	};

	struct SectorVertex
	{
		Vec3f pos;
		Vec2f uv;
		u32 color;
	};

	struct DebugVertex
	{
		Vec2f pos;
		Vec2f uv;
	};

	struct ShaderVariables
	{
		s32 cameraPos  = -1;
		s32 cameraView = -1;
		s32 cameraProj = -1;
	};
		
	static bool s_cmdBufferInit = false;
	static IndexBuffer*  s_indexBuffer = nullptr;
	static VertexBuffer* s_sectorVertexBuffer = nullptr;
	static VertexBuffer* s_debugVertexBuffer = nullptr;
	static SectorVertex* s_sectorVertexData = nullptr;
	static DebugVertex* s_debugVertexData = nullptr;
	static RenderTargetHandle s_renderTarget = nullptr;
	static Shader s_shader[2];
	static ShaderVariables s_shaderVar = {};
	static CameraGpu* s_camera = nullptr;
	static u32 s_width = 0, s_height = 0;
	static u32 s_sectorQuadCount = 0;
	static u32 s_debugRectCount = 0;
	static SectorInfo s_sectorInfo;

	static DrawCommand s_cmd[CMD_MAX];
	static u32 s_cmdCount = 0;

	bool init()
	{
		if (s_cmdBufferInit) { return true; }

		// Create the static index buffer, which can support up to 16k quads in a single draw call.
		u16* indexData = new u16[MAX_QUADS_PER_DRAW * 6];
		u32 vertexOffset = 0;
		u16* output = indexData;
		for (u32 i = 0; i < MAX_QUADS_PER_DRAW; i++, vertexOffset += 4, output += 6)
		{
			output[0] = vertexOffset;
			output[1] = vertexOffset + 1;
			output[2] = vertexOffset + 2;

			output[3] = vertexOffset;
			output[4] = vertexOffset + 2;
			output[5] = vertexOffset + 3;
		}

		s_indexBuffer = new IndexBuffer();
		s_indexBuffer->create(MAX_QUADS_PER_DRAW * 6, sizeof(u16), false, indexData);
		delete[] indexData;

		// Sector vertex buffer.
		s_sectorVertexBuffer = new VertexBuffer();
		s_sectorVertexBuffer->create(MAX_SECTOR_QUADS * 4, sizeof(SectorVertex), TFE_ARRAYSIZE(c_sectorAttribMap), c_sectorAttribMap, true);
		s_sectorVertexData = new SectorVertex[MAX_SECTOR_QUADS * 4];

		// Debug vertex buffer.
		s_debugVertexBuffer = new VertexBuffer();
		s_debugVertexBuffer->create(MAX_DEBUG_RECTS * 4, sizeof(DebugVertex), TFE_ARRAYSIZE(c_debugAttribMap), c_debugAttribMap, true);
		s_debugVertexData = new DebugVertex[MAX_DEBUG_RECTS * 4];

		// Shaders
		s_shader[0].load("Shaders/jedi_Wall.vert", "Shaders/jedi_Wall.frag");
		s_shaderVar.cameraPos  = s_shader[0].getVariableId("CameraPos");
		s_shaderVar.cameraView = s_shader[0].getVariableId("CameraView");
		s_shaderVar.cameraProj = s_shader[0].getVariableId("CameraProj");
		s_shader[1].load("Shaders/jedi_DebugRect.vert", "Shaders/jedi_DebugRect.frag");

		s_cmdBufferInit = true;
		return true;
	}

	void destroy()
	{
		delete s_indexBuffer;
		delete s_sectorVertexBuffer;
		delete s_debugVertexBuffer;
		delete[] s_sectorVertexData;
		delete[] s_debugVertexData;

		s_indexBuffer = nullptr;
		s_sectorVertexBuffer = nullptr;
		s_debugVertexBuffer  = nullptr;
		s_sectorVertexData   = nullptr;
		s_debugVertexData    = nullptr;
	}

	void startFrame(CameraGpu* camera)
	{
		// Does the render target size need to change?
		// TODO: If the render target is created, then the virtual framebuffer texture does not need to be.
		u32 width, height;
		vfb_getResolution(&width, &height);
		if (width != s_width || height != s_height)
		{
			s_width = width;
			s_height = height;
			TFE_RenderBackend::freeRenderTarget(s_renderTarget);
			s_renderTarget = TFE_RenderBackend::createRenderTarget(s_width, s_height, true);
		}
		TFE_RenderBackend::setOutputRenderTarget(s_renderTarget);

		s_sectorQuadCount = 0;
		s_debugRectCount = 0;
		s_cmdCount = 0;
		s_camera = camera;
	}

	void endFrame()
	{
		s_sectorVertexBuffer->update(s_sectorVertexData, s_sectorQuadCount * 4 * sizeof(SectorVertex));

		// Color shouldn't be cleared in general, but it is right now for testing.
		// TODO: Switch to clearDepth only.
		const f32 clearColor[] = { 0.3f, 0.0f, 0.3f, 1.0f };
		TFE_RenderBackend::bindRenderTarget(s_renderTarget);
		TFE_RenderBackend::clearRenderTarget(s_renderTarget, clearColor, 1.0f);
		TFE_RenderState::setStateEnable(true, STATE_DEPTH_TEST | STATE_DEPTH_WRITE | STATE_CULLING | STATE_SCISSOR);

		s_shader[0].bind();
		s_shader[0].setVariable(s_shaderVar.cameraPos,  SVT_VEC3,   s_camera->pos.m);
		s_shader[0].setVariable(s_shaderVar.cameraView, SVT_MAT3x3, s_camera->viewMtx);
		s_shader[0].setVariable(s_shaderVar.cameraProj, SVT_MAT4x4, s_camera->projMtx);

		s_indexBuffer->bind();
		s_sectorVertexBuffer->bind();
		for (u32 i = 0; i < s_cmdCount; i++)
		{
			DrawCommand* cmd = &s_cmd[i];
			if (cmd->cmd == CMD_SET_CLIP_RECT)
			{
				TFE_RenderBackend::setViewport(cmd->clipRect[0], cmd->clipRect[1], cmd->clipRect[2], cmd->clipRect[3]);
			}
			else if (cmd->cmd == CMD_DRAW_WALLS)
			{
				TFE_RenderBackend::drawIndexedTriangles(cmd->quadCount * 2, sizeof(u16), cmd->quadStart * 6);
			}
		}

		TFE_RenderState::setStateEnable(false, STATE_DEPTH_TEST | STATE_DEPTH_WRITE | STATE_CULLING | STATE_SCISSOR);
		if (s_debugRectCount)
		{
			s_debugVertexBuffer->update(s_debugVertexData, s_debugRectCount * 4 * sizeof(DebugVertex));

			s_shader[1].bind();
			s_debugVertexBuffer->bind();
			TFE_RenderBackend::drawIndexedTriangles(s_debugRectCount * 2, sizeof(u16));

			s_debugVertexBuffer->unbind();
		}
		else
		{
			s_sectorVertexBuffer->unbind();
		}
		s_indexBuffer->unbind();

		TFE_RenderBackend::unbindRenderTarget();
	}
		
	void setSectorInfo(const SectorInfo& sectorInfo)
	{
		s_sectorInfo = sectorInfo;
	}
		
	void setClipRegion(s32 x0, s32 y0, s32 x1, s32 y1)
	{
		u32 index = s_cmdCount;
		s_cmdCount++;

		s_cmd[index].cmd = CMD_SET_CLIP_RECT;
		s_cmd[index].clipRect[0] = x0;
		s_cmd[index].clipRect[1] = y0;
		s_cmd[index].clipRect[2] = x1;
		s_cmd[index].clipRect[3] = y1;
	}

	u32 getWallQuadCount()
	{
		return s_sectorQuadCount;
	}

	void drawWalls(u32 quadStart, u32 quadCount)
	{
		DrawCommand* cmd = &s_cmd[s_cmdCount];
		s_cmdCount++;

		cmd->cmd = CMD_DRAW_WALLS;
		cmd->quadStart = quadStart;
		cmd->quadCount = quadCount;
	}

	void addSolidWall(const WallInfo& wallInfo)
	{
		if (s_sectorQuadCount >= MAX_SECTOR_QUADS) { return; }

		SectorVertex* quadVtx = &s_sectorVertexData[s_sectorQuadCount * 4];
		s_sectorQuadCount++;

		quadVtx[0].pos = { wallInfo.v0.x, s_sectorInfo.heights[1], wallInfo.v0.z };
		quadVtx[0].uv  = { wallInfo.uv0.x, wallInfo.uv1.z };
		quadVtx[0].color = max(wallInfo.lightLevel, 0);

		quadVtx[1].pos = { wallInfo.v1.x, s_sectorInfo.heights[1], wallInfo.v1.z };
		quadVtx[1].uv  = { wallInfo.uv1.x, wallInfo.uv1.z };
		quadVtx[1].color = max(wallInfo.lightLevel, 0);

		quadVtx[2].pos = { wallInfo.v1.x, s_sectorInfo.heights[0], wallInfo.v1.z };
		quadVtx[2].uv  = { wallInfo.uv1.x, wallInfo.uv0.z };
		quadVtx[2].color = max(wallInfo.lightLevel, 0);

		quadVtx[3].pos = { wallInfo.v0.x, s_sectorInfo.heights[0], wallInfo.v0.z };
		quadVtx[3].uv  = { wallInfo.uv0.x, wallInfo.uv0.z };
		quadVtx[3].color = max(wallInfo.lightLevel, 0);
	}

	void addWallPart(const WallInfo& wallInfo, f32 bot, f32 top)
	{
		if (s_sectorQuadCount >= MAX_SECTOR_QUADS) { return; }

		SectorVertex* quadVtx = &s_sectorVertexData[s_sectorQuadCount * 4];
		s_sectorQuadCount++;

		quadVtx[0].pos = { wallInfo.v0.x, top, wallInfo.v0.z };
		quadVtx[0].uv  = { wallInfo.uv0.x, wallInfo.uv1.z };
		quadVtx[0].color = max(wallInfo.lightLevel, 0);

		quadVtx[1].pos = { wallInfo.v1.x, top, wallInfo.v1.z };
		quadVtx[1].uv  = { wallInfo.uv1.x, wallInfo.uv1.z };
		quadVtx[1].color = max(wallInfo.lightLevel, 0);

		quadVtx[2].pos = { wallInfo.v1.x, bot, wallInfo.v1.z };
		quadVtx[2].uv  = { wallInfo.uv1.x, wallInfo.uv0.z };
		quadVtx[2].color = max(wallInfo.lightLevel, 0);

		quadVtx[3].pos = { wallInfo.v0.x, bot, wallInfo.v0.z };
		quadVtx[3].uv  = { wallInfo.uv0.x, wallInfo.uv0.z };
		quadVtx[3].color = max(wallInfo.lightLevel, 0);
	}

	void addDebugRect(Vec2f* v0, Vec2f* v1)
	{
		DebugVertex* rectVtx = &s_debugVertexData[s_debugRectCount * 4];
		s_debugRectCount++;

		rectVtx[0] = { { v0->x, v0->z }, { 0.0f, 0.0f } };
		rectVtx[1] = { { v1->x, v0->z }, { 1.0f, 0.0f } };
		rectVtx[2] = { { v1->x, v1->z }, { 1.0f, 1.0f } };
		rectVtx[3] = { { v0->x, v1->z }, { 0.0f, 1.0f } };
	}
}  // TFE_CommandBuffer