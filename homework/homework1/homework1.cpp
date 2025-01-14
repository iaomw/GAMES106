/*
* Vulkan Example - glTF scene loading and rendering
*
* Copyright (C) 2020-2022 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

/*
 * Shows how to load and display a simple scene from a glTF file
 * Note that this isn't a complete glTF loader and only basic functions are shown here
 * This means no complex materials, no animations, no skins, etc.
 * For details on how glTF 2.0 works, see the official spec at https://github.com/KhronosGroup/glTF/tree/master/specification/2.0
 *
 * Other samples will load models using a dedicated model loader with more features (see base/VulkanglTFModel.hpp)
 *
 * If you are looking for a complete glTF implementation, check out https://github.com/SaschaWillems/Vulkan-glTF-PBR/
 */

#include "glm/gtc/type_ptr.hpp"
#include <thread>
#include <stdint.h>
#include <unordered_map>
#include <utility>
#include <vector>
#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#ifdef VK_USE_PLATFORM_ANDROID_KHR
#define TINYGLTF_ANDROID_LOAD_FROM_ASSETS
#endif
#include "tiny_gltf.h"

#include "vulkanexamplebase.h"

#define ENABLE_VALIDATION false

// Contains everything required to render a glTF model in Vulkan
// This class is heavily simplified (compared to glTF's feature set) but retains the basic glTF structure
class VulkanglTFModel
{
public:
	// The class requires some Vulkan objects so it can create it's own resources
	vks::VulkanDevice* vulkanDevice;
	VkQueue copyQueue;

	// The vertex layout for the samples' model
	struct Vertex {
		glm::vec3 pos;
		glm::vec3 normal;
		glm::vec2 uv;
		glm::vec4 color;
		glm::vec4 tangent;
	};

	// Single vertex buffer for all primitives
	struct {
		VkBuffer buffer;
		VkDeviceMemory memory;
	} vertices;

	// Single index buffer for all primitives
	struct {
		int count;
		VkBuffer buffer;
		VkDeviceMemory memory;
	} indices;

	// The following structures roughly represent the glTF scene structure
	// To keep things simple, they only contain those properties that are required for this sample
	struct Node;

	// A primitive contains the data for a single draw call
	struct Primitive {
		uint32_t firstIndex;
		uint32_t indexCount;
		int32_t materialIndex;
	};

	template <typename Type>
	struct WrapperSSBO {
		vks::Buffer ssbo{};
		std::vector<Type> contents{};
		VkDescriptorSet descriptorSet{};

		size_t byte_length() {
			return contents.size() * sizeof(Type);
		}
	};

	WrapperSSBO<glm::mat4> modelMatrices;
	WrapperSSBO<glm::mat4> normalMatrices;

	std::vector<
		std::function<void(float)>
	> animationCallbacks;

	// Contains the node's (optional) geometry and can be made up of an arbitrary number of primitives
	struct Mesh {
		std::vector<Primitive> primitives;
	};

	// A node represents an object in the glTF scene graph
	struct Node {
		uint32_t index;
		Node* parent;
		Mesh mesh;

		std::vector<Node*> children;

		glm::mat4 defaultMatrix = glm::mat4(1.0f);

		glm::vec3 translation = glm::vec3(0.0f);
		glm::quat rotation {1,0,0,0};
		glm::vec3 scale {1, 1, 1};

		glm::mat4 animatedMatrix() {
			glm::mat4 result_mat = glm::mat4(rotation) * glm::scale(glm::mat4(1.0f), scale) * defaultMatrix;
			result_mat = glm::translate(glm::mat4(1.0f), translation) * result_mat;
			return result_mat;
		};

		~Node() {
			for (auto& child : children) {
				delete child;
			}
		}
	};

	
	Node* findNode(Node *parent, uint32_t index)
	{
		Node *nodeFound = nullptr;
		if (parent->index == index)
		{
			return parent;
		}
		for (auto &child : parent->children)
		{
			nodeFound = findNode(child, index);
			if (nodeFound)
			{
				break;
			}
		}
		return nodeFound;
	}

	Node* nodeFromIndex(uint32_t index)
	{
		Node *nodeFound = nullptr;
		for (auto &node : nodes)
		{
			nodeFound = findNode(node, index);
			if (nodeFound)
			{
				break;
			}
		}
		return nodeFound;
	}

	// A glTF material stores information in e.g. the texture that is attached to it and colors
	struct Material {
		int baseColorTextureIndex = -1;
		glm::vec4 baseColorFactor = glm::vec4(1.0f);
		
		int metallicRoughnessTextureIndex = -1;
		float metalicFactor = 1.0f;
		float roughnessFactor = 1.0f;

		int normalTexureIndex = -1;
		int occlusionTextureIndex = -1;

		int emissiveTextureIndex = -1;
		glm::vec3 emissiveFactor = glm::vec3(1.0f);

		std::string alphaMode{}; //"BLEND" "MASK" "OPAQUE"
		VkDescriptorSet descriptorSet;
	};

	// Contains the texture for a single glTF image
	// Images may be reused by texture objects and are as such separated
	struct Image {
		vks::Texture2D texture;
		// We also store (and create) a descriptor set that's used to access this texture from the fragment shader
		VkDescriptorSet descriptorSet;
	};

	// A glTF texture stores a reference to the image and a sampler
	// In this sample, we are only interested in the image
	struct Texture {
		int32_t imageIndex;
	};

	/*
		Model data
	*/
	std::vector<Image> images;
	std::vector<Texture> textures;
	std::vector<Material> materials;
	std::vector<Node*> nodes;

