sampler2D s0 : register(s0);
sampler2D s1 : register(s1);
samplerCUBE sc : register(s2);
struct PS_IN { float4 uv : TEXCOORD0; float3 n : TEXCOORD1; float4 col : COLOR0; };
float4 main(PS_IN i) : COLOR0 {
    float4 a = tex2D(s0, i.uv.xy);
    float4 b = tex2Dlod(s1, float4(i.uv.zw, 0, 1.5));
    float4 c = tex2Dbias(s1, float4(i.uv.xy, 0, -1));
    float4 e = texCUBE(sc, i.n);
    clip(a.a - 0.3);
    float d = ddx(i.uv.x) + ddy(i.uv.y);
    float3 r = lerp(a.rgb, b.rgb * c.rgb, saturate(d)) * e.rgb;
    return float4(pow(abs(r), 2.2), a.a) * i.col;
}
