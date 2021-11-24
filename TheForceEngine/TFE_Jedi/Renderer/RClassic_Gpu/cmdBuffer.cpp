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
	};

	// 24 bytes.
	const AttributeMapping c_sectorAttribMap[] =
	{
		{ ATTR_POS,   ATYPE_FLOAT, 3, 0, false },
		{ ATTR_UV,    ATYPE_FLOAT, 2, 0, false },
		{ ATTR_COLOR, ATYPE_UINT8, 4, 0, false },
	};

	struct SectorVertex
	{
		Vec3f pos;
		Vec2f uv;
		u32 color;
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
	static SectorVertex* s_sectorVertexData = nullptr;
	static RenderTargetHandle s_renderTarget = nullptr;
	static Shader s_shader;
	static ShaderVariables s_shaderVar = {};
	static CameraGpu* s_camera = nullptr;
	static u32 s_width = 0, s_height = 0;
	static u32 s_sectorQuadCount = 0;
	static SectorInfo s_sectorInfo;

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
		s_sectorVertexData = new SectorVertex[MAX_SECTOR_QUADS * 4 * sizeof(SectorVertex)];

		// Shaders
		s_shader.load("Shaders/jedi_Wall.vert", "Shaders/jedi_Wall.frag");
		s_shaderVar.cameraPos  = s_shader.getVariableId("CameraPos");
		s_shaderVar.cameraView = s_shader.getVariableId("CameraView");
		s_shaderVar.cameraProj = s_shader.getVariableId("CameraProj");
		s_cmdBufferInit = true;

		return true;
	}

	void destroy()
	{
		delete s_indexBuffer;
		delete s_sectorVertexBuffer;
		delete[] s_sectorVertexData;

		s_indexBuffer = nullptr;
		s_sectorVertexBuffer = nullptr;
		s_sectorVertexData   = nullptr;
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
		TFE_RenderState::setStateEnable(true, STATE_DEPTH_TEST | STATE_DEPTH_WRITE | STATE_CULLING);

		s_shader.bind();
		s_shader.setVariable(s_shaderVar.cameraPos,  SVT_VEC3,   s_camera->pos.m);
		s_shader.setVariable(s_shaderVar.cameraView, SVT_MAT3x3, s_camera->viewMtx);
		s_shader.setVariable(s_shaderVar.cameraProj, SVT_MAT4x4, s_camera->projMtx);

		s_indexBuffer->bind();
		s_sectorVertexBuffer->bind();
		TFE_RenderBackend::drawIndexedTriangles(s_sectorQuadCount * 2, sizeof(u16));

		TFE_RenderState::setStateEnable(false, STATE_DEPTH_TEST | STATE_DEPTH_WRITE | STATE_CULLING);
		s_indexBuffer->unbind();
		s_sectorVertexBuffer->unbind();
		TFE_RenderBackend::unbindRenderTarget();
	}
		
	void setSectorInfo(const SectorInfo& sectorInfo)
	{
		s_sectorInfo = sectorInfo;
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
		quadVtx[0].uv = { wallInfo.uv0.x, wallInfo.uv1.z };
		quadVtx[0].color = max(wallInfo.lightLevel, 0);

		quadVtx[1].pos = { wallInfo.v1.x, top, wallInfo.v1.z };
		quadVtx[1].uv = { wallInfo.uv1.x, wallInfo.uv1.z };
		quadVtx[1].color = max(wallInfo.lightLevel, 0);

		quadVtx[2].pos = { wallInfo.v1.x, bot, wallInfo.v1.z };
		quadVtx[2].uv = { wallInfo.uv1.x, wallInfo.uv0.z };
		quadVtx[2].color = max(wallInfo.lightLevel, 0);

		quadVtx[3].pos = { wallInfo.v0.x, bot, wallInfo.v0.z };
		quadVtx[3].uv = { wallInfo.uv0.x, wallInfo.uv0.z };
		quadVtx[3].color = max(wallInfo.lightLevel, 0);
	}
}  // TFE_CommandBuffer