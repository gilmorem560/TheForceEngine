#include <DXL2_Renderer/DXL2_SoftRenderCPU/renderer_softCPU.h>
#include <DXL2_RenderBackend/renderBackend.h>

#include <DXL2_Asset/levelAsset.h>
#include <DXL2_Asset/paletteAsset.h>
#include <DXL2_Asset/colormapAsset.h>
#include <DXL2_Asset/spriteAsset.h>
#include <DXL2_Asset/textureAsset.h>
#include <DXL2_Asset/fontAsset.h>
#include <DXL2_Asset/modelAsset.h>

#include <DXL2_System/system.h>
#include <assert.h>
#include <algorithm>

#define ENABLE_FIXED_POINT_COLUMN_RENDERING 1

// TODO: Using an 8-bit texture format + palette conversion on blit might be faster for most
// GPUs since 1/4 of that data will need to be uploaded (theoretically).
f64 m_time = 0.0;

DXL2_SoftRenderCPU::DXL2_SoftRenderCPU()
{
	m_frame = 0;
	m_curPal = *DXL2_Palette::getDefault256();
	m_curColorMap = nullptr;
	m_clearScreen = true;
}

bool DXL2_SoftRenderCPU::init()
{
	m_width = 320;
	m_height = 200;

	if (!DXL2_RenderBackend::createVirtualDisplay(m_width, m_height, DMODE_4x3))
	{
		return false;
	}
	m_display = new u8[m_width * m_height];
	m_display32 = new u32[m_width * m_height];

	m_rcpHalfWidth = 1.0f / (f32(m_width)*0.5f);
	
	return true;
}

void DXL2_SoftRenderCPU::destroy()
{
	delete[] m_display;
	delete[] m_display32;
}

bool DXL2_SoftRenderCPU::changeResolution(u32 width, u32 height)
{
	if (width == m_width && height == m_height) { return true; }

	delete[] m_display;
	delete[] m_display32;

	m_width = width;
	m_height = height;

	if (!DXL2_RenderBackend::createVirtualDisplay(m_width, m_height, DMODE_4x3))
	{
		return false;
	}
	m_display = new u8[m_width * m_height];
	m_display32 = new u32[m_width * m_height];
	m_rcpHalfWidth = 1.0f / (f32(m_width)*0.5f);
	return true;
}

void DXL2_SoftRenderCPU::getResolution(u32* width, u32* height)
{
	*width = m_width;
	*height = m_height;
}

void DXL2_SoftRenderCPU::enableScreenClear(bool enable)
{
	m_clearScreen = enable;
}

void DXL2_SoftRenderCPU::begin()
{
	if (m_clearScreen)
	{
		const u32 pixelCount = m_width * m_height;
		memset(m_display, 0, pixelCount);
	}

	m_time += DXL2_System::getDeltaTime();
}

void DXL2_SoftRenderCPU::end()
{
	// Convert from 8 bit to 32 bit.
	const u32 pixelCount = m_width * m_height;
	const u32* pal = m_curPal.colors;
	for (u32 p = 0; p < pixelCount; p++)
	{
		m_display32[p] = pal[m_display[p]];
	}

	DXL2_RenderBackend::updateVirtualDisplay(m_display32, m_width * m_height * 4u);
	m_frame++;
}

////////////////////////////////////////////
// Draw Commands
////////////////////////////////////////////

// A line drawn using Bresenham will skip drawing some pixels that the line
// passes through but generates more pleasing looking lines.
void DXL2_SoftRenderCPU::drawLineBresenham(f32 ax, f32 ay, f32 bx, f32 by, u8 color)
{
	s32 x0 = s32(ax), y0 = s32(ay);
	s32 x1 = s32(bx), y1 = s32(by);
	s32 dx = abs(x1 - x0);
	s32 dy = -abs(y1 - y0);
	s32 sx = x0 < x1 ? 1 : -1;
	s32 sy = y0 < y1 ? 1 : -1;
	s32 err = dx + dy;

	s32 MAX_STEPS = 600;
	for (s32 i = 0; i < MAX_STEPS; i++)
	{
		if (x0 >= 0 && x0 < (s32)m_width && y0 >= 0 && y0 < (s32)m_height) { m_display[y0*m_width + x0] = color; }
		if (x0 == x1 && y0 == y1) break;

		s32 e2 = 2 * err;
		if (e2 >= dy)
		{
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx)
		{
			err += dx;
			y0 += sy;
		}
	}
}

