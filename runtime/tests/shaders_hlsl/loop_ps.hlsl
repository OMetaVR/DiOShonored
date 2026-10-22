float4 g_c[4] : register(c0);
int g_loops : register(i0);
bool g_flag : register(b1);
struct PS_IN { float4 uv : TEXCOORD0; float3 n : TEXCOORD1; float4 col : COLOR0; };
float4 main(PS_IN i) : COLOR0 {
    float3 acc = 0;
    for (int k = 0; k < 4; k++) acc += g_c[k].xyz * (k + 1);
    if (g_flag) acc *= reflect(i.n, normalize(i.uv.xyz));
    return float4(acc, 1) * i.col;
}
