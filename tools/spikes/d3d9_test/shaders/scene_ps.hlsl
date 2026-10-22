// SM3.0 pixel shader: samples a DXT5 texture + an RTT texture, uses a bool constant
// (static branch, like UE3's feature toggles) and a float4 tint constant.
sampler2D g_tex  : register(s0);  // DXT5 diffuse
sampler2D g_rtt  : register(s1);  // render-to-texture result
float4 g_tint    : register(c0);
bool   g_useRtt  : register(b0);

struct PS_IN {
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
    float2 uv2   : TEXCOORD1;
};
float4 main(PS_IN i) : COLOR0 {
    float4 c = tex2D(g_tex, i.uv) * i.color * g_tint;
    if (g_useRtt) {
        float4 r = tex2D(g_rtt, i.uv2);
        c.rgb = lerp(c.rgb, c.rgb * r.rgb * 1.5, r.a * 0.6);
    }
    return c;
}