void DXL2_SoftRenderCPU::drawLine(const Vec2f* v0, const Vec2f* v1, u8 color)
{
	Vec2f p0 = *v0, p1 = *v1;

	// Clip to viewport.
	if ((p0.x < 0.0f && p1.x > 0.0f) || (p1.x < 0.0f && p0.x > 0.0f))
	{
		f32 s = -p0.x / (p1.x - p0.x);
		Vec2f newVertex = { p0.x + s * (p1.x - p0.x), p0.z + s * (p1.z - p0.z) };
		if (p0.x < p1.x) { p0 = newVertex; }
		else { p1 = newVertex; }
	}
	if ((p0.z < 0.0f && p1.z > 0.0f) || (p1.z < 0.0f && p0.z > 0.0f))
	{
		f32 s = -p0.z / (p1.z - p0.z);
		Vec2f newVertex = { p0.x + s * (p1.x - p0.x), p0.z + s * (p1.z - p0.z) };
		if (p0.z < p1.z) { p0 = newVertex; }
		else { p1 = newVertex; }
	}
	if ((p0.x > f32(m_width - 1) && p1.x < f32(m_width - 1)) || (p1.x > f32(m_width - 1) && p0.x < f32(m_width - 1)))
	{
		f32 s = (f32(m_width) - p0.x) / (p1.x - p0.x);
		Vec2f newVertex = { p0.x + s * (p1.x - p0.x), p0.z + s * (p1.z - p0.z) };
		if (p0.x > p1.x) { p0 = newVertex; }
		else { p1 = newVertex; }
	}
	if ((p0.z > f32(m_height - 1) && p1.z < f32(m_height - 1)) || (p1.z > f32(m_height - 1) && p0.z < f32(m_height - 1)))
	{
		f32 s = (f32(m_height) - p0.z) / (p1.z - p0.z);
		Vec2f newVertex = { p0.x + s * (p1.x - p0.x), p0.z + s * (p1.z - p0.z) };
		if (p0.z > p1.z) { p0 = newVertex; }
		else { p1 = newVertex; }
	}

	drawLineBresenham(p0.x, p0.z, p1.x, p1.z, color);
}

bool DXL2_SoftRenderCPU::clip(s32& x0, s32& y0, s32& w, s32& h)
{
	if (x0 < 0)
	{
		s32 dx = -x0;
		x0 += dx;
		w -= dx;
	}
	if (y0 < 0)
	{
		s32 dy = -y0;
		y0 += dy;
		h -= dy;
	}
	if (x0 + w >= (s32)m_width)
	{
		w -= (x0 + w - (s32)m_width);
	}
	if (y0 + h >= (s32)m_height)
	{
		h -= (y0 + h - (s32)m_height);
	}
	return w > 0 && h > 0;
}

#if ENABLE_FIXED_POINT_COLUMN_RENDERING
void DXL2_SoftRenderCPU::drawSpriteColumn(s32 x0, s32 y0, s32 y1, f32 v0, f32 v1, s8 lightLevel, const u8* image, s32 texHeight)
{
	if (y0 > y1) { return; }

	s32 h = y1 - y0 + 1;
	u8* outBuffer = &m_display[y0 * m_width + x0];

	// Convert from float to 16.16 fixed point to avoid a float to int conversion each pixel.
	s32 fv0 = s32(v0 * 65536.0f);
	s32 fv1 = s32(v1 * 65536.0f);
	s32 dv = (fv1 - fv0) / h;
	s32 v = fv0;

	const u32 colorMapOffset = u32(lightLevel) << 8u;
	for (s32 y = 0; y < h; y++, outBuffer += m_width, v += dv)
	{
		s32 V = (v >> 16);
		if (V < 0 || V >= texHeight) { continue; }

		const u8 color = image[V];
		if (color == 0) { continue; }

		*outBuffer = m_curColorMap->colorMap[colorMapOffset + color];
	}
}

void DXL2_SoftRenderCPU::drawTexturedColumn(s32 x0, s32 y0, s32 y1, f32 v0, f32 v1, s8 lightLevel, const u8* image, s32 texHeight)
{
	if (y0 > y1) { return; }

	s32 h = y1 - y0 + 1;
	u8* outBuffer = &m_display[y0 * m_width + x0];

	// Convert from float to 16.16 fixed point to avoid a float to int conversion each pixel.
	s32 fv0 = s32(v0 * 65536.0f);
	s32 fv1 = s32(v1 * 65536.0f);
	s32 dv = (fv1 - fv0) / h;
	s32 v = fv0;
	
	u32 colorMapOffset = u32(lightLevel) << 8u;
	for (s32 y = 0; y < h; y++, outBuffer += m_width, v += dv)
	{
		const s32 V = (v>>16)&(texHeight - 1);
		*outBuffer = m_curColorMap->colorMap[colorMapOffset + image[V]];
	}
}

void DXL2_SoftRenderCPU::drawMaskTexturedColumn(s32 x0, s32 y0, s32 y1, f32 v0, f32 v1, s8 lightLevel, const u8* image, s32 texHeight)
{
	if (y0 > y1) { return; }

	s32 h = y1 - y0 + 1;
	u8* outBuffer = &m_display[y0 * m_width + x0];

	// Convert from float to 16.16 fixed point to avoid a float to int conversion each pixel.
	s32 fv0 = s32(v0 * 65536.0f);
	s32 fv1 = s32(v1 * 65536.0f);
	s32 dv = (fv1 - fv0) / h;
	s32 v = fv0;

	u32 colorMapOffset = u32(lightLevel) << 8u;
	for (s32 y = 0; y < h; y++, outBuffer += m_width, v += dv)
	{
		const s32 V = (v >> 16)&(texHeight - 1);
		const u8 texel = image[V];
		if (texel == 0) { continue; }

		*outBuffer = m_curColorMap->colorMap[colorMapOffset + texel];
	}
}

