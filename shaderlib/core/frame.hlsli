#ifndef RADRAY_CORE_FRAME_HLSLI
#define RADRAY_CORE_FRAME_HLSLI

#include <core/math.hlsli>

// 着色局部坐标系 (shading frame)。约定 n = +Z, 即局部空间里 z 分量就是 cos(theta)。
// 所有 BSDF 一律在局部系求值, 世界空间的方向向量必须先经 frame_to_local 变换。

struct Frame3 {
    float3 S;  // 切线
    float3 T;  // 副切线
    float3 N;  // 法线 (局部 +Z)
};

/// 由单个法线构造正交基 (Duff et al. 2017, branchless ONB)。
/// 切线方向是任意的, 因此只适用于各向同性材质, 或各向异性方向本身无所谓的场合。
/// 需要贴图对齐的各向异性时请用 make_frame_tangent 传入几何切线。
Frame3 make_frame(float3 n) {
    float sign = (n.z >= 0.0f) ? 1.0f : -1.0f;
    float a = -1.0f / (sign + n.z);
    float b = n.x * n.y * a;
    Frame3 f;
    f.S = float3(1.0f + sign * n.x * n.x * a, sign * b, -sign * n.x);
    f.T = float3(b, sign + n.y * n.y * a, -n.y);
    f.N = n;
    return f;
}

/// 由法线 + 几何切线构造坐标系, 对切线做 Gram-Schmidt 正交化。
/// handedness 即 glTF TANGENT.w (副切线的手性符号)。
Frame3 make_frame_tangent(float3 n, float3 tangent, float handedness) {
    Frame3 f;
    f.N = n;
    f.S = safe_normalize(tangent - n * dot(tangent, n), float3(1.0f, 0.0f, 0.0f));
    f.T = cross(n, f.S) * handedness;
    return f;
}

/// 世界 -> 局部。与 frame_to_world 互逆 (正交基, 转置即逆)。
float3 frame_to_local(Frame3 f, float3 v) {
    return float3(dot(v, f.S), dot(v, f.T), dot(v, f.N));
}

/// 局部 -> 世界。
float3 frame_to_world(Frame3 f, float3 v) {
    return f.S * v.x + f.T * v.y + f.N * v.z;
}

#endif
