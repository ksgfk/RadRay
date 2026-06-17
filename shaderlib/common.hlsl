#ifndef COMMON_HLSLI
#define COMMON_HLSLI

#if defined(VULKAN) || defined(METAL)
#define VK_LOCATION(l)   [[vk::location(l)]]
#define VK_BINDING(b, s) [[vk::binding(b, s)]]
#define VK_PUSH_CONSTANT [[vk::push_constant]]
#define VK_IMAGE_FORMAT(fmt) [[vk::image_format(#fmt)]]
#else
#define VK_LOCATION(l)
#define VK_BINDING(b, s)
#define VK_PUSH_CONSTANT
#define VK_IMAGE_FORMAT(fmt)
#endif

static const float PI = 3.14159265358979323846;
static const float INV_PI = 0.3183098861837907;

struct Frame3
{
    float3 s;
    float3 t;
    float3 n;
};

Frame3 make_frame(float3 n)
{
    float sign = (n.z >= 0.0) ? 1.0 : -1.0;
    float a = -1.0 / (sign + n.z);
    float b = n.x * n.y * a;
    Frame3 f;
    f.s = float3(1.0 + sign * n.x * n.x * a, sign * b, -sign * n.x);
    f.t = float3(b, sign + n.y * n.y * a, -n.y);
    f.n = n;
    return f;
}

float3 to_local(Frame3 f, float3 v)
{
    return float3(dot(v, f.s), dot(v, f.t), dot(v, f.n));
}

float3 to_world(Frame3 f, float3 v)
{
    return f.s * v.x + f.t * v.y + f.n * v.z;
}

float3 linear_to_srgb(float3 c)
{
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, (float3)0.0), (float3)(1.0 / 2.4)) - 0.055;
    return float3(
        c.x < 0.0031308 ? lo.x : hi.x,
        c.y < 0.0031308 ? lo.y : hi.y,
        c.z < 0.0031308 ? lo.z : hi.z);
    // return pow(saturate(c), 1.0 / 2.2);
}

#endif