void DXL2_SoftRenderCPU::drawClampedTexturedColumn(s32 x0, s32 y0, s32 y1, f32 v0, f32 v1, s8 lightLevel, const u8* image, s32 texHeight, s32 offset, bool skipTexels)
{
	if (y0 > y1) { return; }

	const s32 h = y1 - y0 + 1;
	u8* outBuffer = &m_display[y0 * m_width + x0];

	// Convert from float to 16.16 fixed point to avoid a float to int conversion each pixel.
	const s32 fv0 = s32(v0 * 65536.0f);
	const s32 fv1 = s32(v1 * 65536.0f);
	const s32 dv = (fv1 - fv0) / h;
	s32 v = fv0;

	const u32 colorMapOffset = u32(lightLevel) << 8u;
	for (s32 y = 0; y < h; y++, outBuffer += m_width, v += dv)
	{
		s32 V = (v >> 16);
		V -= offset;
		if (V >= -offset && V < texHeight - offset)
		{
			V = V & (texHeight - 1);
			// This hackery seems to be required for many signs in Dark Forces.
			// TODO: Figure out if there is a better way of detecting this or maybe a bug in the texture reader.
			if (skipTexels && V > texHeight - 3)
			{
				if (image[V - texHeight] == 0) { continue; }
				V = texHeight - 4;
			}

			// Support masking.
			const u8 baseClr = image[V];
			if (baseClr == 0) { continue; }
			*outBuffer = m_curColorMap->colorMap[colorMapOffset + baseClr];
		}
	}
}

#define SKY_HALF_HEIGHT 100

void DXL2_SoftRenderCPU::drawSkyColumn(s32 x0, s32 y0, s32 y1, s32 halfScrHeight, s32 offsetY, const u8* image, s32 texHeight)
{
	if (y0 > y1) { return; }

	const s32 h = y1 - y0 + 1;
	u8* outBuffer = &m_display[y0 * m_width + x0];

	// 12 bits of fractional precision, allows for greater than 4k resolution.
	const s32 v0 = y0 * SKY_HALF_HEIGHT * 4096 / halfScrHeight + offsetY * 4096;
	const s32 v1 = (y0 + h) * SKY_HALF_HEIGHT * 4096 / halfScrHeight + offsetY * 4096;
	const s32 dv = (v1 - v0) / h;

	s32 v = v0;
	for (s32 y = 0; y < h; y++, outBuffer += m_width, v += dv)
	{
		const s32 V = (v >> 12) & (texHeight - 1);
		*outBuffer = image[texHeight - V - 1];
	}
}

// TODO: Convert to fixed point
// TODO: Figure out a better way of flipping the U coordinate.
void DXL2_SoftRenderCPU::drawSpan(s32 x0, s32 x1, s32 y, f32 z, f32 r0, f32 r1, f32 xOffset, f32 zOffset, s8 lightLevel, s32 texWidth, s32 texHeight, const u8* image)
{
	const s32 h = x1 - x0 + 1;

	const f32 fx0 = (f32(x0) * m_rcpHalfWidth - 1.0f) * z;
	const f32 fx1 = ((f32(x0) + h) * m_rcpHalfWidth - 1.0f) * z;
	const f32 dx = (fx1 - fx0) / f32(h);

	u8* outBuffer = &m_display[y * m_width + x0];

	f32 x = fx0;
	const u32 colorMapOffset = u32(lightLevel) << 8u;
	for (s32 i = 0; i < h; i++, x += dx, outBuffer++)
	{
		const f32 xWorld = x * r0 - z * r1 + xOffset;	// This should probably be in fixed point to avoid float -> int conversions.
		const f32 zWorld = x * r1 + z * r0 + zOffset;

		s32 U = texWidth > 1 ? s32(xWorld) & (texWidth - 1) : 0;
		U = texWidth - U - 1;
		const s32 V = texHeight > 1 ? s32(zWorld) & (texHeight - 1) : 0;
		*outBuffer = m_curColorMap->colorMap[colorMapOffset + image[U*texHeight + V]];
	}
}

static s32 m_XClip[2];
static const s16* m_YClipUpper;
static const s16* m_YClipLower;

