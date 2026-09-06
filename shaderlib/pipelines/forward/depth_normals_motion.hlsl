#include <pipelines/forward/surface.hlsli>
[shader("vertex")] SurfaceVertexOutput VSMain(SurfaceVertexInput v) { return forward_surface_vertex(v); }
struct PrepassOutput { float4 Normal : SV_Target0; float4 Motion : SV_Target1; };
[shader("pixel")] PrepassOutput PSMain(SurfaceVertexOutput v) {
    forward_surface_color(v.UV);
    PrepassOutput o;
    o.Normal = float4(safe_normalize(v.Normal, float3(0, 1, 0)), 1);
    bool valid = ForwardObject.MotionValid != 0 && v.PreviousClip.w > 0;
    o.Motion = valid ? float4(forward_clip_uv(v.CurrentClip) - forward_clip_uv(v.PreviousClip), v.PreviousClip.z / v.PreviousClip.w, 1) : 0;
    return o;
}
