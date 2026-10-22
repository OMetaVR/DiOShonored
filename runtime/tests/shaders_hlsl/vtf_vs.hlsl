// vertex texture fetch (texldl in a vertex shader) + a vertex shader that exports unusual semantics
sampler2D s_disp : register(s0);
float4x4 g_wvp : register(c0);
struct VS_IN { float3 pos : POSITION; float2 uv : TEXCOORD0; float3 tan : TANGENT; float4 col : COLOR0; };
struct VS_OUT { float4 pos : POSITION; float2 uv : TEXCOORD0; float3 t : TEXCOORD3; float4 col : COLOR0; float psz : PSIZE; };
VS_OUT main(VS_IN i) {
    VS_OUT o;
    float h = tex2Dlod(s_disp, float4(i.uv, 0, 0)).r;
    o.pos = mul(float4(i.pos + float3(0, h, 0), 1), g_wvp);
    o.uv = i.uv; o.t = i.tan; o.col = i.col; o.psz = 4.0;
    return o;
}
