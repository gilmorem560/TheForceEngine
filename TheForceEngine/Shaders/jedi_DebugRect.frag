in vec2 Frag_Uv;
out vec4 Out_Color;

void main()
{
	Out_Color.rgb = vec3(8.0/256.0, 0.0, 0.0);
	Out_Color.a   = 1.0;

	vec2 dFxy = 3.0 * abs(vec2(dFdx(Frag_Uv.x), dFdy(Frag_Uv.y)));
	
	float xDist = min(Frag_Uv.x, 1.0f - Frag_Uv.x);
	float yDist = min(Frag_Uv.y, 1.0f - Frag_Uv.y);
	if (xDist > dFxy.x && yDist > dFxy.y)
	{
		discard;
	}
}
