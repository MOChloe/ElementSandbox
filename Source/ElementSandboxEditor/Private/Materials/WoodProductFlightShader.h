#pragma once

namespace UE::ElementSandbox::WoodFlight
{
// 仅被 Editor Commandlet 写入材质；Time 节点让原生 velocity pass 自动求上一帧 WPO。
// P0/V/A/W/Q0/QR/H 均来自每实例数据，RestOffset 已在材质图内以 LWC 精度减去实例枢轴。
inline const TCHAR* PositionCode = TEXT(R"(
struct FlightMath
{
	float4 Mul(float4 a, float4 b)
	{
		return float4(a.w*b.xyz + b.w*a.xyz + cross(a.xyz,b.xyz), a.w*b.w-dot(a.xyz,b.xyz));
	}
	float3 Rotate(float4 q, float3 v) { return v + 2.0*cross(q.xyz, cross(q.xyz,v)+q.w*v); }
};
FlightMath M;
DeltaRotation = float4(0,0,0,1);
if (H.w < 0.5) return -RestOffset;
if (H.w > 1.5) return float3(0,0,0);
float elapsed = max(Time-P0.w,0.0);
float impact = max(V.w,0.0);
float settle = max(A.w,0.0);
if (Time < P0.w) return -RestOffset;
if (elapsed >= impact+settle) return float3(0,0,0);
float t = min(elapsed,impact);
float3 center = P0.xyz+V.xyz*t+0.5*A.xyz*t*t;
float4 q = Q0;
float speed = length(W.xyz);
if (speed > 0.000001)
{
	float angle = speed*t*0.017453292519943295*0.5;
	q = normalize(M.Mul(float4(W.xyz/speed*sin(angle),cos(angle)),q));
}
if (elapsed >= impact)
{
	float u = settle > 0.000001 ? saturate((elapsed-impact)/settle) : 1.0;
	float alpha = u*u*(3.0-2.0*u);
	center = lerp(H.xyz,float3(0,0,0),alpha);
	center.z += W.w*4.0*alpha*(1.0-alpha);
	float4 end = dot(q,QR) < 0.0 ? -QR : QR;
	q = normalize(lerp(q,end,alpha));
}
DeltaRotation = normalize(M.Mul(q,float4(-QR.xyz,QR.w)));
return center+M.Rotate(DeltaRotation,RestOffset)-RestOffset;
)");
inline const TCHAR* NormalCode = TEXT(R"(
float4 q = normalize(Rotation);
return normalize(BaseNormal+2.0*cross(q.xyz,cross(q.xyz,BaseNormal)+q.w*BaseNormal));
)");
}
