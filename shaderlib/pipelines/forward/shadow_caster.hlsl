#include <pipelines/forward/surface.hlsli>
[shader("vertex")] SurfaceVertexOutput VSMain(SurfaceVertexInput v) { return forward_surface_vertex(v); }
[shader("pixel")] void PSMain(SurfaceVertexOutput v) { forward_surface_color(v.UV); }