// TODO: Convert to fixed point
// TODO: Figure out a better way of flipping the U coordinate.
void DXL2_SoftRenderCPU::drawSpanClip(s32 x0, s32 x1, s32 y, f32 z, f32 r0, f32 r1, f32 xOffset, f32 zOffset, s8 lightLevel, s32 texWidth, s32 texHeight, const u8* image)
{
	// Now adjust for clipping.
	x0 = std::max(x0, m_XClip[0]);
	x1 = std::min(x1, m_XClip[1]);
	if (x0 > x1) { return; }

	const s32 h = x1 - x0 + 1;
	const f32 fx0 = (f32(x0) * m_rcpHalfWidth - 1.0f) * z;
	const f32 fx1 = ((f32(x0) + h) * m_rcpHalfWidth - 1.0f) * z;
	const f32 dx = (fx1 - fx0) / f32(h);

	u8* outBuffer = &m_display[y * m_width + x0];

	f32 x = fx0;
	const u32 colorMapOffset = u32(lightLevel) << 8u;

	if (m_YClipUpper)
	{
		const s16* upperMask = &m_YClipUpper[x0 - m_XClip[0]];
		const s16* lowerMask = &m_YClipLower[x0 - m_XClip[0]];
		for (s32 i = 0; i < h; i++, x += dx, outBuffer++, upperMask++, lowerMask++)
		{
			if (upperMask && (y < *upperMask || y > *lowerMask)) { continue; }
			const f32 xWorld = x * r0 - z * r1 + xOffset;	// This should probably be in fixed point to avoid float -> int conversions.
			const f32 zWorld = x * r1 + z * r0 + zOffset;

			s32 U = texWidth > 1 ? s32(xWorld) & (texWidth - 1) : 0;
			U = texWidth - U - 1;
			const s32 V = texHeight > 1 ? s32(zWorld) & (texHeight - 1) : 0;

			const u8 color = image[U*texHeight + V];
			if (color == 0) { continue; }
			*outBuffer = m_curColorMap->colorMap[colorMapOffset + color];
		}
	}
	else
	{
		for (s32 i = 0; i < h; i++, x += dx, outBuffer++)
		{
			const f32 xWorld = x * r0 - z * r1 + xOffset;	// This should probably be in fixed point to avoid float -> int conversions.
			const f32 zWorld = x * r1 + z * r0 + zOffset;

			s32 U = texWidth > 1 ? s32(xWorld) & (texWidth - 1) : 0;
			U = texWidth - U - 1;
			const s32 V = texHeight > 1 ? s32(zWorld) & (texHeight - 1) : 0;

			const u8 color = image[U*texHeight + V];
			if (color == 0) { continue; }
			*outBuffer = m_curColorMap->colorMap[colorMapOffset + color];
		}
	}
}

void DXL2_SoftRenderCPU::setHLineClip(s32 x0, s32 x1, const s16* upperYMask, const s16* lowerYMask)
{
	m_XClip[0] = x0;
	m_XClip[1] = x1;
	m_YClipUpper = upperYMask;
	m_YClipLower = lowerYMask;
}

void DXL2_SoftRenderCPU::drawHLine(s32 x0, s32 x1, s32 y, u8 color, u8 lightLevel)
{
	x0 = std::max(x0, m_XClip[0]);
	x1 = std::min(x1, m_XClip[1]);
	if (x0 > x1 || y < 0) { return; }
	color = m_curColorMap->colorMap[(u32(lightLevel) << 8u) + color];
		
	u8* outBuffer = &m_display[y * m_width + x0];
	const s32 count = x1 - x0 + 1;
	if (m_YClipUpper)
	{
		const s16* upperMask = &m_YClipUpper[x0 - m_XClip[0]];
		const s16* lowerMask = &m_YClipLower[x0 - m_XClip[0]];
		for (s32 x = 0; x < count; x++, outBuffer++, upperMask++, lowerMask++)
		{
			if (upperMask && (y < *upperMask || y > *lowerMask)) { continue; }
			*outBuffer = color;
		}
	}
	else
	{
		for (s32 x = 0; x < count; x++, outBuffer++)
		{
			*outBuffer = color;
		}
	}
}

void DXL2_SoftRenderCPU::drawHLineGouraud(s32 x0, s32 x1, s32 l0, s32 l1, s32 y, u8 color)
{
	s32 count = x1 - x0 + 1;
	s32 l = l0;
	s32 dl = (l1 - l0) / count;

	// Now adjust for clipping.
	if (m_XClip[0] > x0)
	{
		l += dl * (m_XClip[0] - x0 + 1);
		x0 = m_XClip[0];
	}
	x1 = std::min(x1, m_XClip[1]);
	if (x0 > x1) { return; }

	const u8* colorMap = m_curColorMap->colorMap;
	u8* outBuffer = &m_display[y * m_width + x0];
	count = x1 - x0 + 1;
	if (m_YClipUpper)
	{
		const s16* upperMask = &m_YClipUpper[x0 - m_XClip[0]];
		const s16* lowerMask = &m_YClipLower[x0 - m_XClip[0]];
		for (s32 x = 0; x < count; x++, l += dl, outBuffer++, upperMask++, lowerMask++)
		{
			if (upperMask && (y < *upperMask || y > *lowerMask)) { continue; }

			const s32 L = l >> 16;
			assert(L <= 31);
			*outBuffer = colorMap[L*256 + color];
		}
	}
	else
	{
		for (s32 x = 0; x < count; x++, l += dl, outBuffer++)
		{
			const s32 L = l >> 16;
			assert(L <= 31);
			*outBuffer = colorMap[L*256 + color];
		}
	}
}