	~VulkanglTFModel()
	{
		modelMatrices.ssbo.destroy();
		normalMatrices.ssbo.destroy();

		for (auto node : nodes) {
			delete node;
		}
		// Release all Vulkan resources allocated for the model
		vkDestroyBuffer(vulkanDevice->logicalDevice, vertices.buffer, nullptr);
		vkFreeMemory(vulkanDevice->logicalDevice, vertices.memory, nullptr);
		vkDestroyBuffer(vulkanDevice->logicalDevice, indices.buffer, nullptr);
		vkFreeMemory(vulkanDevice->logicalDevice, indices.memory, nullptr);
		for (Image image : images) {
			vkDestroyImageView(vulkanDevice->logicalDevice, image.texture.view, nullptr);
			vkDestroyImage(vulkanDevice->logicalDevice, image.texture.image, nullptr);
			vkDestroySampler(vulkanDevice->logicalDevice, image.texture.sampler, nullptr);
			vkFreeMemory(vulkanDevice->logicalDevice, image.texture.deviceMemory, nullptr);
		}
	}

	/*
		glTF loading functions

		The following functions take a glTF input model loaded via tinyglTF and convert all required data into our own structure
	*/

	void loadImages(tinygltf::Model& input)
	{
		// Images can be stored inside the glTF (which is the case for the sample model), so instead of directly
		// loading them from disk, we fetch them from the glTF loader and upload the buffers
		images.resize(input.images.size());
		for (size_t i = 0; i < input.images.size(); i++) {
			tinygltf::Image& glTFImage = input.images[i];
			// Get the image data from the glTF loader
			unsigned char* buffer = nullptr;
			VkDeviceSize bufferSize = 0;
			bool deleteBuffer = false;
			// We convert RGB-only images to RGBA, as most devices don't support RGB-formats in Vulkan
			if (glTFImage.component == 3) {
				bufferSize = glTFImage.width * glTFImage.height * 4;
				buffer = new unsigned char[bufferSize];
				unsigned char* rgba = buffer;
				unsigned char* rgb = &glTFImage.image[0];
				for (size_t i = 0; i < glTFImage.width * glTFImage.height; ++i) {
					memcpy(rgba, rgb, sizeof(unsigned char) * 3);
					rgba += 4;
					rgb += 3;
				}
				deleteBuffer = true;
			}
			else {
				buffer = &glTFImage.image[0];
				bufferSize = glTFImage.image.size();
			}
			// Load texture from image buffer
			images[i].texture.fromBuffer(buffer, bufferSize, VK_FORMAT_R8G8B8A8_UNORM, glTFImage.width, glTFImage.height, vulkanDevice, copyQueue);
			if (deleteBuffer) {
				delete[] buffer;
			}
		}
	}

	void loadTextures(tinygltf::Model& input)
	{
		textures.resize(input.textures.size());
		for (size_t i = 0; i < input.textures.size(); i++) {
			textures[i].imageIndex = input.textures[i].source;
		}
	}

	void loadMaterials(tinygltf::Model& input)
	{
		materials.resize(input.materials.size());
		for (size_t i = 0; i < input.materials.size(); i++) {
			// We only read the most basic properties required for our sample
			tinygltf::Material glTFMaterial = input.materials[i];
			// Get the base color factor
			if (glTFMaterial.values.find("baseColorFactor") != glTFMaterial.values.end()) {
				materials[i].baseColorFactor = glm::make_vec4(glTFMaterial.values["baseColorFactor"].ColorFactor().data());
			}
			// Get base color texture index
			if (glTFMaterial.values.find("baseColorTexture") != glTFMaterial.values.end()) {
				materials[i].baseColorTextureIndex = glTFMaterial.values["baseColorTexture"].TextureIndex();
			}

			//"metallicFactor", "roughnessFactor", "emissiveFactor"
			materials[i].baseColorTextureIndex = glTFMaterial.pbrMetallicRoughness.baseColorTexture.index;
			materials[i].metallicRoughnessTextureIndex = glTFMaterial.pbrMetallicRoughness.metallicRoughnessTexture.index;
			materials[i].normalTexureIndex = glTFMaterial.normalTexture.index;
			materials[i].emissiveTextureIndex = glTFMaterial.emissiveTexture.index;
			materials[i].occlusionTextureIndex = glTFMaterial.occlusionTexture.index;

			materials[i].metalicFactor = (float)glTFMaterial.pbrMetallicRoughness.metallicFactor;
			materials[i].roughnessFactor = (float)glTFMaterial.pbrMetallicRoughness.roughnessFactor;

			materials[i].emissiveFactor = glm::vec3(glTFMaterial.emissiveFactor.at(0),
													glTFMaterial.emissiveFactor.at(1),
													glTFMaterial.emissiveFactor.at(2));

			materials[i].alphaMode = glTFMaterial.alphaMode;

			assert(materials[i].metallicRoughnessTextureIndex != -1);
			assert(materials[i].baseColorTextureIndex != -1);
			assert(materials[i].normalTexureIndex != -1);
			//assert(materials[i].emissiveTextureIndex != -1);
			//assert(materials[i].occlusionTextureIndex != -1);
		}
	}

