// SM3.0 vertex shader: transforms by WVP (c0..c3), passes UV and vertex color.
// Mirrors UE3 D3D9Drv style: constants uploaded via SetVertexShaderConstantF.
float4x4 g_wvp : register(c0);
float4   g_params : register(c4);   // x = time, y = uv scroll, z,w unused

struct VS_IN {
    float3 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};
struct VS_OUT {
    float4 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
    float2 uv2   : TEXCOORD1;
};
VS_OUT main(VS_IN i) {
    VS_OUT o;
    o.pos   = mul(float4(i.pos, 1.0), g_wvp);
    o.uv    = i.uv;
    o.uv2   = i.uv * 2.0 + float2(g_params.y, 0.0);
    o.color = i.color;
    return o;
}
