// The Hall of Meat's slow motion replay (hall_of_meat_replay.cpp): one picture drawn over the
// whole target, scaled with a linear filter. The pipeline's blend factor fades it over what is
// there. Compiled into hall_of_meat_replay_shaders.h with the Windows SDK's fxc:
//   fxc /nologo /T vs_5_0 /E vs /O3 /Vn replay_vs /Fh vs.h hall_of_meat_replay.hlsl
//   fxc /nologo /T ps_5_0 /E ps /O3 /Vn replay_ps /Fh ps.h hall_of_meat_replay.hlsl
// (the two outputs one after the other).
Texture2D picture : register(t0);
SamplerState smooth : register(s0);

struct Corner {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// A triangle that covers the target: no vertex buffer.
Corner vs(uint id : SV_VertexID) {
    Corner corner;
    corner.uv = float2((id << 1) & 2, id & 2);
    corner.position = float4(corner.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return corner;
}

float4 ps(Corner corner) : SV_Target {
    return float4(picture.Sample(smooth, corner.uv).rgb, 1);
}
