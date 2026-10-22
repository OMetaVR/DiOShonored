// SM3.0 pixel shader for the render-to-texture pass: procedural rings + gradient, alpha varies.
float4 g_params : register(c0); // x = time

struct PS_IN {
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
    float2 uv2   : TEXCOORD1;
};
float4 main(PS_IN i) : COLOR0 {
    float2 p = i.uv - 0.5;
    float d = length(p);
    float rings = 0.5 + 0.5 * sin(d * 40.0 - g_params.x * 3.0);
    float3 col = float3(0.2 + 0.8 * i.uv.x, 0.3 + 0.7 * rings, 1.0 - 0.8 * i.uv.y);
    float a = saturate(1.0 - d * 1.6);
    return float4(col, a);
}
