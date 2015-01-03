#include "CommonCode.vs"

struct VSInput
{
    float4 position : POSITION;
    float3 normal : NORMAL;
    #ifdef INSTANCED
    float4x3 instanceWorldMatrix : TEXCOORD4;
    #endif
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float3 worldPos : TEXCOORD1;
    #ifdef SHADOW
    float4 shadowPos[4] : TEXCOORD4;
    #endif
};

VSOutput main(VSInput input)
{
    VSOutput output;

    #ifdef INSTANCED
    output.worldPos = mul(input.position, input.instanceWorldMatrix);
    output.normal = normalize(mul(input.normal, (float3x3)input.instanceWorldMatrix));
    #else
    output.worldPos = mul(input.position, worldMatrix);
    output.normal = normalize(mul(input.normal, (float3x3)worldMatrix));
    #endif

    output.position = mul(float4(output.worldPos, 1.0), viewProjMatrix);
    #ifdef SHADOW
    for (int i = 0; i < 4; ++i)
        output.shadowPos[i] = mul(float4(output.worldPos, 1.0), shadowMatrices[i]);
    #endif
    return output;
}