#include "Shaders/grid.h"

uniform sampler2D image;
uniform sampler2D Palette;
uniform sampler2D ColorMap;

// x = sector ambient, y = ...
uniform ivec4 Lighting;

in vec2 Frag_Uv;
in vec3 Frag_Pos;
flat in uvec4 Frag_Color;
out vec4 Out_Color;

void main()
{
	// For now just do fullbright.
	// read the color index, it will be 0.0 - 255.0/256.0 range which maps to 0 - 255
	float index   = texture(image, Frag_Uv).r;
	//Out_Color.rgb = texture(Palette, vec2(index, 0.5)).rgb;
	float brightness = (16.0 + 32.0*Frag_Uv.x) / 256.0;  //float(Frag_Color) / 256.0;
	Out_Color.rgb = vec3(brightness, 0.0, 0.0);
	Out_Color.a   = 1.0;
}
