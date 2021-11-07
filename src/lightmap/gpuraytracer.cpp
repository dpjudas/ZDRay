
#include "math/mathlib.h"
#include "surfaces.h"
#include "level/level.h"
#include "gpuraytracer.h"
#include "worker.h"
#include "framework/binfile.h"
#include "framework/templates.h"
#include "framework/halffloat.h"
#include "vulkanbuilders.h"
#include <map>
#include <vector>
#include <algorithm>
#include <zlib.h>
#include "glsl_rgen_bounce.h"
#include "glsl_rgen_light.h"
#include "glsl_rgen_sun.h"
#include "glsl_rchit_bounce.h"
#include "glsl_rchit_light.h"
#include "glsl_rchit_sun.h"
#include "glsl_rmiss_bounce.h"
#include "glsl_rmiss_light.h"
#include "glsl_rmiss_sun.h"

extern int LightBounce;
extern bool VKDebug;

GPURaytracer::GPURaytracer()
{
	device = std::make_unique<VulkanDevice>(0, VKDebug);
	PrintVulkanInfo();
}

GPURaytracer::~GPURaytracer()
{
}

void GPURaytracer::Raytrace(LevelMesh* level)
{
	mesh = level;

	printf("Building vulkan acceleration structures\n");

	if (device->renderdoc)
		device->renderdoc->StartFrameCapture(0, 0);

	CreateVulkanObjects();

	printf("Tracing light probes\n");

	Worker::RunJob((int)mesh->lightProbes.size(), [=](int id) {
		RaytraceProbeSample(&mesh->lightProbes[id]);
	});

	printf("Tracing surfaces (%d bounces)\n", LightBounce);

	std::vector<SurfaceTask> tasks;

	for (size_t i = 0; i < mesh->surfaces.size(); i++)
	{
		Surface* surface = mesh->surfaces[i].get();
		int sampleWidth = surface->lightmapDims[0];
		int sampleHeight = surface->lightmapDims[1];
		for (int y = 0; y < sampleHeight; y++)
		{
			for (int x = 0; x < sampleWidth; x++)
			{
				SurfaceTask task;
				task.surf = (int)i;
				task.x = x;
				task.y = y;
				tasks.push_back(task);
			}
		}
	}

	UploadTasks(tasks);

	// Sunlight
	{
		Uniforms uniforms = {};
		uniforms.LightOrigin = Vec3(0.0f, 0.0f, 0.0f);
		uniforms.LightRadius = -1.0f;
		uniforms.LightIntensity = 1.0f;
		uniforms.LightInnerAngleCos = -1.0f;
		uniforms.LightOuterAngleCos = -1.0f;
		uniforms.LightSpotDir = mesh->map->GetSunDirection();
		uniforms.LightColor = mesh->map->GetSunColor();
		uniforms.PassType = 0.0f;
		uniforms.SampleDistance = (float)mesh->samples;
		RunTrace(uniforms, rgenSunRegion);
	}

	for (ThingLight& light : mesh->map->ThingLights)
	{
		Uniforms uniforms = {};
		uniforms.LightOrigin = light.LightOrigin();
		uniforms.LightRadius = light.LightRadius();
		uniforms.LightIntensity = light.intensity;
		uniforms.LightInnerAngleCos = light.innerAngleCos;
		uniforms.LightOuterAngleCos = light.outerAngleCos;
		uniforms.LightSpotDir = light.SpotDir();
		uniforms.LightColor = light.rgb;
		uniforms.PassType = 1.0f;
		uniforms.SampleDistance = (float)mesh->samples;
		RunTrace(uniforms, rgenLightRegion);
	}

	DownloadTasks(tasks);

	if (device->renderdoc)
		device->renderdoc->EndFrameCapture(0, 0);

	printf("Raytrace complete\n");
}