	void loadNode(const tinygltf::Node& inputNode, const int nodeIdx, const tinygltf::Model& input, VulkanglTFModel::Node* parent, std::vector<uint32_t>& indexBuffer, std::vector<VulkanglTFModel::Vertex>& vertexBuffer)
	{
		VulkanglTFModel::Node* node = new VulkanglTFModel::Node{};
		node->defaultMatrix = glm::mat4(1.0f);
		node->parent = parent;
		node->index = nodeIdx;

		// Get the local node matrix
		// It's either made up from translation, rotation, scale or a 4x4 matrix
		if (inputNode.translation.size() == 3) {
			node->translation = glm::make_vec3(inputNode.translation.data());
		}
		if (inputNode.rotation.size() == 4) {
			glm::quat q = glm::make_quat(inputNode.rotation.data());
			node->rotation = q;
		}
		if (inputNode.scale.size() == 3) {
			node->scale = glm::make_vec3(inputNode.scale.data());
		}
		if (inputNode.matrix.size() == 16) {
			node->defaultMatrix = glm::make_mat4x4(inputNode.matrix.data());
		};

		// Load node's children
		if (inputNode.children.size() > 0) {
			for (size_t i = 0; i < inputNode.children.size(); i++) {
				auto nodeIdx = inputNode.children[i];
				loadNode(input.nodes[inputNode.children[i]], nodeIdx, input , node, indexBuffer, vertexBuffer);
			}
		}

		// If the node contains mesh data, we load vertices and indices from the buffers
		// In glTF this is done via accessors and buffer views
		if (inputNode.mesh > -1) {
			const tinygltf::Mesh mesh = input.meshes[inputNode.mesh];
			// Iterate through all primitives of this node's mesh
			for (size_t i = 0; i < mesh.primitives.size(); i++) {
				const tinygltf::Primitive& glTFPrimitive = mesh.primitives[i];
				uint32_t firstIndex = static_cast<uint32_t>(indexBuffer.size());
				uint32_t vertexStart = static_cast<uint32_t>(vertexBuffer.size());
				uint32_t indexCount = 0;
				// Vertices
				{
					const float* positionBuffer = nullptr;
					const float* normalsBuffer = nullptr;
					const float* texCoordsBuffer = nullptr;
					const float* tangentBuffer = nullptr;
					size_t vertexCount = 0;

					// Get buffer data for vertex positions
					if (glTFPrimitive.attributes.find("POSITION") != glTFPrimitive.attributes.end()) {
						const tinygltf::Accessor& accessor = input.accessors[glTFPrimitive.attributes.find("POSITION")->second];
						const tinygltf::BufferView& view = input.bufferViews[accessor.bufferView];
						positionBuffer = reinterpret_cast<const float*>(&(input.buffers[view.buffer].data[accessor.byteOffset + view.byteOffset]));
						vertexCount = accessor.count;
					}
					// Get buffer data for vertex normals
					if (glTFPrimitive.attributes.find("NORMAL") != glTFPrimitive.attributes.end()) {
						const tinygltf::Accessor& accessor = input.accessors[glTFPrimitive.attributes.find("NORMAL")->second];
						const tinygltf::BufferView& view = input.bufferViews[accessor.bufferView];
						normalsBuffer = reinterpret_cast<const float*>(&(input.buffers[view.buffer].data[accessor.byteOffset + view.byteOffset]));
					}
					// Get buffer data for vertex texture coordinates
					// glTF supports multiple sets, we only load the first one
					if (glTFPrimitive.attributes.find("TEXCOORD_0") != glTFPrimitive.attributes.end()) {
						const tinygltf::Accessor& accessor = input.accessors[glTFPrimitive.attributes.find("TEXCOORD_0")->second];
						const tinygltf::BufferView& view = input.bufferViews[accessor.bufferView];
						texCoordsBuffer = reinterpret_cast<const float*>(&(input.buffers[view.buffer].data[accessor.byteOffset + view.byteOffset]));
					}

					if (glTFPrimitive.attributes.find("TANGENT") != glTFPrimitive.attributes.end()) {
						const tinygltf::Accessor& accessor = input.accessors[glTFPrimitive.attributes.find("TANGENT")->second];
						const tinygltf::BufferView& view = input.bufferViews[accessor.bufferView];
						tangentBuffer = reinterpret_cast<const float*>(&(input.buffers[view.buffer].data[accessor.byteOffset + view.byteOffset]));
					}

					// Append data to model's vertex buffer
					for (size_t v = 0; v < vertexCount; v++) {
						Vertex vert{};
						vert.pos = glm::vec4(glm::make_vec3(&positionBuffer[v * 3]), 1.0f);
						vert.normal = glm::normalize(glm::vec3(normalsBuffer ? glm::make_vec3(&normalsBuffer[v * 3]) : glm::vec3(0.0f)));
						vert.uv = texCoordsBuffer ? glm::make_vec2(&texCoordsBuffer[v * 2]) : glm::vec3(0.0f);
						vert.color = glm::vec4(1.0f);
						vert.tangent = tangentBuffer ? glm::make_vec4(&tangentBuffer[v*4]) : glm::vec4(0.0f);
						vertexBuffer.push_back(vert);
					}
				}
				// Indices
				{
					const tinygltf::Accessor& accessor = input.accessors[glTFPrimitive.indices];
					const tinygltf::BufferView& bufferView = input.bufferViews[accessor.bufferView];
					const tinygltf::Buffer& buffer = input.buffers[bufferView.buffer];

					indexCount += static_cast<uint32_t>(accessor.count);

					// glTF supports different component types of indices
					switch (accessor.componentType) {
					case TINYGLTF_PARAMETER_TYPE_UNSIGNED_INT: {
						const uint32_t* buf = reinterpret_cast<const uint32_t*>(&buffer.data[accessor.byteOffset + bufferView.byteOffset]);
						for (size_t index = 0; index < accessor.count; index++) {
							indexBuffer.push_back(buf[index] + vertexStart);
						}
						break;
					}
					case TINYGLTF_PARAMETER_TYPE_UNSIGNED_SHORT: {
						const uint16_t* buf = reinterpret_cast<const uint16_t*>(&buffer.data[accessor.byteOffset + bufferView.byteOffset]);
						for (size_t index = 0; index < accessor.count; index++) {
							indexBuffer.push_back(buf[index] + vertexStart);
						}
						break;
					}
					case TINYGLTF_PARAMETER_TYPE_UNSIGNED_BYTE: {
						const uint8_t* buf = reinterpret_cast<const uint8_t*>(&buffer.data[accessor.byteOffset + bufferView.byteOffset]);
						for (size_t index = 0; index < accessor.count; index++) {
							indexBuffer.push_back(buf[index] + vertexStart);
						}
						break;
					}
					default:
						std::cerr << "Index component type " << accessor.componentType << " not supported!" << std::endl;
						return;
					}
				}
				Primitive primitive{};
				primitive.firstIndex = firstIndex;
				primitive.indexCount = indexCount;
				primitive.materialIndex = glTFPrimitive.material;
				node->mesh.primitives.push_back(primitive);
			}
		}

		if (parent) {
			parent->children.push_back(node);
		}
		else {
			nodes.push_back(node);
		}
	}

