// Copyright 2018 The SwiftShader Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "VkDescriptorSetLayout.hpp"

#include "VkDescriptorSet.hpp"
#include "VkSampler.hpp"
#include "VkImageView.hpp"
#include "VkBufferView.hpp"
#include "System/Types.hpp"

#include <algorithm>
#include <cstring>

namespace
{

static bool UsesImmutableSamplers(const VkDescriptorSetLayoutBinding& binding)
{
	return (((binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) ||
	        (binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)) &&
	        (binding.pImmutableSamplers != nullptr));
}

}

namespace vk
{

DescriptorSetLayout::DescriptorSetLayout(const VkDescriptorSetLayoutCreateInfo* pCreateInfo, void* mem) :
	flags(pCreateInfo->flags), bindingCount(pCreateInfo->bindingCount), bindings(reinterpret_cast<VkDescriptorSetLayoutBinding*>(mem))
{
	uint8_t* hostMemory = static_cast<uint8_t*>(mem) + bindingCount * sizeof(VkDescriptorSetLayoutBinding);
	bindingOffsets = reinterpret_cast<size_t*>(hostMemory);
	hostMemory += bindingCount * sizeof(size_t);

	size_t offset = 0;
	for(uint32_t i = 0; i < bindingCount; i++)
	{
		bindings[i] = pCreateInfo->pBindings[i];
		if(UsesImmutableSamplers(bindings[i]))
		{
			size_t immutableSamplersSize = bindings[i].descriptorCount * sizeof(VkSampler);
			bindings[i].pImmutableSamplers = reinterpret_cast<const VkSampler*>(hostMemory);
			hostMemory += immutableSamplersSize;
			memcpy(const_cast<VkSampler*>(bindings[i].pImmutableSamplers),
			       pCreateInfo->pBindings[i].pImmutableSamplers,
			       immutableSamplersSize);
		}
		else
		{
			bindings[i].pImmutableSamplers = nullptr;
		}
		bindingOffsets[i] = offset;
		offset += bindings[i].descriptorCount * GetDescriptorSize(bindings[i].descriptorType);
	}
}

void DescriptorSetLayout::destroy(const VkAllocationCallbacks* pAllocator)
{
	vk::deallocate(bindings, pAllocator); // This allocation also contains pImmutableSamplers
}

size_t DescriptorSetLayout::ComputeRequiredAllocationSize(const VkDescriptorSetLayoutCreateInfo* pCreateInfo)
{
	size_t allocationSize = pCreateInfo->bindingCount * (sizeof(VkDescriptorSetLayoutBinding) + sizeof(size_t));

	for(uint32_t i = 0; i < pCreateInfo->bindingCount; i++)
	{
		if(UsesImmutableSamplers(pCreateInfo->pBindings[i]))
		{
			allocationSize += pCreateInfo->pBindings[i].descriptorCount * sizeof(VkSampler);
		}
	}

	return allocationSize;
}

size_t DescriptorSetLayout::GetDescriptorSize(VkDescriptorType type)
{
	size_t size = 0;

	switch(type)
	{
	case VK_DESCRIPTOR_TYPE_SAMPLER:
	case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
	case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		size = sizeof(SampledImageDescriptor);
		break;
	case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
	case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
		size = sizeof(StorageImageDescriptor);
		break;
	case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
		size = sizeof(VkDescriptorImageInfo);
		break;
	case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		size = sizeof(VkBufferView);
		break;
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
		size = sizeof(VkDescriptorBufferInfo);
		break;
	default:
		UNIMPLEMENTED("Unsupported Descriptor Type");
		return 0;
	}

	// Aligning each descriptor to 16 bytes allows for more efficient vector accesses in the shaders.
	return sw::align<16>(size);  // TODO(b/123244275): Eliminate by using a custom alignas(16) struct for each desctriptor.
}

size_t DescriptorSetLayout::getDescriptorSetAllocationSize() const
{
	// vk::DescriptorSet has a layout member field.
	return sizeof(vk::DescriptorSetHeader) + getDescriptorSetDataSize();
}

size_t DescriptorSetLayout::getDescriptorSetDataSize() const
{
	size_t size = 0;
	for(uint32_t i = 0; i < bindingCount; i++)
	{
		size += bindings[i].descriptorCount * GetDescriptorSize(bindings[i].descriptorType);
	}

	return size;
}

uint32_t DescriptorSetLayout::getBindingIndex(uint32_t binding) const
{
	for(uint32_t i = 0; i < bindingCount; i++)
	{
		if(binding == bindings[i].binding)
		{
			return i;
		}
	}

	DABORT("Invalid DescriptorSetLayout binding: %d", int(binding));
	return 0;
}

void DescriptorSetLayout::initialize(VkDescriptorSet vkDescriptorSet)
{
	// Use a pointer to this descriptor set layout as the descriptor set's header
	DescriptorSet* descriptorSet = vk::Cast(vkDescriptorSet);
	descriptorSet->header.layout = this;
	uint8_t* mem = descriptorSet->data;

	for(uint32_t i = 0; i < bindingCount; i++)
	{
		size_t typeSize = GetDescriptorSize(bindings[i].descriptorType);
		if(UsesImmutableSamplers(bindings[i]))
		{
			for(uint32_t j = 0; j < bindings[i].descriptorCount; j++)
			{
				SampledImageDescriptor* imageSamplerDescriptor = reinterpret_cast<SampledImageDescriptor*>(mem);
				imageSamplerDescriptor->updateSampler(vk::Cast(bindings[i].pImmutableSamplers[j]));
				mem += typeSize;
			}
		}
		else
		{
			mem += bindings[i].descriptorCount * typeSize;
		}
	}
}

size_t DescriptorSetLayout::getBindingCount() const
{
	return bindingCount;
}

size_t DescriptorSetLayout::getBindingStride(uint32_t binding) const
{
	uint32_t index = getBindingIndex(binding);
	return GetDescriptorSize(bindings[index].descriptorType);
}

size_t DescriptorSetLayout::getBindingOffset(uint32_t binding, size_t arrayElement) const
{
	uint32_t index = getBindingIndex(binding);
	auto typeSize = GetDescriptorSize(bindings[index].descriptorType);
	return bindingOffsets[index] + OFFSET(DescriptorSet, data[0]) + (typeSize * arrayElement);
}

bool DescriptorSetLayout::isDynamic(VkDescriptorType type)
{
	return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
		   type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

bool DescriptorSetLayout::isBindingDynamic(uint32_t binding) const
{
	uint32_t index = getBindingIndex(binding);
	return isDynamic(bindings[index].descriptorType);
}

uint32_t DescriptorSetLayout::getDynamicDescriptorCount() const
{
	uint32_t count = 0;
	for (size_t i = 0; i < bindingCount; i++)
	{
		if (isDynamic(bindings[i].descriptorType))
		{
			count += bindings[i].descriptorCount;
		}
	}
	return count;
}

uint32_t DescriptorSetLayout::getDynamicDescriptorOffset(uint32_t binding) const
{
	uint32_t n = getBindingIndex(binding);
	ASSERT(isDynamic(bindings[n].descriptorType));

	uint32_t index = 0;
	for (uint32_t i = 0; i < n; i++)
	{
		if (isDynamic(bindings[i].descriptorType))
		{
			index += bindings[i].descriptorCount;
		}
	}
	return index;
}

VkDescriptorSetLayoutBinding const & DescriptorSetLayout::getBindingLayout(uint32_t binding) const
{
	uint32_t index = getBindingIndex(binding);
	return bindings[index];
}

uint8_t* DescriptorSetLayout::getOffsetPointer(DescriptorSet *descriptorSet, uint32_t binding, uint32_t arrayElement, uint32_t count, size_t* typeSize) const
{
	uint32_t index = getBindingIndex(binding);
	*typeSize = GetDescriptorSize(bindings[index].descriptorType);
	size_t byteOffset = bindingOffsets[index] + (*typeSize * arrayElement);
	ASSERT(((*typeSize * count) + byteOffset) <= getDescriptorSetDataSize()); // Make sure the operation will not go out of bounds
	return &descriptorSet->data[byteOffset];
}

void SampledImageDescriptor::updateSampler(const vk::Sampler *sampler)
{
	this->sampler = sampler;

	texture.minLod = sw::clamp(sampler->minLod, 0.0f, (float)(sw::MAX_TEXTURE_LOD));
	texture.maxLod = sw::clamp(sampler->maxLod, 0.0f, (float)(sw::MAX_TEXTURE_LOD));
}

void DescriptorSetLayout::WriteDescriptorSet(DescriptorSet *dstSet, VkDescriptorUpdateTemplateEntry const &entry, char const *src)
{
	DescriptorSetLayout* dstLayout = dstSet->header.layout;
	auto &binding = dstLayout->bindings[dstLayout->getBindingIndex(entry.dstBinding)];
	ASSERT(dstLayout);
	ASSERT(binding.descriptorType == entry.descriptorType);

	size_t typeSize = 0;
	uint8_t* memToWrite = dstLayout->getOffsetPointer(dstSet, entry.dstBinding, entry.dstArrayElement, entry.descriptorCount, &typeSize);

	ASSERT(reinterpret_cast<intptr_t>(memToWrite) % 16 == 0);  // Each descriptor must be 16-byte aligned.

	if(entry.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
	{
		SampledImageDescriptor *imageSampler = reinterpret_cast<SampledImageDescriptor*>(memToWrite);

		for(uint32_t i = 0; i < entry.descriptorCount; i++)
		{
			auto update = reinterpret_cast<VkDescriptorImageInfo const *>(src + entry.offset + entry.stride * i);
			vk::ImageView *imageView = vk::Cast(update->imageView);
			sw::Texture *texture = &imageSampler[i].texture;

			// "All consecutive bindings updated via a single VkWriteDescriptorSet structure, except those with a
			//  descriptorCount of zero, must all either use immutable samplers or must all not use immutable samplers."
			if(!binding.pImmutableSamplers)
			{
				imageSampler[i].updateSampler(vk::Cast(update->sampler));
			}

			imageSampler[i].imageView = imageView;

			auto &subresourceRange = imageView->getSubresourceRange();
			int baseLevel = subresourceRange.baseMipLevel;

			for(int mipmapLevel = 0; mipmapLevel < sw::MIPMAP_LEVELS; mipmapLevel++)
			{
				int level = mipmapLevel - baseLevel;  // Level within the image view
				level = sw::clamp(level, 0, (int)subresourceRange.levelCount - 1);

				VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_COLOR_BIT;
				sw::Mipmap &mipmap = texture->mipmap[mipmapLevel];

				if(imageView->getType() == VK_IMAGE_VIEW_TYPE_CUBE)
				{
					for(int face = 0; face < 6; face++)
					{
						// Obtain the pointer to the corner of the level including the border, for seamless sampling.
						// This is taken into account in the sampling routine, which can't handle negative texel coordinates.
						VkOffset3D offset = {-1, -1, 0};

						// TODO(b/129523279): Implement as 6 consecutive layers instead of separate pointers.
						mipmap.buffer[face] = imageView->getOffsetPointer(offset, aspect, level, face);
					}
				}
				else
				{
					VkOffset3D offset = {0, 0, 0};
					mipmap.buffer[0] = imageView->getOffsetPointer(offset, aspect, level, 0);
				}

				VkExtent3D extent = imageView->getMipLevelExtent(level);
				Format format = imageView->getFormat();
				int width = extent.width;
				int height = extent.height;
				int depth = extent.depth;
				int pitchP = imageView->rowPitchBytes(aspect, level) / format.bytes();
				int sliceP = imageView->slicePitchBytes(aspect, level) / format.bytes();

				float exp2LOD = 1.0f;

				if(mipmapLevel == 0)
				{
					texture->widthHeightLOD[0] = width * exp2LOD;
					texture->widthHeightLOD[1] = width * exp2LOD;
					texture->widthHeightLOD[2] = height * exp2LOD;
					texture->widthHeightLOD[3] = height * exp2LOD;

					texture->widthLOD[0] = width * exp2LOD;
					texture->widthLOD[1] = width * exp2LOD;
					texture->widthLOD[2] = width * exp2LOD;
					texture->widthLOD[3] = width * exp2LOD;

					texture->heightLOD[0] = height * exp2LOD;
					texture->heightLOD[1] = height * exp2LOD;
					texture->heightLOD[2] = height * exp2LOD;
					texture->heightLOD[3] = height * exp2LOD;

					texture->depthLOD[0] = depth * exp2LOD;
					texture->depthLOD[1] = depth * exp2LOD;
					texture->depthLOD[2] = depth * exp2LOD;
					texture->depthLOD[3] = depth * exp2LOD;
				}

				if(format.isFloatFormat())
				{
					mipmap.fWidth[0] = (float)width / 65536.0f;
					mipmap.fWidth[1] = (float)width / 65536.0f;
					mipmap.fWidth[2] = (float)width / 65536.0f;
					mipmap.fWidth[3] = (float)width / 65536.0f;

					mipmap.fHeight[0] = (float)height / 65536.0f;
					mipmap.fHeight[1] = (float)height / 65536.0f;
					mipmap.fHeight[2] = (float)height / 65536.0f;
					mipmap.fHeight[3] = (float)height / 65536.0f;

					mipmap.fDepth[0] = (float)depth / 65536.0f;
					mipmap.fDepth[1] = (float)depth / 65536.0f;
					mipmap.fDepth[2] = (float)depth / 65536.0f;
					mipmap.fDepth[3] = (float)depth / 65536.0f;
				}

				short halfTexelU = 0x8000 / width;
				short halfTexelV = 0x8000 / height;
				short halfTexelW = 0x8000 / depth;

				mipmap.uHalf[0] = halfTexelU;
				mipmap.uHalf[1] = halfTexelU;
				mipmap.uHalf[2] = halfTexelU;
				mipmap.uHalf[3] = halfTexelU;

				mipmap.vHalf[0] = halfTexelV;
				mipmap.vHalf[1] = halfTexelV;
				mipmap.vHalf[2] = halfTexelV;
				mipmap.vHalf[3] = halfTexelV;

				mipmap.wHalf[0] = halfTexelW;
				mipmap.wHalf[1] = halfTexelW;
				mipmap.wHalf[2] = halfTexelW;
				mipmap.wHalf[3] = halfTexelW;

				mipmap.width[0] = width;
				mipmap.width[1] = width;
				mipmap.width[2] = width;
				mipmap.width[3] = width;

				mipmap.height[0] = height;
				mipmap.height[1] = height;
				mipmap.height[2] = height;
				mipmap.height[3] = height;

				mipmap.depth[0] = depth;
				mipmap.depth[1] = depth;
				mipmap.depth[2] = depth;
				mipmap.depth[3] = depth;

				mipmap.onePitchP[0] = 1;
				mipmap.onePitchP[1] = pitchP;
				mipmap.onePitchP[2] = 1;
				mipmap.onePitchP[3] = pitchP;

				mipmap.pitchP[0] = pitchP;
				mipmap.pitchP[1] = pitchP;
				mipmap.pitchP[2] = pitchP;
				mipmap.pitchP[3] = pitchP;

				mipmap.sliceP[0] = sliceP;
				mipmap.sliceP[1] = sliceP;
				mipmap.sliceP[2] = sliceP;
				mipmap.sliceP[3] = sliceP;

				// TODO(b/129523279)
				if(false/*format == FORMAT_YV12_BT601 ||
				   format == FORMAT_YV12_BT709 ||
				   format == FORMAT_YV12_JFIF*/)
				{
					unsigned int YStride = pitchP;
					unsigned int YSize = YStride * height;
					unsigned int CStride = sw::align<16>(YStride / 2);
					unsigned int CSize = CStride * height / 2;

					mipmap.buffer[1] = (sw::byte*)mipmap.buffer[0] + YSize;
					mipmap.buffer[2] = (sw::byte*)mipmap.buffer[1] + CSize;

					texture->mipmap[1].width[0] = width / 2;
					texture->mipmap[1].width[1] = width / 2;
					texture->mipmap[1].width[2] = width / 2;
					texture->mipmap[1].width[3] = width / 2;
					texture->mipmap[1].height[0] = height / 2;
					texture->mipmap[1].height[1] = height / 2;
					texture->mipmap[1].height[2] = height / 2;
					texture->mipmap[1].height[3] = height / 2;
					texture->mipmap[1].onePitchP[0] = 1;
					texture->mipmap[1].onePitchP[1] = CStride;
					texture->mipmap[1].onePitchP[2] = 1;
					texture->mipmap[1].onePitchP[3] = CStride;
				}
			}
		}
	}
	else if (entry.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
			 entry.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
	{
		auto descriptor = reinterpret_cast<StorageImageDescriptor *>(memToWrite);
		for(uint32_t i = 0; i < entry.descriptorCount; i++)
		{
			auto update = reinterpret_cast<VkDescriptorImageInfo const *>(src + entry.offset + entry.stride * i);
			auto imageView = Cast(update->imageView);
			descriptor[i].ptr = imageView->getOffsetPointer({0, 0, 0}, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0);
			descriptor[i].extent = imageView->getMipLevelExtent(0);
			descriptor[i].rowPitchBytes = imageView->rowPitchBytes(VK_IMAGE_ASPECT_COLOR_BIT, 0);
			descriptor[i].slicePitchBytes = imageView->getSubresourceRange().layerCount > 1
											? imageView->layerPitchBytes(VK_IMAGE_ASPECT_COLOR_BIT)
											: imageView->slicePitchBytes(VK_IMAGE_ASPECT_COLOR_BIT, 0);
			descriptor[i].arrayLayers = imageView->getSubresourceRange().layerCount;
			descriptor[i].sizeInBytes = imageView->getImageSizeInBytes();
		}
	}
	else if (entry.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
	{
		auto descriptor = reinterpret_cast<StorageImageDescriptor *>(memToWrite);
		for (uint32_t i = 0; i < entry.descriptorCount; i++)
		{
			auto update = reinterpret_cast<VkBufferView const *>(src + entry.offset + entry.stride * i);
			auto bufferView = Cast(*update);
			descriptor[i].ptr = bufferView->getPointer();
			descriptor[i].extent = {bufferView->getElementCount(), 1, 1};
			descriptor[i].rowPitchBytes = 0;
			descriptor[i].slicePitchBytes = 0;
			descriptor[i].arrayLayers = 1;
			descriptor[i].sizeInBytes = bufferView->getRangeInBytes();
		}
	}
	else
	{
		// If the dstBinding has fewer than descriptorCount array elements remaining
		// starting from dstArrayElement, then the remainder will be used to update
		// the subsequent binding - dstBinding+1 starting at array element zero. If
		// a binding has a descriptorCount of zero, it is skipped. This behavior
		// applies recursively, with the update affecting consecutive bindings as
		// needed to update all descriptorCount descriptors.
		for (auto i = 0u; i < entry.descriptorCount; i++)
			memcpy(memToWrite + typeSize * i, src + entry.offset + entry.stride * i, typeSize);
	}
}

void DescriptorSetLayout::WriteDescriptorSet(const VkWriteDescriptorSet& writeDescriptorSet)
{
	DescriptorSet* dstSet = vk::Cast(writeDescriptorSet.dstSet);
	VkDescriptorUpdateTemplateEntry e;
	e.descriptorType = writeDescriptorSet.descriptorType;
	e.dstBinding = writeDescriptorSet.dstBinding;
	e.dstArrayElement = writeDescriptorSet.dstArrayElement;
	e.descriptorCount = writeDescriptorSet.descriptorCount;
	e.offset = 0;
	void const *ptr = nullptr;
	switch (writeDescriptorSet.descriptorType)
	{
	case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
	case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		ptr = writeDescriptorSet.pTexelBufferView;
		e.stride = sizeof(*VkWriteDescriptorSet::pTexelBufferView);
		break;

	case VK_DESCRIPTOR_TYPE_SAMPLER:
	case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
	case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
	case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
	case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		ptr = writeDescriptorSet.pImageInfo;
		e.stride = sizeof(VkDescriptorImageInfo);
		break;

	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
		ptr = writeDescriptorSet.pBufferInfo;
		e.stride = sizeof(VkDescriptorBufferInfo);
		break;

	default:
		UNIMPLEMENTED("descriptor type %u", writeDescriptorSet.descriptorType);
	}

	WriteDescriptorSet(dstSet, e, reinterpret_cast<char const *>(ptr));
}

void DescriptorSetLayout::CopyDescriptorSet(const VkCopyDescriptorSet& descriptorCopies)
{
	DescriptorSet* srcSet = vk::Cast(descriptorCopies.srcSet);
	DescriptorSetLayout* srcLayout = srcSet->header.layout;
	ASSERT(srcLayout);

	DescriptorSet* dstSet = vk::Cast(descriptorCopies.dstSet);
	DescriptorSetLayout* dstLayout = dstSet->header.layout;
	ASSERT(dstLayout);

	size_t srcTypeSize = 0;
	uint8_t* memToRead = srcLayout->getOffsetPointer(srcSet, descriptorCopies.srcBinding, descriptorCopies.srcArrayElement, descriptorCopies.descriptorCount, &srcTypeSize);

	size_t dstTypeSize = 0;
	uint8_t* memToWrite = dstLayout->getOffsetPointer(dstSet, descriptorCopies.dstBinding, descriptorCopies.dstArrayElement, descriptorCopies.descriptorCount, &dstTypeSize);

	ASSERT(srcTypeSize == dstTypeSize);
	size_t writeSize = dstTypeSize * descriptorCopies.descriptorCount;
	memcpy(memToWrite, memToRead, writeSize);
}

} // namespace vk
