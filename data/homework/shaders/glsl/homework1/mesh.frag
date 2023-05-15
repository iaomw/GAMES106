#version 450

const float PI = 3.14159265359f;

layout (set = 1, binding = 0) uniform sampler2D texAlbedo;
layout (set = 1, binding = 1) uniform sampler2D texNormal;
layout (set = 1, binding = 2) uniform sampler2D texMetallicRoughness;

layout (set = 1, binding = 3) uniform sampler2D texAO;
layout (set = 1, binding = 4) uniform sampler2D texEmissive;

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec3 inColor;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec3 inViewVec;
layout (location = 4) in vec3 inLightVec;
layout (location = 5) in vec4 inTangent;

// Normal Distribution function --------------------------------------
float D_GGX(float dotNH, float roughness)
{
	float alpha = roughness * roughness;
	float alpha2 = alpha * alpha;
	float denom = dotNH * dotNH * (alpha2 - 1.0f) + 1.0f;
	return (alpha2)/(PI * denom*denom); 
}

// Geometric Shadowing function --------------------------------------
float G_SchlicksmithGGX(float dotNL, float dotNV, float roughness)
{
	float r = (roughness + 1.0);
	float k = (r*r) / 8.0;
	float GL = dotNL / (dotNL * (1.0 - k) + k);
	float GV = dotNV / (dotNV * (1.0 - k) + k);
	return GL * GV;
}

// Fresnel function ----------------------------------------------------
vec3 F_Schlick(float cosTheta, float metallic, vec3 albedo)
{
	vec3 F0 = mix(vec3(0.04), albedo, metallic); // * material.specular
	vec3 F = F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0); 
	return F;    
}

// Specular BRDF composition --------------------------------------------

vec3 BRDF(vec3 L, vec3 V, vec3 N, float metallic, float roughness, vec3 albedo)
{
	// Precalculate vectors and dot products	
	vec3 H = normalize (V + L);
	float dotNV = clamp(dot(N, V), 0.0, 1.0);
	float dotNL = clamp(dot(N, L), 0.0, 1.0);
	float dotLH = clamp(dot(L, H), 0.0, 1.0);
	float dotNH = clamp(dot(N, H), 0.0, 1.0);

	// Light color fixed
	vec3 lightColor = vec3(1.0);

	vec3 color = vec3(0.0);

	if (dotNL > 0.0)
	{
		float rroughness = max(0.05, roughness);
		// D = Normal distribution (Distribution of the microfacets)
		float D = D_GGX(dotNH, roughness); 
		// G = Geometric shadowing term (Microfacets shadowing)
		float G = G_SchlicksmithGGX(dotNL, dotNV, rroughness);
		// F = Fresnel factor (Reflectance depending on angle of incidence)
		vec3 F = F_Schlick(dotNV, metallic, albedo);

		vec3 spec = D * F * G / (4.0 * dotNL * dotNV + 0.001f);

		//color += spec * dotNL * lightColor;
		vec3 Ks = F;
		vec3 Kd = (vec3(1.0f)-Ks) * (1.0f - metallic);

		color += (Kd * albedo / PI + spec) * dotNL;
	}

	return color;
}

vec3 convertNormal()
{
	vec3 tangentNormal = texture(texNormal, inUV).xyz * 2.0 - 1.0;

	vec3 N = normalize(inNormal);
	vec3 T = normalize(inTangent.xyz);

	T = normalize(T - N * dot(N, T)); // make sure T is perpendicular to N

	vec3 B = normalize(cross(N, T) * inTangent.w);
	mat3 TBN = mat3(T, B, N);
	return normalize(TBN * tangentNormal);
}

layout (location = 0) out vec4 outFragColor;

void main() 
{
	vec4 albedo = texture(texAlbedo, inUV);
	albedo.rgb = pow(albedo.rgb, vec3(2.2));

	if (albedo.a < 0.8f) {
		discard;
	}

	float metallic = texture(texMetallicRoughness, inUV).r;
	float roughness = texture(texMetallicRoughness, inUV).g;

	vec3 N = convertNormal();

	//outFragColor.xyz = (N + 1.0) * 0.5; //(inNormal + 1.0f) * 0.5f;
	//return;

	vec3 V = normalize(inViewVec);
	vec3 L = normalize(inLightVec);
	//vec3 R = reflect(L, N);
	//vec3 diffuse = max(dot(N, L), 0.15) * inColor;
	//vec3 specular = pow(max(dot(R, V), 0.0), 16.0) * vec3(0.75);

	vec3 Li = vec3(0.0);
	Li += BRDF(L, V, N, metallic, roughness, albedo.rgb);
	// fake ambient
	Li += albedo.rgb * 0.05 * texture(texAO, inUV).rrr;
	vec3 emission = texture(texEmissive, inUV).rgb;
	Li += emission;

	// Linear to sRGB
	vec3 color = pow(Li, vec3(0.4545));

	// if (length(emission) > 0) {
	// 	albedo.a = 1.0f;
	// }

	//outFragColor = vec4(vec3(albedo.a), 1.0f);
	outFragColor = vec4(color, albedo.a);
}