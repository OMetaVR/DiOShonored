// assorted ALU: pow/exp/log/rsq/rcp/min/max/cmp/dsx, dynamic if on a bool constant, shadow-style compare, derivative use
sampler2D s0 : register(s0);
float4 g_p : register(c0);
float4 g_q : register(c1);
bool g_b : register(b0);
struct PS_IN { float4 a : TEXCOORD0; float4 b : TEXCOORD1; float4 c : COLOR0; float4 d : COLOR1; };
float4 main(PS_IN i) : COLOR0 {
    float4 t = tex2Dproj(s0, i.a);
    float4 r = pow(max(t, 0.001), g_p) * rsqrt(max(i.b.x, 0.01)) / max(i.b.y, 0.1);
    r = (i.b.z > 0.5) ? r : r.zyxw * g_q;
    if (g_b) { r.rgb = normalize(r.rgb) * length(i.c.rgb); } else { r.a = exp2(log2(r.a + 1) * 0.5); }
    r += i.d * step(0.5, i.c.a) + cross(i.a.xyz, i.b.xyz).xyzx;
    return r;
}
