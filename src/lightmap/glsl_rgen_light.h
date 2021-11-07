static const char* glsl_rgen_light = R"glsl(

#version 460
#extension GL_EXT_ray_tracing : require

struct hitPayload
{
	vec3 hitPosition;
	float hitAttenuation;
	int hitSurfaceIndex;
};

layout(location = 0) rayPayloadEXT hitPayload payload;

layout(set = 0, binding = 0) uniform accelerationStructureEXT acc;
layout(set = 0, binding = 1, rgba32f) uniform image2D startpositions;
layout(set = 0, binding = 2, rgba32f) uniform image2D positions;
layout(set = 0, binding = 3, rgba32f) uniform image2D outputs;

layout(set = 0, binding = 4) uniform Uniforms
{
	uint SampleIndex;
	uint SampleCount;
	uint PassType;
	uint Padding2;
	vec3 LightOrigin;
	float Padding0;
	float LightRadius;
	float LightIntensity;
	float LightInnerAngleCos;
	float LightOuterAngleCos;
	vec3 LightDir;
	float SampleDistance;
	vec3 LightColor;
	float Padding1;
};

struct SurfaceInfo
{
	vec3 Normal;
	float EmissiveDistance;
	vec3 EmissiveColor;
	float EmissiveIntensity;
	float Sky;
	float Padding0, Padding1, Padding2;
};

layout(set = 0, binding = 6) buffer SurfaceBuffer { SurfaceInfo surfaces[]; };

vec2 Hammersley(uint i, uint N);
float RadicalInverse_VdC(uint bits);

void main()
{
	ivec2 texelPos = ivec2(gl_LaunchIDEXT.xy);
	vec4 incoming = imageLoad(outputs, texelPos);
	vec4 data0 = imageLoad(positions, texelPos);
	int surfaceIndex = int(data0.w);
	if (surfaceIndex < 0 || incoming.w <= 0.0)
		return;

	const float minDistance = 0.01;

	vec3 origin = data0.xyz;
	float dist = distance(LightOrigin, origin);
	if (dist > minDistance && dist < LightRadius)
	{
		vec3 dir = normalize(LightOrigin - origin);

		SurfaceInfo surface = surfaces[surfaceIndex];
		vec3 normal = surface.Normal;

		float distAttenuation = max(1.0 - (dist / LightRadius), 0.0);
		float angleAttenuation = max(dot(normal, dir), 0.0);
		float spotAttenuation = 1.0;
		if (LightOuterAngleCos > -1.0)
		{
			float cosDir = dot(dir, LightDir);
			spotAttenuation = smoothstep(LightOuterAngleCos, LightInnerAngleCos, cosDir);
			spotAttenuation = max(spotAttenuation, 0.0);
		}

		float attenuation = distAttenuation * angleAttenuation * spotAttenuation;
		if (attenuation > 0.0)
		{
			float shadowAttenuation = 0.0;

			if (PassType == 0)
			{
				vec3 e0 = cross(normal, abs(normal.x) < abs(normal.y) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0));
				vec3 e1 = cross(normal, e0);
				e0 = cross(normal, e1);
				for (uint i = 0; i < SampleCount; i++)
				{
					vec2 offset = (Hammersley(i, SampleCount) - 0.5) * SampleDistance;
					vec3 origin2 = origin + offset.x * e0 + offset.y * e1;

					float dist2 = distance(LightOrigin, origin2);
					vec3 dir2 = normalize(LightOrigin - origin2);

					traceRayEXT(acc, gl_RayFlagsOpaqueEXT, 0xff, 1, 0, 1, origin2, minDistance, dir2, dist2, 0);
					shadowAttenuation += payload.hitAttenuation;
				}
				shadowAttenuation *= 1.0 / float(SampleCount);
			}
			else
			{
				traceRayEXT(acc, gl_RayFlagsOpaqueEXT, 0xff, 1, 0, 1, origin, minDistance, dir, dist, 0);
				shadowAttenuation = payload.hitAttenuation;
			}

			attenuation *= shadowAttenuation;

			incoming.rgb += LightColor * (attenuation * LightIntensity) * incoming.w;
		}
	}

	imageStore(outputs, texelPos, incoming);
}

float RadicalInverse_VdC(uint bits)
{
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10f; // / 0x100000000
}

vec2 Hammersley(uint i, uint N)
{
	return vec2(float(i) / float(N), RadicalInverse_VdC(i));
}

)glsl";