	/*
		glTF rendering functions
	*/

	// Draw a single node including child nodes (if present)
	void drawNode(VkCommandBuffer commandBuffer, VkPipelineLayout pipelineLayout, VulkanglTFModel::Node* node)
	{
		if (node->mesh.primitives.size() > 0) {

			vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t), &node->index);

			for (VulkanglTFModel::Primitive& primitive : node->mesh.primitives) {
				if (primitive.indexCount > 0) {
					// Get the texture index for this primitive
					// Bind the descriptor for the current primitive's texture
					auto& material = materials[primitive.materialIndex];
					vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 1, 1, &material.descriptorSet, 0, nullptr);
					vkCmdDrawIndexed(commandBuffer, primitive.indexCount, 1, primitive.firstIndex, 0, 0);
				}
			}
		}
		for (auto& child : node->children) {
			drawNode(commandBuffer, pipelineLayout, child);
		}
	}

	void diveNode(VulkanglTFModel::Node* node) {
		if (node->mesh.primitives.size() > 0) {

			glm::mat4 nodeMatrix = node->animatedMatrix();
			
			VulkanglTFModel::Node* currentParent = node->parent;
			while (currentParent) {
				nodeMatrix = currentParent->animatedMatrix() * nodeMatrix;
				currentParent = currentParent->parent;
			}

			modelMatrices.contents[node->index] = nodeMatrix;
			normalMatrices.contents[node->index] = glm::transpose( glm::inverse( nodeMatrix ) );
		}
		for (auto& child : node->children) {
			diveNode(child);
		}
	}

	// Draw the glTF scene starting at the top-level-nodes
	void draw(VkCommandBuffer commandBuffer, VkPipelineLayout pipelineLayout)
	{
		// All vertices and indices are stored in single buffers, so we only need to bind once
		VkDeviceSize offsets[1] = { 0 };
		vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertices.buffer, offsets);
		vkCmdBindIndexBuffer(commandBuffer, indices.buffer, 0, VK_INDEX_TYPE_UINT32);

		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 2, 1, &modelMatrices.descriptorSet, 0, nullptr);

		// Render all nodes at top-level
		for (auto& node : nodes) {
			drawNode(commandBuffer, pipelineLayout, node);
		}
	}

};

class VulkanExample : public VulkanExampleBase
{
public:
	bool wireframe = false;

	VulkanglTFModel glTFModel;

	vks::Texture2D fallbackTexAO;
	vks::Texture2D fallbackTexEmissive;

	struct ShaderData {
		vks::Buffer buffer;
		struct Values {
			glm::mat4 projection;
			glm::mat4 model;
			glm::vec4 lightPos = glm::vec4(5.0f, 5.0f, -5.0f, 1.0f);
			glm::vec4 viewPos;
		} values;
	} shaderData;

	struct Pipelines {
		VkPipeline solid;
		VkPipeline wireframe = VK_NULL_HANDLE;
	} pipelines;

	VkPipelineLayout pipelineLayout;
	VkDescriptorSet descriptorSet;

	struct DescriptorSetLayouts {
		VkDescriptorSetLayout scene;
		VkDescriptorSetLayout textures;

		VkDescriptorSetLayout nodeMatrices;

	} descriptorSetLayouts;

	VulkanExample() : VulkanExampleBase(ENABLE_VALIDATION)
	{
		title = "homework1";
		camera.type = Camera::CameraType::lookat;
		camera.flipY = true;
		camera.setPosition(glm::vec3(0.0f, -0.1f, -1.0f));
		camera.setRotation(glm::vec3(0.0f, 45.0f, 0.0f));
		camera.setPerspective(60.0f, (float)width / (float)height, 0.1f, 256.0f);
	}