void DXL2_SoftRenderCPU::drawHLineTextured(s32 x0, s32 x1, s32 y, s32 u0, s32 v0, s32 u1, s32 v1, s32 texWidth, s32 texHeight, const u8* image, u8 lightLevel)
{
	s32 count = x1 - x0 + 1;
	s32 u = u0, v = v0;
	s32 du = (u1 - u0) / count;
	s32 dv = (v1 - v0) / count;

	// Now adjust for clipping.
	if (m_XClip[0] > x0)
	{
		u += du * (m_XClip[0] - x0 + 1);
		v += dv * (m_XClip[0] - x0 + 1);
		x0 = m_XClip[0];
	}
	x1 = std::min(x1, m_XClip[1]);
	count = x1 - x0 + 1;

	const u32 colorMapOffset = u32(lightLevel) << 8u;
	u8* outBuffer = &m_display[y * m_width + x0];
	if (m_YClipUpper)
	{
		const s16* upperMask = &m_YClipUpper[x0 - m_XClip[0]];
		const s16* lowerMask = &m_YClipLower[x0 - m_XClip[0]];
		for (s32 x = 0; x < count; x++, outBuffer++, u += du, v += dv, upperMask++, lowerMask++)
		{
			if ((y < *upperMask || y > *lowerMask)) { continue; }

			const s32 U = (u >> 20)&(texWidth - 1);
			const s32 V = (v >> 20)&(texHeight - 1);
			*outBuffer = m_curColorMap->colorMap[colorMapOffset + image[U*texHeight + V]];
		}
	}
	else
	{
		for (s32 x = 0; x < count; x++, outBuffer++, u += du, v += dv)
		{
			const s32 U = (u >> 20)&(texWidth - 1);
			const s32 V = (v >> 20)&(texHeight - 1);
			*outBuffer = m_curColorMap->colorMap[colorMapOffset + image[U*texHeight + V]];
		}
	}
}

void DXL2_SoftRenderCPU::drawHLineTexturedPC(s32 x0, s32 x1, s32 y, s32 u0, s32 v0, s32 u1, s32 v1, s32 z0, s32 z1, s32 texWidth, s32 texHeight, const u8* image, u8 lightLevel)
{
	s32 count = x1 - x0 + 1;
	s32 u = u0, v = v0, z = z0;
	s32 du = (u1 - u0) / count;
	s32 dv = (v1 - v0) / count;
	s32 dz = (z1 - z0) / count;

	// Now adjust for clipping.
	if (m_XClip[0] > x0)
	{
		u += du * (m_XClip[0] - x0 + 1);
		v += dv * (m_XClip[0] - x0 + 1);
		z += dz * (m_XClip[0] - x0 + 1);
		x0 = m_XClip[0];
	}
	x1 = std::min(x1, m_XClip[1]);
	count = x1 - x0 + 1;

	const u32 colorMapOffset = u32(lightLevel) << 8u;
	u8* outBuffer = &m_display[y * m_width + x0];
	if (m_YClipUpper)
	{
		const s16* upperMask = &m_YClipUpper[x0 - m_XClip[0]];
		const s16* lowerMask = &m_YClipLower[x0 - m_XClip[0]];
		for (s32 x = 0; x < count; x++, outBuffer++, u += du, v += dv, z += dz, upperMask++, lowerMask++)
		{
			if ((y < *upperMask || y > *lowerMask)) { continue; }

			const s32 U = (u / z)&(texWidth - 1);
			const s32 V = (v / z)&(texHeight - 1);
			*outBuffer = m_curColorMap->colorMap[colorMapOffset + image[U*texHeight + V]];
		}
	}
	else
	{
		for (s32 x = 0; x < count; x++, outBuffer++, u += du, v += dv, z += dz)
		{
			const s32 U = (u / z)&(texWidth - 1);
			const s32 V = (v / z)&(texHeight - 1);
			*outBuffer = m_curColorMap->colorMap[colorMapOffset + image[U*texHeight + V]];
		}
	}
}

