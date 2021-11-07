
#pragma once

#include "vulkandevice.h"
#include "vulkanobjects.h"

class LevelMesh;

struct Uniforms
{
	Vec3 LightOrigin;
	float PassType;
	float LightRadius;
	float LightIntensity;
	float LightInnerAngleCos;
	float LightOuterAngleCos;
	Vec3 LightSpotDir;
	float SampleDistance;
	Vec3 LightColor;
	float Padding;
};

struct SurfaceInfo
{
	Vec3 Normal;
	float EmissiveDistance;
	Vec3 EmissiveColor;
	float EmissiveIntensity;
	float Sky;
	float Padding0, Padding1, Padding2;
};

struct SurfaceTask
{
	int surf, x, y;
};

class GPURaytracer
{
public:
	GPURaytracer();
	~GPURaytracer();

	void Raytrace(LevelMesh* level);

private:
	void CreateVulkanObjects();
	void CreateVertexAndIndexBuffers();
	void CreateBottomLevelAccelerationStructure();
	void CreateTopLevelAccelerationStructure();
	void CreateShaders();
	std::unique_ptr<VulkanShader> CompileRayGenShader(const char* code, const char* name);
	std::unique_ptr<VulkanShader> CompileClosestHitShader(const char* code, const char* name);
	std::unique_ptr<VulkanShader> CompileMissShader(const char* code, const char* name);
	void CreatePipeline();
	void CreateDescriptorSet();

	void UploadTasks(const std::vector<SurfaceTask>& tasks);
	void RunTrace(const Uniforms& uniforms, const VkStridedDeviceAddressRegionKHR& rgenShader);
	void DownloadTasks(const std::vector<SurfaceTask>& tasks);

	void PrintVulkanInfo();

	void RaytraceProbeSample(LightProbeSample* probe);
	void RaytraceSurfaceSample(Surface* surface, int x, int y);
	Vec3 TracePath(const Vec3& pos, const Vec3& dir, int sampleIndex, int depth = 0);

	Vec3 GetLightEmittance(Surface* surface, const Vec3& pos);
	Vec3 GetSurfaceEmittance(Surface* surface, float distance);

	static float RadicalInverse_VdC(uint32_t bits);
	static Vec2 Hammersley(uint32_t i, uint32_t N);
	static Vec3 ImportanceSampleGGX(Vec2 Xi, Vec3 N, float roughness);

	int SAMPLE_COUNT = 1024;

	LevelMesh* mesh = nullptr;

	std::unique_ptr<VulkanDevice> device;

	std::unique_ptr<VulkanBuffer> vertexBuffer;
	std::unique_ptr<VulkanBuffer> indexBuffer;
	std::unique_ptr<VulkanBuffer> transferBuffer;
	std::unique_ptr<VulkanBuffer> surfaceIndexBuffer;
	std::unique_ptr<VulkanBuffer> surfaceBuffer;

	std::unique_ptr<VulkanBuffer> blScratchBuffer;
	std::unique_ptr<VulkanBuffer> blAccelStructBuffer;
	std::unique_ptr<VulkanAccelerationStructure> blAccelStruct;

	std::unique_ptr<VulkanBuffer> tlTransferBuffer;
	std::unique_ptr<VulkanBuffer> tlScratchBuffer;
	std::unique_ptr<VulkanBuffer> tlInstanceBuffer;
	std::unique_ptr<VulkanBuffer> tlAccelStructBuffer;
	std::unique_ptr<VulkanAccelerationStructure> tlAccelStruct;

	std::unique_ptr<VulkanShader> rgenBounce, rgenLight, rgenSun;
	std::unique_ptr<VulkanShader> rmissBounce, rmissLight, rmissSun;
	std::unique_ptr<VulkanShader> rchitBounce, rchitLight, rchitSun;

	std::unique_ptr<VulkanDescriptorSetLayout> descriptorSetLayout;

	std::unique_ptr<VulkanPipelineLayout> pipelineLayout;
	std::unique_ptr<VulkanPipeline> pipeline;
	std::unique_ptr<VulkanBuffer> shaderBindingTable;
	std::unique_ptr<VulkanBuffer> sbtTransferBuffer;

	VkStridedDeviceAddressRegionKHR rgenBounceRegion = {}, rgenLightRegion = {}, rgenSunRegion = {};
	VkStridedDeviceAddressRegionKHR missRegion = {};
	VkStridedDeviceAddressRegionKHR hitRegion = {};
	VkStridedDeviceAddressRegionKHR callRegion = {};

	std::unique_ptr<VulkanImage> positionsImage, normalsImage, outputImage;
	std::unique_ptr<VulkanImageView> positionsImageView, normalsImageView, outputImageView;
	std::unique_ptr<VulkanBuffer> imageTransferBuffer;

	std::unique_ptr<VulkanBuffer> uniformBuffer;
	std::unique_ptr<VulkanBuffer> uniformTransferBuffer;

	std::unique_ptr<VulkanDescriptorPool> descriptorPool;
	std::unique_ptr<VulkanDescriptorSet> descriptorSet;

	std::unique_ptr<VulkanCommandPool> cmdpool;
	std::unique_ptr<VulkanCommandBuffer> cmdbuffer;

	int rayTraceImageSize = 2048;
};
