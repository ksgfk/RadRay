// https://www.shadertoy.com/view/4df3Rn

#include <common.hlsl>

struct MandelbrotParams {
    float centerX;
    float centerY;
    float scale;    // half-height in complex plane units
    float aspect;   // width / height
    uint maxIter;
    uint width;
    uint height;
    uint aaLevel;   // supersampling: 1=off, 2=2x2, 3=3x3, 4=4x4
};

VK_PUSH_CONSTANT ConstantBuffer<MandelbrotParams> _Params : register(b0);
VK_BINDING(0, 0) VK_IMAGE_FORMAT(rgba8) RWTexture2D<unorm float4> _Output : register(u0, space0);

// Returns smooth iteration count, or 0.0 for interior points.
float Mandelbrot(float2 c, uint maxIter) {
    float c2 = dot(c, c);
    // Early-out: skip computation inside M1 bulb
    if (256.0 * c2 * c2 - 96.0 * c2 + 32.0 * c.x - 3.0 < 0.0) return 0.0;
    // Early-out: skip computation inside M2 bulb
    if (16.0 * (c2 + 2.0 * c.x + 1.0) - 1.0 < 0.0) return 0.0;

    const float B = 256.0;
    float n = 0.0;
    float2 z = float2(0.0, 0.0);
    for (uint i = 0; i < maxIter; i++) {
        z = float2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + c;
        if (dot(z, z) > B * B) break;
        n += 1.0;
    }
    if (n >= float(maxIter)) return 0.0;

    // Smooth iteration count
    return n - log2(log2(dot(z, z))) + 4.0;
}

float3 ColorMap(float l) {
    if (l < 0.5) return float3(0.0, 0.0, 0.0);
    return 0.5 + 0.5 * cos(3.0 + l * 0.15 + float3(0.0, 0.6, 1.0));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= _Params.width || id.y >= _Params.height) return;

    const float2 res = float2(_Params.width, _Params.height);
    const uint aa = max(1u, min(4u, _Params.aaLevel));
    float3 col = float3(0.0, 0.0, 0.0);

    for (uint m = 0; m < aa; m++) {
        for (uint n = 0; n < aa; n++) {
            // Stratified sub-pixel offset: centers samples within each cell
            float2 subPixel = (float2(m, n) + 0.5) / float(aa);
            float2 uv = (float2(id.xy) + subPixel) / res;
            float cx = _Params.centerX + (uv.x - 0.5) * _Params.scale * _Params.aspect * 2.0;
            float cy = _Params.centerY - (uv.y - 0.5) * _Params.scale * 2.0;

            float l = Mandelbrot(float2(cx, cy), _Params.maxIter);
            col += ColorMap(l);
        }
    }
    col /= float(aa * aa);
    _Output[id.xy] = float4(col, 1.0);
}