void DXL2_SoftRenderCPU::drawHLineTexturedGouraud(s32 x0, s32 x1, s32 y, s32 u0, s32 v0, s32 u1, s32 v1, s32 l0, s32 l1, s32 texWidth, s32 texHeight, const u8* image)
{
	s32 count = x1 - x0 + 1;
	s32 u = u0, v = v0, l = l0;
	s32 du = (u1 - u0) / count;
	s32 dv = (v1 - v0) / count;
	s32 dl = (l1 - l0) / count;

	// Now adjust for clipping.
	if (m_XClip[0] > x0)
	{
		u += du * (m_XClip[0] - x0 + 1);
		v += dv * (m_XClip[0] - x0 + 1);
		l += dl * (m_XClip[0] - x0 + 1);
		x0 = m_XClip[0];
	}
	x1 = std::min(x1, m_XClip[1]);
	count = x1 - x0 + 1;

	const u8* colorMap = m_curColorMap->colorMap;
	u8* outBuffer = &m_display[y * m_width + x0];
	if (m_YClipUpper)
	{
		const s16* upperMask = &m_YClipUpper[x0 - m_XClip[0]];
		const s16* lowerMask = &m_YClipLower[x0 - m_XClip[0]];
		for (s32 x = 0; x < count; x++, outBuffer++, u += du, v += dv, l += dl, upperMask++, lowerMask++)
		{
			if ((y < *upperMask || y > *lowerMask)) { continue; }

			const s32 U = (u >> 20)&(texWidth - 1);
			const s32 V = (v >> 20)&(texHeight - 1);
			const s32 L = (l >> 16);
			*outBuffer = colorMap[L*256 + image[U*texHeight + V]];
		}
	}
	else
	{
		for (s32 x = 0; x < count; x++, outBuffer++, u += du, v += dv, l += dl)
		{
			const s32 U = (u >> 20)&(texWidth - 1);
			const s32 V = (v >> 20)&(texHeight - 1);
			const s32 L = (l >> 16);
			*outBuffer = colorMap[L*256 + image[U*texHeight + V]];
		}
	}
}

void DXL2_SoftRenderCPU::drawHLineTexturedGouraudPC(s32 x0, s32 x1, s32 y, s32 u0, s32 v0, s32 u1, s32 v1, s32 z0, s32 z1, s32 l0, s32 l1, s32 texWidth, s32 texHeight, const u8* image)
{
	s32 count = x1 - x0 + 1;
	s32 u = u0, v = v0, z = z0, l = l0;
	s32 du = (u1 - u0) / count;
	s32 dv = (v1 - v0) / count;
	s32 dz = (z1 - z0) / count;
	s32 dl = (l1 - l0) / count;

	// Now adjust for clipping.
	if (m_XClip[0] > x0)
	{
		u += du * (m_XClip[0] - x0 + 1);
		v += dv * (m_XClip[0] - x0 + 1);
		z += dz * (m_XClip[0] - x0 + 1);
		l += dl * (m_XClip[0] - x0 + 1);
		x0 = m_XClip[0];
	}
	x1 = std::min(x1, m_XClip[1]);
	count = x1 - x0 + 1;

	const u8* colorMap = m_curColorMap->colorMap;
	u8* outBuffer = &m_display[y * m_width + x0];
	if (m_YClipUpper)
	{
		const s16* upperMask = &m_YClipUpper[x0 - m_XClip[0]];
		const s16* lowerMask = &m_YClipLower[x0 - m_XClip[0]];
		for (s32 x = 0; x < count; x++, outBuffer++, u += du, v += dv, z += dz, l += dl, upperMask++, lowerMask++)
		{
			if ((y < *upperMask || y > *lowerMask)) { continue; }

			const s32 U = (u / z)&(texWidth - 1);
			const s32 V = (v / z)&(texHeight - 1);
			const s32 L = (l >> 16);
			*outBuffer = colorMap[L*256 + image[U*texHeight + V]];
		}
}
	else
	{
		for (s32 x = 0; x < count; x++, outBuffer++, u += du, v += dv, z += dz, l += dl)
		{
			const s32 U = (u / z)&(texWidth - 1);
			const s32 V = (v / z)&(texHeight - 1);
			const s32 L = (l >> 16);
			*outBuffer = colorMap[L*256 + image[U*texHeight + V]];
		}
	}
}

#else
void DXL2_SoftRenderCPU::drawTexturedColumn(s32 x0, s32 y0, s32 y1, u8 lightLevel, f32 v0, f32 v1, const u8* image, s32 texHeight)
{
	if (y0 > y1) { return; }

	s32 h = y1 - y0 + 1;
	u8* outBuffer = &m_display[y0 * m_width + x0];

	f32 fv0 = v0;
	f32 fv1 = v1;
	f32 dv = (fv1 - fv0) / f32(h);
	f32 v = fv0;

	for (s32 y = 0; y < h; y++, outBuffer += m_width, v += dv)
	{
		const s32 V = s32(v)&(texHeight - 1);
		*outBuffer = image[V];
	}
}
#endif

void DXL2_SoftRenderCPU::drawColoredColumn(s32 x0, s32 y0, s32 y1, u8 color)
{
	s32 h = y1 - y0 + 1;
	u8* outBuffer = &m_display[y0 * m_width + x0];

	for (s32 y = 0; y < h; y++, outBuffer += m_width)
	{
		*outBuffer = color;
	}
}