void GPURaytracer::CreateVulkanObjects()
{
	cmdpool = std::make_unique<VulkanCommandPool>(device.get(), device->graphicsFamily);
	cmdbuffer = cmdpool->createBuffer();
	cmdbuffer->begin();

	CreateVertexAndIndexBuffers();
	CreateBottomLevelAccelerationStructure();
	CreateTopLevelAccelerationStructure();
	CreateShaders();
	CreatePipeline();
	CreateDescriptorSet();

	PipelineBarrier finishbuildbarrier;
	finishbuildbarrier.addMemory(VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
	finishbuildbarrier.execute(cmdbuffer.get(), VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
}

void GPURaytracer::UploadTasks(const std::vector<SurfaceTask>& tasks)
{
	size_t maxTasks = (size_t)rayTraceImageSize * rayTraceImageSize;
	if (tasks.size() > maxTasks)
		throw std::runtime_error("Ray trace task count is too large");

	size_t imageSize = sizeof(Vec4) * rayTraceImageSize * rayTraceImageSize;
	uint8_t* imageData = (uint8_t*)imageTransferBuffer->Map(0, imageSize * 2);
	Vec4* positions = (Vec4*)imageData;
	Vec4* normals = (Vec4*)(imageData + imageSize);
	for (size_t i = 0; i < tasks.size(); i++)
	{
		const SurfaceTask& task = tasks[i];
		Surface* surface = mesh->surfaces[task.surf].get();

		Vec3 normal = surface->plane.Normal();
		Vec3 pos = surface->lightmapOrigin + normal + surface->lightmapSteps[0] * (float)task.x + surface->lightmapSteps[1] * (float)task.y;

		positions[i] = Vec4(pos, 1.0f);
		normals[i] = Vec4(normal, 1.0f);
	}
	for (size_t i = tasks.size(); i < maxTasks; i++)
	{
		positions[i] = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
		normals[i] = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
	}
	imageTransferBuffer->Unmap();

	PipelineBarrier barrier1;
	barrier1.addImage(positionsImage.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
	barrier1.addImage(normalsImage.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
	barrier1.execute(cmdbuffer.get(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkBufferImageCopy region = {};
	region.bufferOffset = 0;
	region.imageExtent.width = rayTraceImageSize;
	region.imageExtent.height = rayTraceImageSize;
	region.imageExtent.depth = 1;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	cmdbuffer->copyBufferToImage(imageTransferBuffer->buffer, positionsImage->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	region = {};
	region.bufferOffset = imageSize;
	region.imageExtent.width = rayTraceImageSize;
	region.imageExtent.height = rayTraceImageSize;
	region.imageExtent.depth = 1;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	cmdbuffer->copyBufferToImage(imageTransferBuffer->buffer, normalsImage->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	PipelineBarrier barrier2;
	barrier2.addBuffer(uniformBuffer.get(), VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
	barrier2.addImage(positionsImage.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
	barrier2.addImage(normalsImage.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
	barrier2.addImage(outputImage.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
	barrier2.execute(cmdbuffer.get(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
}

void GPURaytracer::RunTrace(const Uniforms& uniforms, const VkStridedDeviceAddressRegionKHR& rgenShader)
{
	auto data = uniformTransferBuffer->Map(0, sizeof(Uniforms));
	memcpy(data, &uniforms, sizeof(Uniforms));
	uniformTransferBuffer->Unmap();

	cmdbuffer->copyBuffer(uniformTransferBuffer.get(), uniformBuffer.get());

	PipelineBarrier barrier3;
	barrier3.addBuffer(uniformBuffer.get(), VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
	barrier3.execute(cmdbuffer.get(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);

	cmdbuffer->bindPipeline(VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline.get());
	cmdbuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipelineLayout.get(), 0, descriptorSet.get());
	cmdbuffer->traceRays(&rgenShader, &missRegion, &hitRegion, &callRegion, rayTraceImageSize, rayTraceImageSize, 1);
	cmdbuffer->end();

	auto submitFence = std::make_unique<VulkanFence>(device.get());

	QueueSubmit submit;
	submit.addCommandBuffer(cmdbuffer.get());
	submit.execute(device.get(), device->graphicsQueue, submitFence.get());

	vkWaitForFences(device->device, 1, &submitFence->fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
	vkResetFences(device->device, 1, &submitFence->fence);

	printf(".");

	cmdbuffer.reset();
	cmdbuffer = cmdpool->createBuffer();
	cmdbuffer->begin();
}

void GPURaytracer::DownloadTasks(const std::vector<SurfaceTask>& tasks)
{
	PipelineBarrier barrier4;
	barrier4.addImage(outputImage.get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	barrier4.execute(cmdbuffer.get(), VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkBufferImageCopy region = {};
	region.bufferOffset = 0;
	region.imageExtent.width = rayTraceImageSize;
	region.imageExtent.height = rayTraceImageSize;
	region.imageExtent.depth = 1;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	cmdbuffer->copyImageToBuffer(outputImage->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imageTransferBuffer->buffer, 1, &region);
	cmdbuffer->end();

	auto submitFence = std::make_unique<VulkanFence>(device.get());

	QueueSubmit submit;
	submit.addCommandBuffer(cmdbuffer.get());
	submit.execute(device.get(), device->graphicsQueue, submitFence.get());

	vkWaitForFences(device->device, 1, &submitFence->fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
	vkResetFences(device->device, 1, &submitFence->fence);

	size_t imageSize = sizeof(Vec4) * rayTraceImageSize * rayTraceImageSize;
	uint8_t* imageData = (uint8_t*)imageTransferBuffer->Map(0, imageSize);
	Vec4* output = (Vec4*)imageData;
	for (size_t i = 0; i < tasks.size(); i++)
	{
		const SurfaceTask& task = tasks[i];
		Surface* surface = mesh->surfaces[task.surf].get();

		size_t sampleWidth = surface->lightmapDims[0];
		surface->samples[task.x + task.y * sampleWidth] = Vec3(output[i].x, output[i].y, output[i].z);
	}
	imageTransferBuffer->Unmap();

	printf("\n");
}

void GPURaytracer::CreateVertexAndIndexBuffers()
{
	std::vector<SurfaceInfo> surfaces;
	surfaces.reserve(mesh->surfaces.size());
	for (const auto& surface : mesh->surfaces)
	{
		SurfaceLightDef* def = nullptr;
		if (surface->type >= ST_MIDDLESIDE && surface->type <= ST_LOWERSIDE)
		{
			int lightdefidx = mesh->map->Sides[surface->typeIndex].lightdef;
			if (lightdefidx != -1)
			{
				def = &mesh->map->SurfaceLights[lightdefidx];
			}
		}
		else if (surface->type == ST_FLOOR || surface->type == ST_CEILING)
		{
			MapSubsectorEx* sub = &mesh->map->GLSubsectors[surface->typeIndex];
			IntSector* sector = mesh->map->GetSectorFromSubSector(sub);

			if (sector && surface->numVerts > 0)
			{
				if (sector->floorlightdef != -1 && surface->type == ST_FLOOR)
				{
					def = &mesh->map->SurfaceLights[sector->floorlightdef];
				}
				else if (sector->ceilinglightdef != -1 && surface->type == ST_CEILING)
				{
					def = &mesh->map->SurfaceLights[sector->ceilinglightdef];
				}
			}
		}

		SurfaceInfo info;
		info.Sky = surface->bSky ? 1.0f : 0.0f;
		info.Normal = surface->plane.Normal();
		if (def)
		{
			info.EmissiveDistance = def->distance;
			info.EmissiveIntensity = def->intensity;
			info.EmissiveColor = def->rgb;
		}
		else
		{
			info.EmissiveDistance = 0.0f;
			info.EmissiveIntensity = 0.0f;
			info.EmissiveColor = Vec3(0.0f, 0.0f, 0.0f);
		}
		surfaces.push_back(info);
	}

	size_t vertexbuffersize = (size_t)mesh->MeshVertices.Size() * sizeof(Vec3);
	size_t indexbuffersize = (size_t)mesh->MeshElements.Size() * sizeof(uint32_t);
	size_t surfaceindexbuffersize = (size_t)mesh->MeshSurfaces.Size() * sizeof(uint32_t);
	size_t surfacebuffersize = (size_t)surfaces.size() * sizeof(SurfaceInfo);
	size_t transferbuffersize = vertexbuffersize + indexbuffersize + surfaceindexbuffersize + surfacebuffersize;
	size_t vertexoffset = 0;
	size_t indexoffset = vertexoffset + vertexbuffersize;
	size_t surfaceindexoffset = indexoffset + indexbuffersize;
	size_t surfaceoffset = surfaceindexoffset + surfaceindexbuffersize;

	BufferBuilder vbuilder;
	vbuilder.setUsage(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	vbuilder.setSize(vertexbuffersize);
	vertexBuffer = vbuilder.create(device.get());
	vertexBuffer->SetDebugName("vertexBuffer");

	BufferBuilder ibuilder;
	ibuilder.setUsage(VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	ibuilder.setSize(indexbuffersize);
	indexBuffer = ibuilder.create(device.get());
	indexBuffer->SetDebugName("indexBuffer");

	BufferBuilder sibuilder;
	sibuilder.setUsage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	sibuilder.setSize(surfaceindexbuffersize);
	surfaceIndexBuffer = sibuilder.create(device.get());
	surfaceIndexBuffer->SetDebugName("surfaceIndexBuffer");

	BufferBuilder sbuilder;
	sbuilder.setUsage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	sbuilder.setSize(surfacebuffersize);
	surfaceBuffer = sbuilder.create(device.get());
	surfaceBuffer->SetDebugName("surfaceBuffer");

	BufferBuilder tbuilder;
	tbuilder.setUsage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	tbuilder.setSize(transferbuffersize);
	transferBuffer = tbuilder.create(device.get());
	transferBuffer->SetDebugName("transferBuffer");
	uint8_t* data = (uint8_t*)transferBuffer->Map(0, transferbuffersize);
	memcpy(data + vertexoffset, mesh->MeshVertices.Data(), vertexbuffersize);
	memcpy(data + indexoffset, mesh->MeshElements.Data(), indexbuffersize);
	memcpy(data + surfaceindexoffset, mesh->MeshSurfaces.Data(), surfaceindexbuffersize);
	memcpy(data + surfaceoffset, surfaces.data(), surfacebuffersize);
	transferBuffer->Unmap();

	cmdbuffer->copyBuffer(transferBuffer.get(), vertexBuffer.get(), vertexoffset);
	cmdbuffer->copyBuffer(transferBuffer.get(), indexBuffer.get(), indexoffset);
	cmdbuffer->copyBuffer(transferBuffer.get(), surfaceIndexBuffer.get(), surfaceindexoffset);
	cmdbuffer->copyBuffer(transferBuffer.get(), surfaceBuffer.get(), surfaceoffset);

	VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	cmdbuffer->pipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void GPURaytracer::CreateBottomLevelAccelerationStructure()
{
	VkBufferDeviceAddressInfo info = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
	info.buffer = vertexBuffer->buffer;
	VkDeviceAddress vertexAddress = vkGetBufferDeviceAddress(device->device, &info);

	info = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
	info.buffer = indexBuffer->buffer;
	VkDeviceAddress indexAddress = vkGetBufferDeviceAddress(device->device, &info);

	VkAccelerationStructureGeometryTrianglesDataKHR triangles = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR };
	triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	triangles.vertexData.deviceAddress = vertexAddress;
	triangles.vertexStride = sizeof(Vec3);
	triangles.indexType = VK_INDEX_TYPE_UINT32;
	triangles.indexData.deviceAddress = indexAddress;
	triangles.maxVertex = mesh->MeshVertices.Size();

	VkAccelerationStructureGeometryKHR accelStructBLDesc = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	accelStructBLDesc.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	accelStructBLDesc.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	accelStructBLDesc.geometry.triangles = triangles;

	VkAccelerationStructureBuildRangeInfoKHR rangeInfo = {};
	rangeInfo.primitiveCount = mesh->MeshElements.Size() / 3;

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.flags = accelStructBLDesc.flags | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &accelStructBLDesc;

	uint32_t maxPrimitiveCount = rangeInfo.primitiveCount;

	VkAccelerationStructureBuildSizesInfoKHR sizeInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	vkGetAccelerationStructureBuildSizesKHR(device->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &maxPrimitiveCount, &sizeInfo);

	BufferBuilder blbufbuilder;
	blbufbuilder.setUsage(VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
	blbufbuilder.setSize(sizeInfo.accelerationStructureSize);
	blAccelStructBuffer = blbufbuilder.create(device.get());
	blAccelStructBuffer->SetDebugName("blAccelStructBuffer");

	VkAccelerationStructureKHR blAccelStructHandle = {};
	VkAccelerationStructureCreateInfoKHR createInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
	createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	createInfo.buffer = blAccelStructBuffer->buffer;
	createInfo.size = sizeInfo.accelerationStructureSize;
	VkResult result = vkCreateAccelerationStructureKHR(device->device, &createInfo, nullptr, &blAccelStructHandle);
	if (result != VK_SUCCESS)
		throw std::runtime_error("vkCreateAccelerationStructureKHR failed");
	blAccelStruct = std::make_unique<VulkanAccelerationStructure>(device.get(), blAccelStructHandle);

	BufferBuilder sbuilder;
	sbuilder.setUsage(VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	sbuilder.setSize(sizeInfo.buildScratchSize);
	blScratchBuffer = sbuilder.create(device.get());
	blScratchBuffer->SetDebugName("blScratchBuffer");

	info = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
	info.buffer = blScratchBuffer->buffer;
	VkDeviceAddress scratchAddress = vkGetBufferDeviceAddress(device->device, &info);

	buildInfo.dstAccelerationStructure = blAccelStruct->accelstruct;
	buildInfo.scratchData.deviceAddress = scratchAddress;
	VkAccelerationStructureBuildRangeInfoKHR* rangeInfos[] = { &rangeInfo };
	cmdbuffer->buildAccelerationStructures(1, &buildInfo, rangeInfos);
}

void GPURaytracer::CreateTopLevelAccelerationStructure()
{
	VkAccelerationStructureDeviceAddressInfoKHR addressInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
	addressInfo.accelerationStructure = blAccelStruct->accelstruct;
	VkDeviceAddress blAccelStructAddress = vkGetAccelerationStructureDeviceAddressKHR(device->device, &addressInfo);

	VkAccelerationStructureInstanceKHR instance = {};
	instance.transform.matrix[0][0] = 1.0f;
	instance.transform.matrix[1][1] = 1.0f;
	instance.transform.matrix[2][2] = 1.0f;
	instance.instanceCustomIndex = 0;
	instance.accelerationStructureReference = blAccelStructAddress;
	instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
	instance.mask = 0xff;
	instance.instanceShaderBindingTableRecordOffset = 0;

	BufferBuilder tbuilder;
	tbuilder.setUsage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	tbuilder.setSize(sizeof(VkAccelerationStructureInstanceKHR));
	tlTransferBuffer = tbuilder.create(device.get());
	tlTransferBuffer->SetDebugName("tlTransferBuffer");
	auto data = (uint8_t*)tlTransferBuffer->Map(0, sizeof(VkAccelerationStructureInstanceKHR));
	memcpy(data, &instance, sizeof(VkAccelerationStructureInstanceKHR));
	tlTransferBuffer->Unmap();

	BufferBuilder instbufbuilder;
	instbufbuilder.setUsage(VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	instbufbuilder.setSize(sizeof(VkAccelerationStructureInstanceKHR));
	tlInstanceBuffer = instbufbuilder.create(device.get());
	tlInstanceBuffer->SetDebugName("tlInstanceBuffer");

	cmdbuffer->copyBuffer(tlTransferBuffer.get(), tlInstanceBuffer.get());

	VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	cmdbuffer->pipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, nullptr, 0, nullptr);

	VkBufferDeviceAddressInfo info = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
	info.buffer = tlInstanceBuffer->buffer;
	VkDeviceAddress instanceBufferAddress = vkGetBufferDeviceAddress(device->device, &info);

	VkAccelerationStructureGeometryInstancesDataKHR instances = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR };
	instances.data.deviceAddress = instanceBufferAddress;

	VkAccelerationStructureGeometryKHR accelStructTLDesc = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	accelStructTLDesc.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	accelStructTLDesc.geometry.instances = instances;

	VkAccelerationStructureBuildRangeInfoKHR rangeInfo = {};
	rangeInfo.primitiveCount = 1;

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &accelStructTLDesc;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	buildInfo.srcAccelerationStructure = VK_NULL_HANDLE;

	uint32_t maxInstanceCount = 1;

	VkAccelerationStructureBuildSizesInfoKHR sizeInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	vkGetAccelerationStructureBuildSizesKHR(device->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &maxInstanceCount, &sizeInfo);

	BufferBuilder tlbufbuilder;
	tlbufbuilder.setUsage(VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
	tlbufbuilder.setSize(sizeInfo.accelerationStructureSize);
	tlAccelStructBuffer = tlbufbuilder.create(device.get());
	tlAccelStructBuffer->SetDebugName("tlAccelStructBuffer");

	VkAccelerationStructureKHR tlAccelStructHandle = {};
	VkAccelerationStructureCreateInfoKHR createInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
	createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	createInfo.buffer = tlAccelStructBuffer->buffer;
	createInfo.size = sizeInfo.accelerationStructureSize;
	VkResult result = vkCreateAccelerationStructureKHR(device->device, &createInfo, nullptr, &tlAccelStructHandle);
	if (result != VK_SUCCESS)
		throw std::runtime_error("vkCreateAccelerationStructureKHR failed");
	tlAccelStruct = std::make_unique<VulkanAccelerationStructure>(device.get(), tlAccelStructHandle);

	BufferBuilder sbuilder;
	sbuilder.setUsage(VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	sbuilder.setSize(sizeInfo.buildScratchSize);
	tlScratchBuffer = sbuilder.create(device.get());
	tlScratchBuffer->SetDebugName("tlScratchBuffer");

	info = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
	info.buffer = tlScratchBuffer->buffer;
	VkDeviceAddress scratchAddress = vkGetBufferDeviceAddress(device->device, &info);

	buildInfo.dstAccelerationStructure = tlAccelStruct->accelstruct;
	buildInfo.scratchData.deviceAddress = scratchAddress;

	VkAccelerationStructureBuildRangeInfoKHR* rangeInfos[] = { &rangeInfo };
	cmdbuffer->buildAccelerationStructures(1, &buildInfo, rangeInfos);
}

void GPURaytracer::CreateShaders()
{
	rgenBounce = CompileRayGenShader(glsl_rgen_bounce, "rgen.bounce");
	rgenLight = CompileRayGenShader(glsl_rgen_light, "rgen.light");
	rgenSun = CompileRayGenShader(glsl_rgen_sun, "rgen.sun");
	rchitBounce = CompileClosestHitShader(glsl_rchit_bounce, "rchit.bounce");
	rchitLight = CompileClosestHitShader(glsl_rchit_light, "rchit.light");
	rchitSun = CompileClosestHitShader(glsl_rchit_sun, "rchit.sun");
	rmissBounce = CompileMissShader(glsl_rmiss_bounce, "rmiss.bounce");
	rmissLight = CompileMissShader(glsl_rmiss_light, "rmiss.light");
	rmissSun = CompileMissShader(glsl_rmiss_sun, "rmiss.sun");
}

std::unique_ptr<VulkanShader> GPURaytracer::CompileRayGenShader(const char* code, const char* name)
{
	try
	{
		ShaderBuilder builder;
		builder.setRayGenShader(code);
		auto shader = builder.create(device.get());
		shader->SetDebugName(name);
		return shader;
	}
	catch (const std::exception& e)
	{
		throw std::runtime_error(std::string("Could not compile ") + name + ": " + e.what());
	}
}

std::unique_ptr<VulkanShader> GPURaytracer::CompileClosestHitShader(const char* code, const char* name)
{
	try
	{
		ShaderBuilder builder;
		builder.setClosestHitShader(code);
		auto shader = builder.create(device.get());
		shader->SetDebugName(name);
		return shader;
	}
	catch (const std::exception& e)
	{
		throw std::runtime_error(std::string("Could not compile ") + name + ": " + e.what());
	}
}

std::unique_ptr<VulkanShader> GPURaytracer::CompileMissShader(const char* code, const char* name)
{
	try
	{
		ShaderBuilder builder;
		builder.setMissShader(code);
		auto shader = builder.create(device.get());
		shader->SetDebugName(name);
		return shader;
	}
	catch (const std::exception& e)
	{
		throw std::runtime_error(std::string("Could not compile ") + name + ": " + e.what());
	}
}

void GPURaytracer::CreatePipeline()
{
	DescriptorSetLayoutBuilder setbuilder;
	setbuilder.addBinding(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
	setbuilder.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
	setbuilder.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
	setbuilder.addBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
	setbuilder.addBinding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
	setbuilder.addBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
	setbuilder.addBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
	descriptorSetLayout = setbuilder.create(device.get());
	descriptorSetLayout->SetDebugName("descriptorSetLayout");

	PipelineLayoutBuilder layoutbuilder;
	layoutbuilder.addSetLayout(descriptorSetLayout.get());
	pipelineLayout = layoutbuilder.create(device.get());
	pipelineLayout->SetDebugName("pipelineLayout");

	RayTracingPipelineBuilder builder;
	builder.setLayout(pipelineLayout.get());
	builder.setMaxPipelineRayRecursionDepth(1);
	builder.addShader(VK_SHADER_STAGE_RAYGEN_BIT_KHR, rgenBounce.get());
	builder.addShader(VK_SHADER_STAGE_RAYGEN_BIT_KHR, rgenLight.get());
	builder.addShader(VK_SHADER_STAGE_RAYGEN_BIT_KHR, rgenSun.get());
	builder.addShader(VK_SHADER_STAGE_MISS_BIT_KHR, rmissBounce.get());
	builder.addShader(VK_SHADER_STAGE_MISS_BIT_KHR, rmissLight.get());
	builder.addShader(VK_SHADER_STAGE_MISS_BIT_KHR, rmissSun.get());
	builder.addShader(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, rchitBounce.get());
	builder.addShader(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, rchitLight.get());
	builder.addShader(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, rchitSun.get());
	builder.addRayGenGroup(0);
	builder.addRayGenGroup(1);
	builder.addRayGenGroup(2);
	builder.addMissGroup(3);
	builder.addMissGroup(4);
	builder.addMissGroup(5);
	builder.addTrianglesHitGroup(6);
	builder.addTrianglesHitGroup(7);
	builder.addTrianglesHitGroup(8);
	pipeline = builder.create(device.get());
	pipeline->SetDebugName("pipeline");

	const auto& rtProperties = device->physicalDevice.rayTracingProperties;

	auto align_up = [](VkDeviceSize value, VkDeviceSize alignment)
	{
		if (alignment != 0)
			return (value + alignment - 1) / alignment * alignment;
		else
			return value;
	};

	VkDeviceSize raygenCount = 3;
	VkDeviceSize missCount = 3;
	VkDeviceSize hitCount = 3;

	VkDeviceSize handleSize = rtProperties.shaderGroupHandleSize;
	VkDeviceSize handleSizeAligned = align_up(handleSize, rtProperties.shaderGroupHandleAlignment);

	VkDeviceSize rgenStride = align_up(handleSizeAligned, rtProperties.shaderGroupBaseAlignment);
	VkDeviceSize rgenSize = rgenStride * raygenCount;

	rgenBounceRegion.stride = rgenStride;
	rgenBounceRegion.size = rgenStride;
	rgenLightRegion.stride = rgenStride;
	rgenLightRegion.size = rgenStride;
	rgenSunRegion.stride = rgenStride;
	rgenSunRegion.size = rgenStride;

	missRegion.stride = handleSizeAligned;
	missRegion.size = align_up(missCount * handleSizeAligned, rtProperties.shaderGroupBaseAlignment);

	hitRegion.stride = handleSizeAligned;
	hitRegion.size = align_up(hitCount * handleSizeAligned, rtProperties.shaderGroupBaseAlignment);

	VkDeviceSize rgenOffset = 0;
	VkDeviceSize missOffset = rgenOffset + rgenSize;
	VkDeviceSize hitOffset = missOffset + missRegion.size;

	VkDeviceSize sbtBufferSize = rgenSize + missRegion.size + hitRegion.size;

	BufferBuilder bufbuilder;
	bufbuilder.setUsage(VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	bufbuilder.setSize(sbtBufferSize);
	shaderBindingTable = bufbuilder.create(device.get());
	shaderBindingTable->SetDebugName("shaderBindingTable");

	BufferBuilder tbuilder;
	tbuilder.setUsage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	tbuilder.setSize(sbtBufferSize);
	sbtTransferBuffer = tbuilder.create(device.get());
	sbtTransferBuffer->SetDebugName("sbtTransferBuffer");
	uint8_t* src = (uint8_t*)pipeline->shaderGroupHandles.data();
	uint8_t* dest = (uint8_t*)sbtTransferBuffer->Map(0, sbtBufferSize);
	for (VkDeviceSize i = 0; i < raygenCount; i++)
	{
		memcpy(dest + rgenOffset + i * rgenStride, src, handleSize);
		src += handleSize;
	}
	for (VkDeviceSize i = 0; i < missCount; i++)
	{
		memcpy(dest + missOffset + i * missRegion.stride, src, handleSize);
		src += handleSize;
	}
	for (VkDeviceSize i = 0; i < hitCount; i++)
	{
		memcpy(dest + hitOffset + i * hitRegion.stride, src, handleSize);
		src += handleSize;
	}
	sbtTransferBuffer->Unmap();

	cmdbuffer->copyBuffer(sbtTransferBuffer.get(), shaderBindingTable.get());

	VkBufferDeviceAddressInfo info = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
	info.buffer = shaderBindingTable->buffer;
	VkDeviceAddress sbtAddress = vkGetBufferDeviceAddress(device->device, &info);

	rgenBounceRegion.deviceAddress = sbtAddress + rgenOffset;
	rgenLightRegion.deviceAddress = sbtAddress + rgenOffset + rgenStride;
	rgenSunRegion.deviceAddress = sbtAddress + rgenOffset + 2 * rgenStride;
	missRegion.deviceAddress = sbtAddress + missOffset;
	hitRegion.deviceAddress = sbtAddress + hitOffset;
}

void GPURaytracer::CreateDescriptorSet()
{
	BufferBuilder uniformbuilder;
	uniformbuilder.setUsage(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	uniformbuilder.setSize(sizeof(Uniforms));
	uniformBuffer = uniformbuilder.create(device.get());
	uniformBuffer->SetDebugName("uniformBuffer");

	BufferBuilder uniformtransferbuilder;
	uniformtransferbuilder.setUsage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	uniformtransferbuilder.setSize(sizeof(Uniforms));
	uniformTransferBuffer = uniformtransferbuilder.create(device.get());
	uniformTransferBuffer->SetDebugName("uniformTransferBuffer");

	BufferBuilder itbuilder;
	itbuilder.setUsage(VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	itbuilder.setSize(2 * sizeof(Vec4) * rayTraceImageSize * rayTraceImageSize);
	imageTransferBuffer = itbuilder.create(device.get());
	imageTransferBuffer->SetDebugName("imageTransferBuffer");

	ImageBuilder imgbuilder1;
	imgbuilder1.setUsage(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	imgbuilder1.setFormat(VK_FORMAT_R32G32B32A32_SFLOAT);
	imgbuilder1.setSize(rayTraceImageSize, rayTraceImageSize);
	positionsImage = imgbuilder1.create(device.get());
	positionsImage->SetDebugName("positionsImage");

	ImageBuilder imgbuilder2;
	imgbuilder2.setUsage(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	imgbuilder2.setFormat(VK_FORMAT_R32G32B32A32_SFLOAT);
	imgbuilder2.setSize(rayTraceImageSize, rayTraceImageSize);
	normalsImage = imgbuilder2.create(device.get());
	normalsImage->SetDebugName("normalsImage");

	ImageBuilder imgbuilder3;
	imgbuilder3.setUsage(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	imgbuilder3.setFormat(VK_FORMAT_R32G32B32A32_SFLOAT);
	imgbuilder3.setSize(rayTraceImageSize, rayTraceImageSize);
	outputImage = imgbuilder3.create(device.get());
	outputImage->SetDebugName("outputImage");

	ImageViewBuilder viewbuilder1;
	viewbuilder1.setImage(positionsImage.get(), VK_FORMAT_R32G32B32A32_SFLOAT);
	positionsImageView = viewbuilder1.create(device.get());
	positionsImageView->SetDebugName("positionsImageView");

	ImageViewBuilder viewbuilder2;
	viewbuilder2.setImage(normalsImage.get(), VK_FORMAT_R32G32B32A32_SFLOAT);
	normalsImageView = viewbuilder2.create(device.get());
	normalsImageView->SetDebugName("normalsImageView");

	ImageViewBuilder viewbuilder3;
	viewbuilder3.setImage(outputImage.get(), VK_FORMAT_R32G32B32A32_SFLOAT);
	outputImageView = viewbuilder3.create(device.get());
	outputImageView->SetDebugName("outputImageView");

	DescriptorPoolBuilder poolbuilder;
	poolbuilder.addPoolSize(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1);
	poolbuilder.addPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3);
	poolbuilder.addPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1);
	poolbuilder.addPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2);
	poolbuilder.setMaxSets(1);
	descriptorPool = poolbuilder.create(device.get());
	descriptorPool->SetDebugName("descriptorPool");

	descriptorSet = descriptorPool->allocate(descriptorSetLayout.get());
	descriptorSet->SetDebugName("descriptorSet");

	WriteDescriptors write;
	write.addAccelerationStructure(descriptorSet.get(), 0, tlAccelStruct.get());
	write.addStorageImage(descriptorSet.get(), 1, positionsImageView.get(), VK_IMAGE_LAYOUT_GENERAL);
	write.addStorageImage(descriptorSet.get(), 2, normalsImageView.get(), VK_IMAGE_LAYOUT_GENERAL);
	write.addStorageImage(descriptorSet.get(), 3, outputImageView.get(), VK_IMAGE_LAYOUT_GENERAL);
	write.addBuffer(descriptorSet.get(), 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uniformBuffer.get());
	write.addBuffer(descriptorSet.get(), 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, surfaceIndexBuffer.get());
	write.addBuffer(descriptorSet.get(), 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, surfaceBuffer.get());
	write.updateSets(device.get());
}

void GPURaytracer::PrintVulkanInfo()
{
	const auto& props = device->physicalDevice.properties;

	std::string deviceType;
	switch (props.deviceType)
	{
	case VK_PHYSICAL_DEVICE_TYPE_OTHER: deviceType = "other"; break;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: deviceType = "integrated gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: deviceType = "discrete gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: deviceType = "virtual gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_CPU: deviceType = "cpu"; break;
	default: deviceType = std::to_string(props.deviceType); break;
	}

	std::string apiVersion = std::to_string(VK_VERSION_MAJOR(props.apiVersion)) + "." + std::to_string(VK_VERSION_MINOR(props.apiVersion)) + "." + std::to_string(VK_VERSION_PATCH(props.apiVersion));
	std::string driverVersion = std::to_string(VK_VERSION_MAJOR(props.driverVersion)) + "." + std::to_string(VK_VERSION_MINOR(props.driverVersion)) + "." + std::to_string(VK_VERSION_PATCH(props.driverVersion));

	printf("Vulkan device: %s\n", props.deviceName);
	printf("Vulkan device type: %s\n", deviceType.c_str());
	printf("Vulkan version: %s (api) %s (driver)\n", apiVersion.c_str(), driverVersion.c_str());
}

void GPURaytracer::RaytraceProbeSample(LightProbeSample* probe)
{
	Vec3 incoming(0.0f, 0.0f, 0.0f);

	if (LightBounce > 0)
	{
		Vec3 directions[6] =
		{
			{  1.0f,  0.0f,  0.0f },
			{ -1.0f,  0.0f,  0.0f },
			{  0.0f,  1.0f,  0.0f },
			{  0.0f, -1.0f,  0.0f },
			{  0.0f,  0.0f,  1.0f, },
			{  0.0f,  0.0f, -1.0f, }
		};
		for (int i = 0; i < SAMPLE_COUNT; i++)
		{
			const Vec3& normal = directions[i % 6];
			Vec2 Xi = Hammersley(i, SAMPLE_COUNT);
			Vec3 H = ImportanceSampleGGX(Xi, normal, 1.0f);
			Vec3 L = Vec3::Normalize(H * (2.0f * Vec3::Dot(normal, H)) - normal);
			incoming += TracePath(probe->Position, L, i);
		}
		incoming = incoming / (float)SAMPLE_COUNT / (float)LightBounce;
	}

	for (ThingLight& light : mesh->map->ThingLights)
	{
		Vec3 lightOrigin = light.LightOrigin();
		float lightRadius = light.LightRadius();

		if (probe->Position.DistanceSq(lightOrigin) > (lightRadius * lightRadius))
			continue;

		if (mesh->TraceAnyHit(lightOrigin, probe->Position))
			continue; // this light is occluded by something

		Vec3 dir = (lightOrigin - probe->Position);
		float dist = dir.Unit();
		dir.Normalize();

		incoming += light.rgb * (light.SpotAttenuation(dir) * light.DistAttenuation(dist) * light.intensity);
	}

	const Vec3& sundir = mesh->map->GetSunDirection();
	LevelTraceHit trace = mesh->Trace(probe->Position, probe->Position + sundir * 32768.0f);
	if (trace.fraction != 1.0f && trace.hitSurface->bSky)
		incoming += mesh->map->GetSunColor();

	probe->Color = incoming;
}

void GPURaytracer::RaytraceSurfaceSample(Surface* surface, int x, int y)
{
	Vec3 normal = surface->plane.Normal();
	Vec3 pos = surface->lightmapOrigin + normal + surface->lightmapSteps[0] * (float)x + surface->lightmapSteps[1] * (float)y;

	Vec3 incoming(0.0f, 0.0f, 0.0f);
	if (LightBounce > 0)
	{
		float totalWeight = 0.0f;
		for (int i = 0; i < SAMPLE_COUNT; i++)
		{
			Vec2 Xi = Hammersley(i, SAMPLE_COUNT);
			Vec3 H = ImportanceSampleGGX(Xi, normal, 1.0f);
			Vec3 L = Vec3::Normalize(H * (2.0f * Vec3::Dot(normal, H)) - normal);
			float NdotL = std::max(Vec3::Dot(normal, L), 0.0f);
			if (NdotL > 0.0f)
			{
				incoming += TracePath(pos, L, i) * NdotL;
				totalWeight += NdotL;
			}
		}
		incoming = incoming / totalWeight / (float)LightBounce;
	}

	incoming = incoming + GetSurfaceEmittance(surface, 0.0f) + GetLightEmittance(surface, pos);

	const Vec3& sundir = mesh->map->GetSunDirection();
	float attenuation = normal.Dot(sundir);
	if (attenuation > 0.0f)
	{
		LevelTraceHit trace = mesh->Trace(pos, pos + sundir * 32768.0f);
		if (trace.fraction != 1.0f && trace.hitSurface->bSky)
			incoming += mesh->map->GetSunColor() * attenuation;
	}

	size_t sampleWidth = surface->lightmapDims[0];
	surface->samples[x + y * sampleWidth] = incoming;
}

Vec3 GPURaytracer::GetLightEmittance(Surface* surface, const Vec3& pos)
{
	Vec3 emittance = Vec3(0.0f);
	for (ThingLight& light : mesh->map->ThingLights)
	{
		Vec3 lightOrigin = light.LightOrigin();
		float lightRadius = light.LightRadius();

		if (surface->plane.Distance(lightOrigin) - surface->plane.d < 0)
			continue; // completely behind the plane

		if (pos.DistanceSq(lightOrigin) > (lightRadius * lightRadius))
			continue; // light too far away

		Vec3 dir = (lightOrigin - pos);
		float dist = dir.Unit();
		dir.Normalize();

		float attenuation = light.SpotAttenuation(dir) * light.DistAttenuation(dist) * surface->plane.Normal().Dot(dir);
		if (attenuation <= 0.0f)
			continue;

		if (mesh->TraceAnyHit(lightOrigin, pos))
			continue; // this light is occluded by something

		emittance += light.rgb * (attenuation * light.intensity);
	}
	return emittance;
}

Vec3 GPURaytracer::TracePath(const Vec3& pos, const Vec3& dir, int sampleIndex, int depth)
{
	if (depth >= LightBounce)
		return Vec3(0.0f);

	LevelTraceHit hit = mesh->Trace(pos + dir * 0.1f, pos + dir * 2000.0f);
	if (hit.fraction == 1.0f)
		return Vec3(0.0f);

	Vec3 normal = hit.hitSurface->plane.Normal();
	Vec3 hitpos = hit.start * (1.0f - hit.fraction) + hit.end * hit.fraction;

	Vec3 emittance = GetSurfaceEmittance(hit.hitSurface, pos.Distance(hitpos)) + GetLightEmittance(hit.hitSurface, hitpos) * 0.5f;

	const Vec3& sundir = mesh->map->GetSunDirection();
	float attenuation = normal.Dot(sundir);
	if (attenuation > 0.0f)
	{
		Vec3 start = hitpos + normal * 0.1f;
		LevelTraceHit trace = mesh->Trace(start, start + sundir * 32768.0f);
		if (trace.fraction != 1.0f && trace.hitSurface->bSky)
			emittance += mesh->map->GetSunColor() * (attenuation * 0.5f);
	}

	Vec2 Xi = Hammersley(sampleIndex, SAMPLE_COUNT);
	Vec3 H = ImportanceSampleGGX(Xi, normal, 1.0f);
	Vec3 L = Vec3::Normalize(H * (2.0f * Vec3::Dot(normal, H)) - normal);

	float NdotL = Vec3::Dot(normal, L);
	if (NdotL <= 0.0f)
		return emittance;

	const float p = 1 / (2 * M_PI);
	Vec3 incoming = TracePath(hitpos, normal, (sampleIndex + depth + 1) % SAMPLE_COUNT, depth + 1);

	return emittance + incoming * NdotL / p;
}

float GPURaytracer::RadicalInverse_VdC(uint32_t bits)
{
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10f; // / 0x100000000
}

Vec2 GPURaytracer::Hammersley(uint32_t i, uint32_t N)
{
	return Vec2(float(i) / float(N), RadicalInverse_VdC(i));
}

Vec3 GPURaytracer::ImportanceSampleGGX(Vec2 Xi, Vec3 N, float roughness)
{
	float a = roughness * roughness;

	float phi = 2.0f * M_PI * Xi.x;
	float cosTheta = sqrt((1.0f - Xi.y) / (1.0f + (a * a - 1.0f) * Xi.y));
	float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

	// from spherical coordinates to cartesian coordinates
	Vec3 H(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta);

	// from tangent-space vector to world-space sample vector
	Vec3 up = std::abs(N.z) < 0.999f ? Vec3(0.0f, 0.0f, 1.0f) : Vec3(1.0f, 0.0f, 0.0f);
	Vec3 tangent = Vec3::Normalize(Vec3::Cross(up, N));
	Vec3 bitangent = Vec3::Cross(N, tangent);

	Vec3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
	return Vec3::Normalize(sampleVec);
}

Vec3 GPURaytracer::GetSurfaceEmittance(Surface* surface, float distance)
{
	SurfaceLightDef* def = nullptr;
	if (surface->type >= ST_MIDDLESIDE && surface->type <= ST_LOWERSIDE)
	{
		int lightdefidx = mesh->map->Sides[surface->typeIndex].lightdef;
		if (lightdefidx != -1)
		{
			def = &mesh->map->SurfaceLights[lightdefidx];
		}
	}
	else if (surface->type == ST_FLOOR || surface->type == ST_CEILING)
	{
		MapSubsectorEx* sub = &mesh->map->GLSubsectors[surface->typeIndex];
		IntSector* sector = mesh->map->GetSectorFromSubSector(sub);

		if (sector && surface->numVerts > 0)
		{
			if (sector->floorlightdef != -1 && surface->type == ST_FLOOR)
			{
				def = &mesh->map->SurfaceLights[sector->floorlightdef];
			}
			else if (sector->ceilinglightdef != -1 && surface->type == ST_CEILING)
			{
				def = &mesh->map->SurfaceLights[sector->ceilinglightdef];
			}
		}
	}

	if (def && distance < def->distance + def->distance)
	{
		float radius = def->distance + def->distance;
		float attenuation = std::max(1.0f - (distance / radius), 0.0f);
		return def->rgb * (attenuation * def->intensity);
	}
	else
	{
		return Vec3(0.0f);
	}
}