	~VulkanExample()
	{
		// Clean up used Vulkan resources
		// Note : Inherited destructor cleans up resources stored in base class
		vkDestroyPipeline(device, pipelines.solid, nullptr);
		if (pipelines.wireframe != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, pipelines.wireframe, nullptr);
		}

		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.scene, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.textures, nullptr);

		vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.nodeMatrices, nullptr);
		
		destroyFallbackAssets();
		shaderData.buffer.destroy();
	}

	void initFallbackAssets() {
		uint32_t width = 1024, height = 1024;
		std::vector<unsigned char> buffer(width * height * 4, 1);
		VkDeviceSize bufferSize = static_cast<VkDeviceSize>(buffer.size());
		fallbackTexAO.fromBuffer(buffer.data(), bufferSize, VK_FORMAT_R8G8B8A8_UNORM, width, height, vulkanDevice, queue);

		buffer.resize(width * height * 4, 0);
		fallbackTexEmissive.fromBuffer(buffer.data(), bufferSize, VK_FORMAT_R8G8B8A8_UNORM, width, height, vulkanDevice, queue);
	}

	void destroyFallbackAssets() {
		fallbackTexAO.destroy();
		fallbackTexEmissive.destroy();
	}

	virtual void getEnabledFeatures()
	{
		// Fill mode non solid is required for wireframe display
		if (deviceFeatures.fillModeNonSolid) {
			enabledFeatures.fillModeNonSolid = VK_TRUE;
		};
	}

	void buildCommandBuffers()
	{
		VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();

		VkClearValue clearValues[2];
		clearValues[0].color = defaultClearColor;
		clearValues[0].color = { { 0.25f, 0.25f, 0.25f, 1.0f } };;
		clearValues[1].depthStencil = { 1.0f, 0 };

		VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
		renderPassBeginInfo.renderPass = renderPass;
		renderPassBeginInfo.renderArea.offset.x = 0;
		renderPassBeginInfo.renderArea.offset.y = 0;
		renderPassBeginInfo.renderArea.extent.width = width;
		renderPassBeginInfo.renderArea.extent.height = height;
		renderPassBeginInfo.clearValueCount = 2;
		renderPassBeginInfo.pClearValues = clearValues;

		const VkViewport viewport = vks::initializers::viewport((float)width, (float)height, 0.0f, 1.0f);
		const VkRect2D scissor = vks::initializers::rect2D(width, height, 0, 0);

		for (int32_t i = 0; i < drawCmdBuffers.size(); ++i)
		{
			renderPassBeginInfo.framebuffer = frameBuffers[i];
			VK_CHECK_RESULT(vkBeginCommandBuffer(drawCmdBuffers[i], &cmdBufInfo));
			vkCmdBeginRenderPass(drawCmdBuffers[i], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);
			vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);
			// Bind scene matrices descriptor to set 0
			vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
			vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, wireframe ? pipelines.wireframe : pipelines.solid);
			glTFModel.draw(drawCmdBuffers[i], pipelineLayout);
			drawUI(drawCmdBuffers[i]);
			vkCmdEndRenderPass(drawCmdBuffers[i]);
			VK_CHECK_RESULT(vkEndCommandBuffer(drawCmdBuffers[i]));
		}
	}

	void loadglTFFile(std::string filename)
	{
		tinygltf::Model glTFInput;
		tinygltf::TinyGLTF gltfContext;
		std::string error, warning;

		this->device = device;

#if defined(__ANDROID__)
		// On Android all assets are packed with the apk in a compressed form, so we need to open them using the asset manager
		// We let tinygltf handle this, by passing the asset manager of our app
		tinygltf::asset_manager = androidApp->activity->assetManager;
#endif
		bool fileLoaded = gltfContext.LoadASCIIFromFile(&glTFInput, &error, &warning, filename);

		// Pass some Vulkan resources required for setup and rendering to the glTF model loading class
		glTFModel.vulkanDevice = vulkanDevice;
		glTFModel.copyQueue = queue;

		std::vector<uint32_t> indexBuffer;
		std::vector<VulkanglTFModel::Vertex> vertexBuffer;

		if (fileLoaded) {
			glTFModel.loadImages(glTFInput);
			glTFModel.loadMaterials(glTFInput);
			glTFModel.loadTextures(glTFInput);
			const tinygltf::Scene& scene = glTFInput.scenes[0];

			for (size_t i = 0; i < scene.nodes.size(); i++) {
				const tinygltf::Node node = glTFInput.nodes[scene.nodes[i]];
				auto nodeIdx = scene.nodes[i];

				glTFModel.loadNode(node, nodeIdx, glTFInput, nullptr, indexBuffer, vertexBuffer);
			}
			
		}
		else {
			vks::tools::exitFatal("Could not open the glTF file.\n\nThe file is part of the additional asset pack.\n\nRun \"download_assets.py\" in the repository root to download the latest version.", -1);
			return;
		}

		for (auto& anim : glTFInput.animations) {

			for (auto& channel : anim.channels) {

				const auto& sampler = anim.samplers[channel.sampler];

				const auto& input_accessor = glTFInput.accessors[sampler.input];
				const auto& output_accessor = glTFInput.accessors[sampler.output];

				const auto input_element_count = input_accessor.count;
				const auto input_element_float_count = tinygltf::GetNumComponentsInType(input_accessor.type);
				const auto input_byte_count = input_element_float_count * sizeof(float) * input_element_count;

				std::vector<float> _input_data_copy_(input_element_float_count * input_element_count);
				{
					auto& input_bufferView = glTFInput.bufferViews[input_accessor.bufferView];
					auto& input_buffer = glTFInput.buffers[input_bufferView.buffer];

					auto byte_offset = input_accessor.byteOffset + input_bufferView.byteOffset; 
					auto input_data_ptr = input_buffer.data.data();

					memcpy(_input_data_copy_.data(), input_data_ptr+byte_offset, input_byte_count);
				}
				
				const auto output_element_count = output_accessor.count;
				const auto output_element_float_count = tinygltf::GetNumComponentsInType(output_accessor.type);
				const auto output_byte_count = output_element_float_count * sizeof(float) * output_element_count;

				std::vector<float> _output_data_copy_(output_element_float_count * output_element_count); 
				{
					auto& output_bufferView = glTFInput.bufferViews[output_accessor.bufferView];
					auto& output_buffer = glTFInput.buffers[output_bufferView.buffer];

					auto byte_offset = output_accessor.byteOffset + output_bufferView.byteOffset; 
					auto output_data_ptr = output_buffer.data.data();

					memcpy(_output_data_copy_.data(), output_data_ptr+byte_offset, output_byte_count);
				}

				float input_min_value = (float)input_accessor.minValues[0];
				float input_max_value = (float)input_accessor.maxValues[0];

				auto run_animation = [=](float render_time) {

					float current_t = fmod(render_time, input_max_value);
					if (current_t < input_min_value) { current_t = input_min_value; }

					for(size_t i=0; i<_input_data_copy_.size()-1; ++i) {

						if (current_t >= _input_data_copy_[i] && current_t < _input_data_copy_[i+1]) {

							float delta = current_t - _input_data_copy_[i];
							float ratio = delta / (_input_data_copy_[i+1]-_input_data_copy_[i]);

							if (channel.target_path == "translation") {

								assert(output_element_float_count == 3);

								glm::vec3 previous_trans ( 
									_output_data_copy_[i * output_element_float_count ],
									_output_data_copy_[i * output_element_float_count + 1],
									_output_data_copy_[i * output_element_float_count + 2]
								);

								glm::vec3 next_trans (
									_output_data_copy_[(i+1) * output_element_float_count ],
									_output_data_copy_[(i+1) * output_element_float_count + 1],
									_output_data_copy_[(i+1) * output_element_float_count + 2]
								);

								glm::vec3 final_trans = glm::mix(previous_trans, next_trans, ratio);

								auto node_ptr = glTFModel.nodeFromIndex(channel.target_node);
								node_ptr->translation = final_trans;  

							} else if (channel.target_path == "scale") {

								assert(output_element_float_count == 3);

								glm::vec3 previous_scale ( 
									_output_data_copy_[i * output_element_float_count ],
									_output_data_copy_[i * output_element_float_count + 1],
									_output_data_copy_[i * output_element_float_count + 2]
								);

								glm::vec3 next_scale (
									_output_data_copy_[(i+1) * output_element_float_count ],
									_output_data_copy_[(i+1) * output_element_float_count + 1],
									_output_data_copy_[(i+1) * output_element_float_count + 2]
								);

								glm::vec3 final_scale = glm::mix(previous_scale, next_scale, ratio);

								auto node_ptr = glTFModel.nodeFromIndex(channel.target_node);
								node_ptr->scale = final_scale;

							} else if (channel.target_path == "rotation") {

								assert(output_element_float_count == 4);

								glm::quat q1;
									q1.x = _output_data_copy_[i * output_element_float_count ];
									q1.y = _output_data_copy_[i * output_element_float_count + 1];
									q1.z = _output_data_copy_[i * output_element_float_count + 2];
									q1.w = _output_data_copy_[i * output_element_float_count + 3];
								
								glm::quat q2;
									q2.x = _output_data_copy_[(i+1) * output_element_float_count ];
									q2.y = _output_data_copy_[(i+1) * output_element_float_count + 1];
									q2.z = _output_data_copy_[(i+1) * output_element_float_count + 2];
									q2.w = _output_data_copy_[(i+1) * output_element_float_count + 3];

								glm::quat q3 = glm::normalize(glm::slerp(q1, q2, ratio));

								auto node_ptr = glTFModel.nodeFromIndex(channel.target_node);
								node_ptr->rotation = q3;
							} 
							break;
						}
					}
				}; // run_animation
				
				glTFModel.animationCallbacks.push_back(
					run_animation
				);
			}
		}

		glTFModel.modelMatrices.contents.resize(glTFInput.nodes.size(), glm::mat4(1.0f));
		glTFModel.normalMatrices.contents.resize(glTFInput.nodes.size(), glm::mat4(1.0f));

		VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&glTFModel.modelMatrices.ssbo,
			glTFModel.modelMatrices.byte_length() ,
			glTFModel.modelMatrices.contents.data()));
		VK_CHECK_RESULT(glTFModel.modelMatrices.ssbo.map()); 

		VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&glTFModel.normalMatrices.ssbo,
			glTFModel.normalMatrices.byte_length() ,
			glTFModel.normalMatrices.contents.data()));
		VK_CHECK_RESULT(glTFModel.normalMatrices.ssbo.map()); 

		// Create and upload vertex and index buffer
		// We will be using one single vertex buffer and one single index buffer for the whole glTF scene
		// Primitives (of the glTF model) will then index into these using index offsets

		size_t vertexBufferSize = vertexBuffer.size() * sizeof(VulkanglTFModel::Vertex);
		size_t indexBufferSize = indexBuffer.size() * sizeof(uint32_t);
		glTFModel.indices.count = static_cast<uint32_t>(indexBuffer.size());

		struct StagingBuffer {
			VkBuffer buffer;
			VkDeviceMemory memory;
		} vertexStaging, indexStaging;

		// Create host visible staging buffers (source)
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			vertexBufferSize,
			&vertexStaging.buffer,
			&vertexStaging.memory,
			vertexBuffer.data()));
		// Index data
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			indexBufferSize,
			&indexStaging.buffer,
			&indexStaging.memory,
			indexBuffer.data()));

		// Create device local buffers (target)
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			vertexBufferSize,
			&glTFModel.vertices.buffer,
			&glTFModel.vertices.memory));
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			indexBufferSize,
			&glTFModel.indices.buffer,
			&glTFModel.indices.memory));

		// Copy data from staging buffers (host) do device local buffer (gpu)
		VkCommandBuffer copyCmd = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
		VkBufferCopy copyRegion = {};

		copyRegion.size = vertexBufferSize;
		vkCmdCopyBuffer(
			copyCmd,
			vertexStaging.buffer,
			glTFModel.vertices.buffer,
			1,
			&copyRegion);

		copyRegion.size = indexBufferSize;
		vkCmdCopyBuffer(
			copyCmd,
			indexStaging.buffer,
			glTFModel.indices.buffer,
			1,
			&copyRegion);

		vulkanDevice->flushCommandBuffer(copyCmd, queue, true);

		// Free staging resources
		vkDestroyBuffer(device, vertexStaging.buffer, nullptr);
		vkFreeMemory(device, vertexStaging.memory, nullptr);
		vkDestroyBuffer(device, indexStaging.buffer, nullptr);
		vkFreeMemory(device, indexStaging.memory, nullptr);
	}

	void loadAssets()
	{
		loadglTFFile(getAssetPath() + "buster_drone/busterDrone.gltf");
	}

	void setupDescriptors()
	{
		/*
			This sample uses separate descriptor sets (and layouts) for the matrices and materials (textures)
		*/

		std::vector<VkDescriptorPoolSize> poolSizes = {
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1),
			// One combined image sampler per model image/texture
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<uint32_t>(glTFModel.images.size() * 5u)),

			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2)
		};
		// One set for matrices and one per model image/texture
		const uint32_t maxSetCount = static_cast<uint32_t>(glTFModel.images.size()) + 1 + 2;
		VkDescriptorPoolCreateInfo descriptorPoolInfo = vks::initializers::descriptorPoolCreateInfo(poolSizes, maxSetCount);
		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));

		// Descriptor set layout for passing matrices
		VkDescriptorSetLayoutBinding setLayoutBinding = vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 0);
		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCI = vks::initializers::descriptorSetLayoutCreateInfo(&setLayoutBinding, 1);
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCI, nullptr, &descriptorSetLayouts.scene));

		// Descriptor set layout for passing material textures

		std::vector<VkDescriptorSetLayoutBinding> texSetLayoutBindings = {
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 0), //albedo
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 1), // normal
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 2), // metallic roughness
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 3), // AO
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 4), // emissive
		};

		descriptorSetLayoutCI = vks::initializers::descriptorSetLayoutCreateInfo(texSetLayoutBindings);
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCI, nullptr, &descriptorSetLayouts.textures));

		std::vector<VkDescriptorSetLayoutBinding> ssboSetLayoutBindings = {
			 vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 0),
			 vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 1)
		};

		descriptorSetLayoutCI = vks::initializers::descriptorSetLayoutCreateInfo(ssboSetLayoutBindings);
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCI, nullptr, &descriptorSetLayouts.nodeMatrices));

		std::vector<VkDescriptorSetLayout> setLayouts = { 
			descriptorSetLayouts.scene, 
			descriptorSetLayouts.textures,
			descriptorSetLayouts.nodeMatrices,
		};

		VkPipelineLayoutCreateInfo pipelineLayoutCI= vks::initializers::pipelineLayoutCreateInfo(setLayouts.data(), static_cast<uint32_t>(setLayouts.size()));
		
		VkPushConstantRange pushConstantRange = vks::initializers::pushConstantRange(VK_SHADER_STAGE_VERTEX_BIT, sizeof(uint32_t), 0);

		// Push constant ranges are part of the pipeline layout
		pipelineLayoutCI.pushConstantRangeCount = 1;
		pipelineLayoutCI.pPushConstantRanges = &pushConstantRange;
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCI, nullptr, &pipelineLayout));

		// Descriptor set for scene matrices
		VkDescriptorSetAllocateInfo allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayouts.scene, 1);
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet));
		VkWriteDescriptorSet writeDescriptorSet = vks::initializers::writeDescriptorSet(descriptorSet, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &shaderData.buffer.descriptor);
		vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);

		// Descriptor sets for materials
		allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayouts.textures, 1);

		for (auto& material : glTFModel.materials) {
			VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &material.descriptorSet));

			std::array<VkDescriptorImageInfo*, 5> descriptorImageInfos{nullptr};
			if (material.baseColorTextureIndex != -1) {
				descriptorImageInfos[0] = &glTFModel.images[material.baseColorTextureIndex].texture.descriptor;
			}
			if (material.normalTexureIndex != -1) {
				descriptorImageInfos[1] = &glTFModel.images[material.normalTexureIndex].texture.descriptor;
			}

			if (material.metallicRoughnessTextureIndex != -1) {
				descriptorImageInfos[2] = &glTFModel.images[material.metallicRoughnessTextureIndex].texture.descriptor;
			}

			if (material.occlusionTextureIndex != -1) {
				descriptorImageInfos[3] = &glTFModel.images[material.occlusionTextureIndex].texture.descriptor;
			}else {
				descriptorImageInfos[3] = &fallbackTexAO.descriptor;
			}

			if (material.emissiveTextureIndex != -1) {
				descriptorImageInfos[4] = &glTFModel.images[material.emissiveTextureIndex].texture.descriptor;
			} else {
				descriptorImageInfos[4] = &fallbackTexEmissive.descriptor;
			}

			//update each binding point about the material descri
			std::vector<VkWriteDescriptorSet> writeDescriptorSets;
			for (int i = 0; i < descriptorImageInfos.size(); ++i) {
				if (descriptorImageInfos[i] != nullptr) {
					VkWriteDescriptorSet w =  vks::initializers::writeDescriptorSet(material.descriptorSet, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, i, descriptorImageInfos[i],  1);
					writeDescriptorSets.emplace_back(w);
				}
			}

			vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(),0 , nullptr);
		}

		allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayouts.nodeMatrices, 1);
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &glTFModel.modelMatrices.descriptorSet));

		auto writeDescriptorSet0 = vks::initializers::writeDescriptorSet(glTFModel.modelMatrices.descriptorSet, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &glTFModel.modelMatrices.ssbo.descriptor);
		auto writeDescriptorSet1 = vks::initializers::writeDescriptorSet(glTFModel.modelMatrices.descriptorSet, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, &glTFModel.normalMatrices.ssbo.descriptor);
		
		std::vector<VkWriteDescriptorSet> ssboWriteDescriptorSets {
			writeDescriptorSet0, 
			writeDescriptorSet1
		};

		vkUpdateDescriptorSets(device, static_cast<uint32_t>(ssboWriteDescriptorSets.size()), ssboWriteDescriptorSets.data(), 0, NULL);
	}

	void preparePipelines()
	{
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCI = vks::initializers::pipelineInputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
		VkPipelineRasterizationStateCreateInfo rasterizationStateCI = vks::initializers::pipelineRasterizationStateCreateInfo(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0);

		auto colorMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

		VkPipelineColorBlendAttachmentState blendAttachmentStateCI = vks::initializers::pipelineColorBlendAttachmentState(colorMask, VK_TRUE);

		blendAttachmentStateCI.blendEnable = VK_TRUE;
		blendAttachmentStateCI.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		blendAttachmentStateCI.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blendAttachmentStateCI.colorBlendOp = VK_BLEND_OP_ADD;
		blendAttachmentStateCI.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		blendAttachmentStateCI.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blendAttachmentStateCI.alphaBlendOp = VK_BLEND_OP_ADD;

		VkPipelineColorBlendStateCreateInfo colorBlendStateCI = vks::initializers::pipelineColorBlendStateCreateInfo(1, &blendAttachmentStateCI);

		//colorBlendStateCI.logicOpEnable = VK_TRUE;
		//colorBlendStateCI.logicOp = VK_LOGIC_OP_COPY; // Optional

		VkPipelineDepthStencilStateCreateInfo depthStencilStateCI = vks::initializers::pipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS_OR_EQUAL);
		VkPipelineViewportStateCreateInfo viewportStateCI = vks::initializers::pipelineViewportStateCreateInfo(1, 1, 0);
		VkPipelineMultisampleStateCreateInfo multisampleStateCI = vks::initializers::pipelineMultisampleStateCreateInfo(VK_SAMPLE_COUNT_1_BIT, 0);
		const std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicStateCI = vks::initializers::pipelineDynamicStateCreateInfo(dynamicStateEnables.data(), static_cast<uint32_t>(dynamicStateEnables.size()), 0);
		// Vertex input bindings and attributes
		const std::vector<VkVertexInputBindingDescription> vertexInputBindings = {
			vks::initializers::vertexInputBindingDescription(0, sizeof(VulkanglTFModel::Vertex), VK_VERTEX_INPUT_RATE_VERTEX),
		};
		const std::vector<VkVertexInputAttributeDescription> vertexInputAttributes = {
			vks::initializers::vertexInputAttributeDescription(0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(VulkanglTFModel::Vertex, pos)),	// Location 0: Position
			vks::initializers::vertexInputAttributeDescription(0, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(VulkanglTFModel::Vertex, normal)), // Location 1: Normal
			vks::initializers::vertexInputAttributeDescription(0, 2, VK_FORMAT_R32G32_SFLOAT, offsetof(VulkanglTFModel::Vertex, uv)),	    // Location 2: UV 
			vks::initializers::vertexInputAttributeDescription(0, 3, VK_FORMAT_R32G32B32_SFLOAT, offsetof(VulkanglTFModel::Vertex, color)),	// Location 3: Color
			vks::initializers::vertexInputAttributeDescription(0, 4, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VulkanglTFModel::Vertex, tangent)) // Location 4 tangent
		};
		VkPipelineVertexInputStateCreateInfo vertexInputStateCI = vks::initializers::pipelineVertexInputStateCreateInfo();
		vertexInputStateCI.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexInputBindings.size());
		vertexInputStateCI.pVertexBindingDescriptions = vertexInputBindings.data();
		vertexInputStateCI.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexInputAttributes.size());
		vertexInputStateCI.pVertexAttributeDescriptions = vertexInputAttributes.data();

		const std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {
			loadShader(getHomeworkShadersPath() + "homework1/mesh.vert.spv", VK_SHADER_STAGE_VERTEX_BIT),
			loadShader(getHomeworkShadersPath() + "homework1/mesh.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT)
		};

		VkGraphicsPipelineCreateInfo pipelineCI = vks::initializers::pipelineCreateInfo(pipelineLayout, renderPass, 0);
		pipelineCI.pVertexInputState = &vertexInputStateCI;
		pipelineCI.pInputAssemblyState = &inputAssemblyStateCI;
		pipelineCI.pRasterizationState = &rasterizationStateCI;
		pipelineCI.pColorBlendState = &colorBlendStateCI;
		pipelineCI.pMultisampleState = &multisampleStateCI;
		pipelineCI.pViewportState = &viewportStateCI;
		pipelineCI.pDepthStencilState = &depthStencilStateCI;
		pipelineCI.pDynamicState = &dynamicStateCI;
		pipelineCI.stageCount = static_cast<uint32_t>(shaderStages.size());
		pipelineCI.pStages = shaderStages.data();

		// Solid rendering pipeline
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.solid));

		// Wire frame rendering pipeline
		if (deviceFeatures.fillModeNonSolid) {
			rasterizationStateCI.polygonMode = VK_POLYGON_MODE_LINE;
			rasterizationStateCI.lineWidth = 1.0f;
			VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.wireframe));
		}
	}

	// Prepare and initialize uniform buffer containing shader uniforms
	void prepareUniformBuffers()
	{
		// Vertex shader uniform buffer block
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&shaderData.buffer,
			sizeof(shaderData.values)));

		// Map persistent
		VK_CHECK_RESULT(shaderData.buffer.map());

		updateUniformBuffers();
	}

	void updateUniformBuffers()
	{
		shaderData.values.projection = camera.matrices.perspective;
		shaderData.values.model = camera.matrices.view;
		shaderData.values.viewPos = camera.viewPos;
		memcpy(shaderData.buffer.mapped, &shaderData.values, sizeof(shaderData.values));
	}

	void prepare()
	{
		VulkanExampleBase::prepare();
		loadAssets();
		initFallbackAssets();
		prepareUniformBuffers();
		setupDescriptors();
		preparePipelines();
		buildCommandBuffers();
		prepared = true;
	}

	virtual void render()
	{
		const static auto launch_time = std::chrono::system_clock::now(); 

		auto current_time = std::chrono::system_clock::now();
		std::chrono::duration<float> elapsed_seconds = current_time - launch_time;
		auto delta = elapsed_seconds.count();

		auto& animationCallbacks = glTFModel.animationCallbacks;
		auto& modelMatrices = glTFModel.modelMatrices;
		auto& normalMatrice = glTFModel.normalMatrices;

		auto thread_count = std::thread::hardware_concurrency();
		auto task_count = animationCallbacks.size();

		auto bin_size = task_count / thread_count;
		auto bin_rema = task_count % thread_count;
		
		std::vector<size_t> bins(thread_count, bin_size);
		bins.back() += bin_rema;

		for(size_t i=0; i<bins.size(); ++i) {
			
			//std::thread([=]{
				for(size_t j=0; j<bins[i]; ++j) {
					size_t idx = bin_size * i + j;
					animationCallbacks[idx](delta);
				}
			//}).join();
		}

		for (auto& node : glTFModel.nodes) {
			glTFModel.diveNode(node);
		}

		modelMatrices.ssbo.copyTo(modelMatrices.contents.data(), modelMatrices.byte_length());
		normalMatrice.ssbo.copyTo(normalMatrice.contents.data(), normalMatrice.byte_length());

		renderFrame();
		if (camera.updated) {
			updateUniformBuffers();
		}
	}

	virtual void viewChanged()
	{
		updateUniformBuffers();
	}

	virtual void OnUpdateUIOverlay(vks::UIOverlay *overlay)
	{
		if (overlay->header("Settings")) {
			if (overlay->checkBox("Wireframe", &wireframe)) {
				buildCommandBuffers();
			}
		}
	}
};

VULKAN_EXAMPLE_MAIN()
