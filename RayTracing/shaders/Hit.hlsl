#include "Common.hlsl"

struct STriVertex {
	float3 position;
	float3 normal;
	float2 texCoord;
};

StructuredBuffer<STriVertex> BTriVertex : register(t0);
StructuredBuffer<int> indices : register(t1);
Texture2D tex : register(t2);
SamplerState gsamLinear  : register(s0);

[shader("closesthit")] void ClosestHit(inout HitInfo payload, Attributes attrib) {
	float3 barycentrics = float3(1.f - attrib.bary.x - attrib.bary.y, attrib.bary.x, attrib.bary.y);
	uint vertId = 3 * PrimitiveIndex();
	
	//float3 hitColor = float3(0.3, 0.8, 0.6);
	//hitColor =	BTriVertex[indices[vertId + 0]].normal * barycentrics.x +
	//			BTriVertex[indices[vertId + 1]].normal * barycentrics.y +
	//			BTriVertex[indices[vertId + 2]].normal * barycentrics.z;

	float2 hitTexCoord = BTriVertex[indices[vertId + 0]].texCoord * barycentrics.x +
						 BTriVertex[indices[vertId + 1]].texCoord * barycentrics.y +
						 BTriVertex[indices[vertId + 2]].texCoord * barycentrics.z;
	float4 reColor = tex.SampleLevel(gsamLinear, hitTexCoord,0.0);
	payload.colorAndDistance = float4(reColor.xyz , RayTCurrent());
	//payload.colorAndDistance = float4(hitColor, RayTCurrent());
	//payload.colorAndDistance = float4(hitTexCoord,1.0, RayTCurrent());
}
