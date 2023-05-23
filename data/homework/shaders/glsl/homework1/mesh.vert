#version 450

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec3 inColor;
layout (location = 4) in vec4 inTangent;

layout (set = 0, binding = 0) uniform UBOScene
{
	mat4 projection;
	mat4 view;
	vec4 lightPos;
	vec4 viewPos;
} uboScene;

layout (set = 2, binding = 0) readonly buffer _ModelMatrix {
	mat4 _modelMatrix[];
};

layout (set = 2, binding = 1) readonly buffer _NormalMatrix {
	mat4 _normalMatrix[];
};

layout(push_constant) uniform PushConsts {
	uint nodeIndex;
} primitive;

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec3 outColor;
layout (location = 2) out vec2 outUV;
layout (location = 3) out vec3 outViewVec;
layout (location = 4) out vec3 outLightVec;
layout (location = 5) out vec4 outTangent;

void main() 
{
	mat4 model_matrix = _modelMatrix[primitive.nodeIndex];
	mat4 normal_matrix = _normalMatrix[primitive.nodeIndex]; //primitive.normalMatrix; //transpose( inverse( primitive.model ) ); 

	outTangent = (normal_matrix * inTangent);
	outTangent.w = inTangent.w;
	//outNormal = inNormal;
	outColor = inColor;
	outUV = inUV;

	vec4 pos_world = model_matrix * vec4(inPos.xyz, 1.0f);
	gl_Position = uboScene.projection * uboScene.view * pos_world;

	vec4 normal_world = normal_matrix * vec4(inNormal, 0.0f);
	outNormal = normalize(normal_world.xyz);
	
	//vec4 pos = uboScene.view * pos_world; //vec4(pos_world, 1.0);
	//outNormal = mat3(uboScene.view) * normal_world.xyz;
	//vec3 lPos = mat3(uboScene.view) * uboScene.lightPos.xyz;

	outLightVec = normalize(uboScene.lightPos.xyz - pos_world.xyz);
	outViewVec = normalize(uboScene.viewPos.xyz - pos_world.xyz);	
}