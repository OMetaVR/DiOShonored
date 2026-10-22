float4x4 g_vp : register(c0);
float4 g_bones[60] : register(c8);
struct VS_IN { float3 pos : POSITION; float4 w : BLENDWEIGHT; float4 idx : BLENDINDICES; float3 n : NORMAL; float2 uv : TEXCOORD0; };
struct VS_OUT { float4 pos : POSITION; float2 uv : TEXCOORD0; float3 n : TEXCOORD1; float fog : FOG; };
VS_OUT main(VS_IN i) {
    VS_OUT o;
    int4 ix = (int4)i.idx * 3;
    float3 p = 0;
    for (int k = 0; k < 4; k++) {
        float4 r0 = g_bones[ix[k]], r1 = g_bones[ix[k] + 1], r2 = g_bones[ix[k] + 2];
        float4 hp = float4(i.pos, 1);
        p += i.w[k] * float3(dot(r0, hp), dot(r1, hp), dot(r2, hp));
    }
    o.pos = mul(float4(p, 1), g_vp);
    o.uv = i.uv;
    o.n = normalize(i.n);
    o.fog = saturate(o.pos.z * 0.01);
    return o;
}
