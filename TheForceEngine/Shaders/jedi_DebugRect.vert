in vec2 vtx_pos;
in vec2 vtx_uv;
out vec2 Frag_Uv;
void main()
{
	gl_Position = vec4(vtx_pos.x, vtx_pos.y, 0.0, 1.0);
	Frag_Uv = vtx_uv;
}