void DXL2_SoftRenderCPU::drawColoredQuad(s32 x0, s32 y0, s32 w, s32 h, u8 color)
{
	if (!clip(x0, y0, w, h)) { return; }
	u8* outBuffer = &m_display[y0 * m_width + x0];

	for (s32 y = 0; y < h; y++, outBuffer += m_width)
	{
		for (s32 x = 0; x < w; x++)
		{
			outBuffer[x] = color;
		}
	}
}

void DXL2_SoftRenderCPU::setPalette(const Palette256* pal)
{
	// Copy the palette because colors can be changed on the fly.
	if (pal)
	{
		m_curPal = *pal;
	}
	else
	{
		m_curPal = *DXL2_Palette::getDefault256();
	}
}

void DXL2_SoftRenderCPU::setColorMap(const ColorMap* cmp)
{
	m_curColorMap = cmp;
}

void DXL2_SoftRenderCPU::setPaletteColor(u8 index, u32 color)
{
	m_curPal.colors[index] = color;
}

//N.8 fixed point, 256 = 1.0.
void DXL2_SoftRenderCPU::blitImage(const TextureFrame* texture, s32 x0, s32 y0, s32 scaleX, s32 scaleY, u8 lightLevel)
{
	const s32 w = texture->width;
	const s32 h = texture->height;
	const u8* image = texture->image;

	u8* outBuffer = m_display;
	s32 xStart = 0, yStart = 0;
	s32 dx = (w*scaleX)>>8, dy = (h*scaleY)>>8;
	if (y0 + dy < 0 || y0 >= (s32)m_height || x0 + dx < 0 || x0 >= (s32)m_width) { return; }

	// Clip
	if (x0 < 0) { xStart = -x0; }
	if (x0 + dx > (s32)m_width) { dx = (s32)m_width - x0; }

	if (y0 < 0) { yStart = -y0; }
	if (y0 + dy > (s32)m_height) { dy = (s32)m_height - y0; }

	// Blit.
	u32 colorOffset = lightLevel * 256;
	const u8* colorMap = m_curColorMap->colorMap;
	for (s32 x = xStart; x < dx; x++)
	{
		const s32 xImage = (x << 8) / scaleX;
		image = &texture->image[xImage*h];

		for (s32 y = yStart; y < dy; y++)
		{
			const s32 yImage = h - ((y << 8) / scaleY) - 1;
			const u8 baseColor = image[yImage];
			const u8 color = lightLevel < 31 ? colorMap[colorOffset + baseColor] : baseColor;
			if (baseColor == 0) { continue; }

			outBuffer[(y+y0)*m_width + x+x0] = color;
		}
	}
}

void DXL2_SoftRenderCPU::print(const char* text, const Font* font, s32 x0, s32 y0, s32 scaleX, s32 scaleY)
{
	if (!text || !font) { return; }

	const size_t len = strlen(text);
	s32 x = x0 << 8, yPos = y0;
	s32 dx = font->maxWidth * scaleX;

	TextureFrame frame = { 0 };
	frame.height = font->height;
	for (size_t i = 0; i < len; i++, text++)
	{
		const char c = *text;
		if (c < font->startChar || c > font->endChar) { x += dx;  continue; }

		const s32 index = s32(c) - font->startChar;
		const s32 w = font->width[index];
		const u8* image = font->imageData + font->imageOffset[index];

		s32 xPos = x >> 8;
		frame.width = w;
		frame.image = (u8*)image;
		blitImage(&frame, xPos, yPos, scaleX, scaleY);
		
		x += (w+1) * scaleX;
	}
}

void DXL2_SoftRenderCPU::drawPalette()
{
	// Draw a 16x16 color grid showing the current palette.
	// Flip 'y' so index 0 starts in the upper left corner.
	u32 xOffset = (m_width - 256) >> 1;
	u32 yOffset = m_height >= 256 ? (m_height - 256) >> 1 : 0;
	for (u32 y = 0; y < 16; y++)
	{
		u32 yCoord = (y << 4) + yOffset;
		for (u32 x = 0; x < 16; x++)
		{
			u32 xCoord = (x << 4) + xOffset;
			drawColoredQuad(xCoord, yCoord, 16, 16, ((15-y) << 4) | x);
		}
	}
}

void DXL2_SoftRenderCPU::drawTexture(Texture* texture, s32 x0, s32 y0)
{
	s32 frame = 0;
	if (texture->frameRate)
	{
		frame = s32(m_time * f64(texture->frameRate)) % texture->frameCount;
	}

	TextureFrame* tex = &texture->frames[frame];
	const s32 w = tex->width;
	const s32 h = tex->height;
	const u8* image = tex->image;

	s32 startX = 0;
	s32 startY = 0;
	if (x0 < 0) { startX -= x0; image -= h * x0; }
	if (y0 < 0) { startY -= y0; }
	
	for (s32 x = startX; x < w && (x+x0) < (s32)m_width; x++, image += h)
	{
		for (s32 y = startY; y < h && (y + y0) < (s32)m_height; y++)
		{
			m_display[(y + y0)*m_width + x + x0] = image[y];
		}
	}
}

