#include <core/platform.hlsli>
struct SignalData { float4 State; };
VK_BINDING(0,0) ConstantBuffer<SignalData> SignalFrame : register(b0,space0);
VK_BINDING(0,1) RWTexture2D<float4> SignalOutput : register(u0,space1);
VK_BINDING(1,1) Texture2D<float4> SignalPrevious : register(t0,space1);
VK_BINDING(2,1) SamplerState SignalSampler : register(s0,space1);
[shader("compute")]
[numthreads(8,8,1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint width,height; SignalOutput.GetDimensions(width,height);
    if (id.x>=width || id.y>=height) return;
    float2 uv=(float2(id.xy)+.5)/float2(width,height);
    if (SignalFrame.State.z>.5 && SignalFrame.State.w>.5) {
        SignalOutput[id.xy]=SignalPrevious.SampleLevel(SignalSampler,uv,0);
        return;
    }
    float2 p=(uv-.5)*float2(1.6,1);
    float time=SignalFrame.State.x;
    float r=length(p),a=atan2(p.y,p.x);
    float ribbon=exp(-abs(r-(.22+.08*sin(3*a-time)))*110);
    float pulse=pow(saturate(sin(r*25-time*2)),28)*.3;
    float3 ink=lerp(float3(.04,.7,.67),float3(1,.39,.11),.5+.5*sin(a+time*.3));
    float3 current=ink*(ribbon+pulse)+float3(.008,.016,.028);
    float3 previous=SignalPrevious.SampleLevel(SignalSampler,uv+float2(.0015*sin(time),-.002),0).rgb;
    float3 color=SignalFrame.State.y>.5 ? max(current,previous*.968) : current;
    SignalOutput[id.xy]=float4(saturate(color),1);
}
