uniform vec3 CameraPos;
uniform mat3 CameraView;
uniform mat4 CameraProj;

in vec3 vtx_pos;
in vec2 vtx_uv;
in uvec4 vtx_color; // x = lighting offset, ...
out vec2 Frag_Uv;
out vec3 Frag_Pos;
flat out uvec4 Frag_Color;
void main()
{
    vec3 vpos = (vtx_pos - CameraPos) * CameraView;
	gl_Position = vec4(vpos,  1.0) * CameraProj;
		
	Frag_Uv  = vtx_uv;
	Frag_Pos = vpos;
	Frag_Color = vtx_color;
}