void DXL2_SoftRenderCPU::drawFont(Font* font)
{
	s32 maxW = font->maxWidth + 1;
	s32 height = font->height;

	s32 xOffset = (m_width - maxW * 16) >> 1;
	s32 yOffset = (m_height - (height+1) * 16) >> 1;

	for (s32 c = 0; c < 256; c++)
	{
		if (c < font->startChar || c > font->endChar) { continue; }
		const s32 index = c - font->startChar;

		const s32 x0 = (c & 15) * maxW + xOffset;
		const s32 y0 = (15 - (c >> 4)) * (height + 1) + yOffset;
		const s32 w = font->width[index];
		const u8* image = font->imageData + font->imageOffset[index];

		const s32 h = height;
		for (s32 x = 0; x < w; x++, image += h)
		{
			for (s32 y = 0; y < h; y++)
			{
				m_display[(y+y0)*m_width + x+x0] = image[y];
			}
		}
	}
}

void DXL2_SoftRenderCPU::drawFrame(Frame* frame, s32 x0, s32 y0)
{
	// for now just draw it like a texture.
	const s32 w = frame->width;
	const s32 h = frame->height;
	const u8* image = frame->image;

	x0 = std::max(x0 + frame->InsertX, 0);
	y0 = std::max(-h + y0 - frame->InsertY, 0);

	s32 xOffset = x0 + (frame->Flip ? w - 1 : 0);
	s32 xSign = frame->Flip ? -1 : 1;
	for (s32 x = 0; x < w; x++, image += h)
	{
		for (s32 y = 0; y < h && y < (s32)m_height; y++)
		{
			m_display[(y+y0)*m_width + (xSign*x+xOffset)] = image[y];
		}
	}
}

void DXL2_SoftRenderCPU::drawSprite(Sprite* sprite, s32 x0, s32 y0, u32 animID, u8 angle)
{
	animID = std::min(animID, (u32)sprite->animationCount);
	SpriteAnim* anim = &sprite->anim[animID];

	angle = angle % anim->angleCount;
	u32 frame = u32(m_time * anim->frameRate) % anim->angles[angle].frameCount;
	frame = anim->angles[angle].frameIndex[frame];

	drawFrame(&sprite->frames[frame], x0, y0);
}

struct MapMarker
{
	f32 x, z;
	s32 layer;
	u8 color;
};
std::vector<MapMarker> s_markers;

void DXL2_SoftRenderCPU::clearMapMarkers()
{
	s_markers.clear();
}

void DXL2_SoftRenderCPU::addMapMarker(s32 layer, f32 x, f32 z, u8 color)
{
	MapMarker marker;
	marker.x = x;
	marker.z = z;
	marker.color = color;
	marker.layer = layer;

	s_markers.push_back(marker);
}

u32 DXL2_SoftRenderCPU::getMapMarkerCount()
{
	return (u32)s_markers.size();
}

void DXL2_SoftRenderCPU::drawMapLines(s32 layers, f32 xOffset, f32 zOffset, f32 scale)
{
	const LevelData* level = DXL2_LevelAsset::getLevelData();
	if (!level) { return; }

	// Draw each sector.
	const u32 sectorCount = (u32)level->sectors.size();
	const Sector* sectors = level->sectors.data();
	const SectorWall* walls = level->walls.data();
	const Vec2f* vertices = level->vertices.data();
	if (!sectorCount) { return; }

	for (u32 s = 0; s < sectorCount; s++)
	{
		if (layers != sectors[s].layer) { continue; }

		const Vec2f* secVtx = vertices + sectors[s].vtxOffset;
		const SectorWall* secWall = walls + sectors[s].wallOffset;
		const u32 wallCount = sectors[s].wallCount;
		for (u32 w = 0; w < wallCount; w++)
		{
			Vec2f v0 = secVtx[secWall[w].i0];
			Vec2f v1 = secVtx[secWall[w].i1];

			v0.x = v0.x*scale + xOffset;
			v0.z = v0.z*scale + zOffset;
			v1.x = v1.x*scale + xOffset;
			v1.z = v1.z*scale + zOffset;

			drawLine(&v0, &v1, secWall[w].adjoin < 0 ? 10 : 14);
		}
	}

	// Draw Map Markers
	const size_t count = s_markers.size();
	const MapMarker* markers = s_markers.data();
	for (size_t i = 0; i < count; i++)
	{
		if (layers != markers[i].layer) { continue; }

		const f32 w = 2.0f / scale;
		const f32 x0 = (markers[i].x - w)*scale + xOffset;
		const f32 z0 = (markers[i].z - w)*scale + zOffset;
		const f32 x1 = (markers[i].x + w)*scale + xOffset;
		const f32 z1 = (markers[i].z + w)*scale + zOffset;

		drawColoredQuad(s32(x0), s32(z0), s32(x1 - x0 + 1), s32(z1 - z0 + 1), markers[i].color);
	}
}