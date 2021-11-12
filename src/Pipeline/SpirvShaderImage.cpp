// Copyright 2019 The SwiftShader Authors. All Rights Reserved.
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

#include "SpirvShader.hpp"

#include "System/Types.hpp"

#include "Vulkan/VkDescriptorSetLayout.hpp"
#include "Vulkan/VkPipelineLayout.hpp"

#include <spirv/unified1/spirv.hpp>

namespace {

VkFormat SpirvFormatToVulkanFormat(spv::ImageFormat format)
{
	switch(format)
	{
	case spv::ImageFormatRgba32f: return VK_FORMAT_R32G32B32A32_SFLOAT;
	case spv::ImageFormatRgba16f: return VK_FORMAT_R16G16B16A16_SFLOAT;
	case spv::ImageFormatR32f: return VK_FORMAT_R32_SFLOAT;
	case spv::ImageFormatRgba8: return VK_FORMAT_R8G8B8A8_UNORM;
	case spv::ImageFormatRgba8Snorm: return VK_FORMAT_R8G8B8A8_SNORM;
	case spv::ImageFormatRg32f: return VK_FORMAT_R32G32_SFLOAT;
	case spv::ImageFormatRg16f: return VK_FORMAT_R16G16_SFLOAT;
	case spv::ImageFormatR11fG11fB10f: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
	case spv::ImageFormatR16f: return VK_FORMAT_R16_SFLOAT;
	case spv::ImageFormatRgba16: return VK_FORMAT_R16G16B16A16_UNORM;
	case spv::ImageFormatRgb10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	case spv::ImageFormatRg16: return VK_FORMAT_R16G16_UNORM;
	case spv::ImageFormatRg8: return VK_FORMAT_R8G8_UNORM;
	case spv::ImageFormatR16: return VK_FORMAT_R16_UNORM;
	case spv::ImageFormatR8: return VK_FORMAT_R8_UNORM;
	case spv::ImageFormatRgba16Snorm: return VK_FORMAT_R16G16B16A16_SNORM;
	case spv::ImageFormatRg16Snorm: return VK_FORMAT_R16G16_SNORM;
	case spv::ImageFormatRg8Snorm: return VK_FORMAT_R8G8_SNORM;
	case spv::ImageFormatR16Snorm: return VK_FORMAT_R16_SNORM;
	case spv::ImageFormatR8Snorm: return VK_FORMAT_R8_SNORM;
	case spv::ImageFormatRgba32i: return VK_FORMAT_R32G32B32A32_SINT;
	case spv::ImageFormatRgba16i: return VK_FORMAT_R16G16B16A16_SINT;
	case spv::ImageFormatRgba8i: return VK_FORMAT_R8G8B8A8_SINT;
	case spv::ImageFormatR32i: return VK_FORMAT_R32_SINT;
	case spv::ImageFormatRg32i: return VK_FORMAT_R32G32_SINT;
	case spv::ImageFormatRg16i: return VK_FORMAT_R16G16_SINT;
	case spv::ImageFormatRg8i: return VK_FORMAT_R8G8_SINT;
	case spv::ImageFormatR16i: return VK_FORMAT_R16_SINT;
	case spv::ImageFormatR8i: return VK_FORMAT_R8_SINT;
	case spv::ImageFormatRgba32ui: return VK_FORMAT_R32G32B32A32_UINT;
	case spv::ImageFormatRgba16ui: return VK_FORMAT_R16G16B16A16_UINT;
	case spv::ImageFormatRgba8ui: return VK_FORMAT_R8G8B8A8_UINT;
	case spv::ImageFormatR32ui: return VK_FORMAT_R32_UINT;
	case spv::ImageFormatRgb10a2ui: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
	case spv::ImageFormatRg32ui: return VK_FORMAT_R32G32_UINT;
	case spv::ImageFormatRg16ui: return VK_FORMAT_R16G16_UINT;
	case spv::ImageFormatRg8ui: return VK_FORMAT_R8G8_UINT;
	case spv::ImageFormatR16ui: return VK_FORMAT_R16_UINT;
	case spv::ImageFormatR8ui: return VK_FORMAT_R8_UINT;

	default:
		UNSUPPORTED("SPIR-V ImageFormat %u", format);
		return VK_FORMAT_UNDEFINED;
	}
}

sw::SIMD::Float sRGBtoLinear(sw::SIMD::Float c)
{
	sw::SIMD::Float lc = c * sw::SIMD::Float(1.0f / 12.92f);
	sw::SIMD::Float ec = sw::power((c + sw::SIMD::Float(0.055f)) * sw::SIMD::Float(1.0f / 1.055f), sw::SIMD::Float(2.4f));

	sw::SIMD::Int linear = CmpLT(c, sw::SIMD::Float(0.04045f));

	return rr::As<sw::SIMD::Float>((linear & rr::As<sw::SIMD::Int>(lc)) | (~linear & rr::As<sw::SIMD::Int>(ec)));  // TODO: IfThenElse()
}

}  // anonymous namespace

namespace sw {

SpirvShader::ImageInstruction::ImageInstruction(InsnIterator insn, const SpirvShader &spirv)
    : ImageInstructionState(parseVariantAndMethod(insn))
    , position(insn.distanceFrom(spirv.begin()))
{
	resultId = insn.resultId();     // word(2)
	sampledImageId = insn.word(3);  // For OpImageFetch this is just an Image, not a SampledImage.
	coordinateId = insn.word(4);

	const Object &coordinateObject = spirv.getObject(coordinateId);
	const Type &coordinateType = spirv.getType(coordinateObject);
	coordinates = coordinateType.componentCount - (isProj() ? 1 : 0);

	if(isDref())
	{
		drefId = insn.word(5);
	}

	if(samplerMethod == Gather)
	{
		gatherComponent = !isDref() ? spirv.getObject(insn.word(5)).constantValue[0] : 0;
	}

	uint32_t imageOperands = getImageOperands(insn);                   // The mask which indicates which operands are provided.
	uint32_t operand = (isDref() || samplerMethod == Gather) ? 7 : 6;  // The first actual operand <id> location.

	if(imageOperands & spv::ImageOperandsBiasMask)
	{
		ASSERT(samplerMethod == Bias);
		lodOrBiasId = insn.word(operand);
		operand++;
		imageOperands &= ~spv::ImageOperandsBiasMask;
	}

	if(imageOperands & spv::ImageOperandsLodMask)
	{
		ASSERT(samplerMethod == Lod || samplerMethod == Fetch);
		lodOrBiasId = insn.word(operand);
		operand++;
		imageOperands &= ~spv::ImageOperandsLodMask;
	}

	if(imageOperands & spv::ImageOperandsGradMask)
	{
		ASSERT(samplerMethod == Grad);
		gradDxId = insn.word(operand + 0);
		gradDyId = insn.word(operand + 1);
		operand += 2;
		imageOperands &= ~spv::ImageOperandsGradMask;

		grad = spirv.getObjectType(gradDxId).componentCount;
	}

	if(imageOperands & spv::ImageOperandsConstOffsetMask)
	{
		offsetId = insn.word(operand);
		operand++;
		imageOperands &= ~spv::ImageOperandsConstOffsetMask;

		offset = spirv.getObjectType(offsetId).componentCount;
	}

	if(imageOperands & spv::ImageOperandsSampleMask)
	{
		ASSERT(samplerMethod == Fetch);
		sampleId = insn.word(operand);
		imageOperands &= ~spv::ImageOperandsSampleMask;

		sample = true;
	}

	if(imageOperands != 0)
	{
		UNSUPPORTED("Image operands 0x%08X", imageOperands);
	}
}

SpirvShader::ImageInstructionState SpirvShader::ImageInstruction::parseVariantAndMethod(InsnIterator insn)
{
	uint32_t imageOperands = getImageOperands(insn);
	bool bias = imageOperands & spv::ImageOperandsBiasMask;
	bool grad = imageOperands & spv::ImageOperandsGradMask;

	switch(insn.opcode())
	{
	case spv::OpImageSampleImplicitLod: return { None, bias ? Bias : Implicit };
	case spv::OpImageSampleExplicitLod: return { None, grad ? Grad : Lod };
	case spv::OpImageSampleDrefImplicitLod: return { Dref, bias ? Bias : Implicit };
	case spv::OpImageSampleDrefExplicitLod: return { Dref, grad ? Grad : Lod };
	case spv::OpImageSampleProjImplicitLod: return { Proj, bias ? Bias : Implicit };
	case spv::OpImageSampleProjExplicitLod: return { Proj, grad ? Grad : Lod };
	case spv::OpImageSampleProjDrefImplicitLod: return { ProjDref, bias ? Bias : Implicit };
	case spv::OpImageSampleProjDrefExplicitLod: return { ProjDref, grad ? Grad : Lod };
	case spv::OpImageGather: return { None, Gather };
	case spv::OpImageDrefGather: return { Dref, Gather };
	case spv::OpImageFetch: return { None, Fetch };
	case spv::OpImageQueryLod: return { None, Query };

	default:
		ASSERT(false);
		return { None, Implicit };
	}
}

uint32_t SpirvShader::ImageInstruction::getImageOperands(InsnIterator insn)
{
	switch(insn.opcode())
	{
	case spv::OpImageSampleImplicitLod:
	case spv::OpImageSampleProjImplicitLod:
		return insn.wordCount() > 5 ? insn.word(5) : 0;  // Optional
	case spv::OpImageSampleExplicitLod:
	case spv::OpImageSampleProjExplicitLod:
		return insn.word(5);  // "Either Lod or Grad image operands must be present."
	case spv::OpImageSampleDrefImplicitLod:
	case spv::OpImageSampleProjDrefImplicitLod:
		return insn.wordCount() > 6 ? insn.word(6) : 0;  // Optional
	case spv::OpImageSampleDrefExplicitLod:
	case spv::OpImageSampleProjDrefExplicitLod:
		return insn.word(6);  // "Either Lod or Grad image operands must be present."
	case spv::OpImageGather:
	case spv::OpImageDrefGather:
		return insn.wordCount() > 6 ? insn.word(6) : 0;  // Optional
	case spv::OpImageFetch:
		return insn.wordCount() > 5 ? insn.word(5) : 0;  // Optional
	case spv::OpImageQueryLod:
		ASSERT(insn.wordCount() == 5);
		return 0;

	default:
		ASSERT(false);
		return 0;
	}
}

SpirvShader::EmitResult SpirvShader::EmitImageSample(InsnIterator insn, EmitState *state) const
{
	auto &resultType = getType(insn.resultTypeId());
	auto &result = state->createIntermediate(insn.resultId(), resultType.componentCount);
	Array<SIMD::Float> out(4);

	ImageInstruction instruction(insn, *this);

	// TODO(b/153380916): When we're in a code path that is always executed,
	// i.e. post-dominators of the entry block, we don't have to dynamically
	// check whether any lanes are active, and can elide the jump.
	If(AnyTrue(state->activeLaneMask()))
	{
		EmitImageSampleUnconditional(out, instruction, state);
	}

	for(auto i = 0u; i < resultType.componentCount; i++) { result.move(i, out[i]); }

	return EmitResult::Continue;
}

void SpirvShader::EmitImageSampleUnconditional(Array<SIMD::Float> &out, const ImageInstruction &instruction, EmitState *state) const
{
	Pointer<Byte> imageDescriptor = state->getPointer(instruction.sampledImageId).base;  // vk::SampledImageDescriptor*

	Pointer<Byte> samplerFunction = lookupSamplerFunction(imageDescriptor, instruction, state);

	callSamplerFunction(samplerFunction, out, imageDescriptor, instruction, state);
}

Pointer<Byte> SpirvShader::lookupSamplerFunction(Pointer<Byte> imageDescriptor, const ImageInstruction &instruction, EmitState *state) const
{
	// If using a separate sampler, look through the OpSampledImage instruction to find the sampler descriptor
	auto &sampledImage = getObject(instruction.sampledImageId);
	auto samplerDescriptor = (sampledImage.opcode() == spv::OpSampledImage) ? state->getPointer(sampledImage.definition.word(4)).base : imageDescriptor;

	rr::Int samplerId = *Pointer<rr::Int>(samplerDescriptor + OFFSET(vk::SampledImageDescriptor, samplerId));  // vk::Sampler::id

	// Above we assumed that if the SampledImage operand is not the result of an OpSampledImage,
	// it must be a combined image sampler loaded straight from the descriptor set. For OpImageFetch
	// it's just an Image operand, so there's no sampler descriptor data.
	if(getType(sampledImage).opcode() != spv::OpTypeSampledImage)
	{
		samplerId = Int(0);
	}

	auto &cache = state->routine->samplerCache.at(instruction.position);
	auto cacheHit = (cache.imageDescriptor == imageDescriptor) && (cache.samplerId == samplerId);  // TODO(b/205566405): Skip sampler ID check for samplerless instructions.

	If(!cacheHit)
	{
		rr::Int imageViewId = *Pointer<rr::Int>(imageDescriptor + OFFSET(vk::SampledImageDescriptor, imageViewId));
		Pointer<Byte> device = *Pointer<Pointer<Byte>>(imageDescriptor + OFFSET(vk::SampledImageDescriptor, device));
		cache.function = Call(getImageSampler, device, instruction.state, samplerId, imageViewId);
		cache.imageDescriptor = imageDescriptor;
		cache.samplerId = samplerId;
	}

	return cache.function;
}

void SpirvShader::callSamplerFunction(Pointer<Byte> samplerFunction, Array<SIMD::Float> &out, Pointer<Byte> imageDescriptor, const ImageInstruction &instruction, EmitState *state) const
{
	Array<SIMD::Float> in(16);  // Maximum 16 input parameter components.

	auto coordinate = Operand(this, state, instruction.coordinateId);

	uint32_t i = 0;
	for(; i < instruction.coordinates; i++)
	{
		if(instruction.isProj())
		{
			in[i] = coordinate.Float(i) / coordinate.Float(instruction.coordinates);  // TODO(b/129523279): Optimize using reciprocal.
		}
		else
		{
			in[i] = coordinate.Float(i);
		}
	}

	if(instruction.isDref())
	{
		auto drefValue = Operand(this, state, instruction.drefId);

		if(instruction.isProj())
		{
			in[i] = drefValue.Float(0) / coordinate.Float(instruction.coordinates);  // TODO(b/129523279): Optimize using reciprocal.
		}
		else
		{
			in[i] = drefValue.Float(0);
		}

		i++;
	}

	if(instruction.lodOrBiasId != 0)
	{
		auto lodValue = Operand(this, state, instruction.lodOrBiasId);
		in[i] = lodValue.Float(0);
		i++;
	}
	else if(instruction.gradDxId != 0)
	{
		auto dxValue = Operand(this, state, instruction.gradDxId);
		auto dyValue = Operand(this, state, instruction.gradDyId);
		ASSERT(dxValue.componentCount == dxValue.componentCount);

		for(uint32_t j = 0; j < dxValue.componentCount; j++, i++)
		{
			in[i] = dxValue.Float(j);
		}

		for(uint32_t j = 0; j < dxValue.componentCount; j++, i++)
		{
			in[i] = dyValue.Float(j);
		}
	}
	else if(instruction.samplerMethod == Fetch)
	{
		// The instruction didn't provide a lod operand, but the sampler's Fetch
		// function requires one to be present. If no lod is supplied, the default
		// is zero.
		in[i] = As<SIMD::Float>(SIMD::Int(0));
		i++;
	}

	if(instruction.offsetId != 0)
	{
		auto offsetValue = Operand(this, state, instruction.offsetId);

		for(uint32_t j = 0; j < offsetValue.componentCount; j++, i++)
		{
			in[i] = As<SIMD::Float>(offsetValue.Int(j));  // Integer values, but transfered as float.
		}
	}

	if(instruction.sample)
	{
		auto sampleValue = Operand(this, state, instruction.sampleId);
		in[i] = As<SIMD::Float>(sampleValue.Int(0));
	}

	Pointer<Byte> texture = imageDescriptor + OFFSET(vk::SampledImageDescriptor, texture);  // sw::Texture*

	Call<ImageSampler>(samplerFunction, texture, &in[0], &out[0], state->routine->constants);
}

SpirvShader::EmitResult SpirvShader::EmitImageQuerySizeLod(InsnIterator insn, EmitState *state) const
{
	auto &resultTy = getType(insn.resultTypeId());
	auto imageId = Object::ID(insn.word(3));
	auto lodId = Object::ID(insn.word(4));

	auto &dst = state->createIntermediate(insn.resultId(), resultTy.componentCount);
	GetImageDimensions(state, resultTy, imageId, lodId, dst);

	return EmitResult::Continue;
}

SpirvShader::EmitResult SpirvShader::EmitImageQuerySize(InsnIterator insn, EmitState *state) const
{
	auto &resultTy = getType(insn.resultTypeId());
	auto imageId = Object::ID(insn.word(3));
	auto lodId = Object::ID(0);

	auto &dst = state->createIntermediate(insn.resultId(), resultTy.componentCount);
	GetImageDimensions(state, resultTy, imageId, lodId, dst);

	return EmitResult::Continue;
}

void SpirvShader::GetImageDimensions(EmitState const *state, Type const &resultTy, Object::ID imageId, Object::ID lodId, Intermediate &dst) const
{
	auto routine = state->routine;
	auto &image = getObject(imageId);
	auto &imageType = getType(image);

	ASSERT(imageType.definition.opcode() == spv::OpTypeImage);
	bool isArrayed = imageType.definition.word(5) != 0;
	uint32_t dimensions = resultTy.componentCount - (isArrayed ? 1 : 0);

	const DescriptorDecorations &d = descriptorDecorations.at(imageId);
	auto descriptorType = routine->pipelineLayout->getDescriptorType(d.DescriptorSet, d.Binding);

	Pointer<Byte> descriptor = state->getPointer(imageId).base;

	Int width;
	Int height;
	Int depth;

	switch(descriptorType)
	{
	case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
	case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
		width = *Pointer<Int>(descriptor + OFFSET(vk::StorageImageDescriptor, width));
		height = *Pointer<Int>(descriptor + OFFSET(vk::StorageImageDescriptor, height));
		depth = *Pointer<Int>(descriptor + OFFSET(vk::StorageImageDescriptor, depth));
		break;
	case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
	case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
	case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		width = *Pointer<Int>(descriptor + OFFSET(vk::SampledImageDescriptor, width));
		height = *Pointer<Int>(descriptor + OFFSET(vk::SampledImageDescriptor, height));
		depth = *Pointer<Int>(descriptor + OFFSET(vk::SampledImageDescriptor, depth));
		break;
	default:
		UNREACHABLE("Image descriptorType: %d", int(descriptorType));
	}

	if(lodId != 0)
	{
		auto lodVal = Operand(this, state, lodId);
		ASSERT(lodVal.componentCount == 1);
		auto lod = lodVal.Int(0);
		auto one = SIMD::Int(1);

		if(dimensions >= 1) dst.move(0, Max(SIMD::Int(width) >> lod, one));
		if(dimensions >= 2) dst.move(1, Max(SIMD::Int(height) >> lod, one));
		if(dimensions >= 3) dst.move(2, Max(SIMD::Int(depth) >> lod, one));
	}
	else
	{

		if(dimensions >= 1) dst.move(0, SIMD::Int(width));
		if(dimensions >= 2) dst.move(1, SIMD::Int(height));
		if(dimensions >= 3) dst.move(2, SIMD::Int(depth));
	}

	if(isArrayed)
	{
		dst.move(dimensions, SIMD::Int(depth));
	}
}

SpirvShader::EmitResult SpirvShader::EmitImageQueryLevels(InsnIterator insn, EmitState *state) const
{
	auto &resultTy = getType(insn.resultTypeId());
	ASSERT(resultTy.componentCount == 1);
	auto imageId = Object::ID(insn.word(3));

	const DescriptorDecorations &d = descriptorDecorations.at(imageId);
	auto descriptorType = state->routine->pipelineLayout->getDescriptorType(d.DescriptorSet, d.Binding);

	Pointer<Byte> descriptor = state->getPointer(imageId).base;
	Int mipLevels = 0;
	switch(descriptorType)
	{
	case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
	case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
	case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		mipLevels = *Pointer<Int>(descriptor + OFFSET(vk::SampledImageDescriptor, mipLevels));  // uint32_t
		break;
	default:
		UNREACHABLE("Image descriptorType: %d", int(descriptorType));
	}

	auto &dst = state->createIntermediate(insn.resultId(), 1);
	dst.move(0, SIMD::Int(mipLevels));

	return EmitResult::Continue;
}

SpirvShader::EmitResult SpirvShader::EmitImageQuerySamples(InsnIterator insn, EmitState *state) const
{
	auto &resultTy = getType(insn.resultTypeId());
	ASSERT(resultTy.componentCount == 1);
	auto imageId = Object::ID(insn.word(3));
	auto imageTy = getObjectType(imageId);
	ASSERT(imageTy.definition.opcode() == spv::OpTypeImage);
	ASSERT(imageTy.definition.word(3) == spv::Dim2D);
	ASSERT(imageTy.definition.word(6 /* MS */) == 1);

	const DescriptorDecorations &d = descriptorDecorations.at(imageId);
	auto descriptorType = state->routine->pipelineLayout->getDescriptorType(d.DescriptorSet, d.Binding);

	Pointer<Byte> descriptor = state->getPointer(imageId).base;
	Int sampleCount = 0;
	switch(descriptorType)
	{
	case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		sampleCount = *Pointer<Int>(descriptor + OFFSET(vk::StorageImageDescriptor, sampleCount));  // uint32_t
		break;
	case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
	case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
	case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		sampleCount = *Pointer<Int>(descriptor + OFFSET(vk::SampledImageDescriptor, sampleCount));  // uint32_t
		break;
	default:
		UNREACHABLE("Image descriptorType: %d", int(descriptorType));
	}

	auto &dst = state->createIntermediate(insn.resultId(), 1);
	dst.move(0, SIMD::Int(sampleCount));

	return EmitResult::Continue;
}

SIMD::Pointer SpirvShader::GetTexelAddress(EmitState const *state, Pointer<Byte> imageBase, Int imageSizeInBytes, Operand const &coordinate, Type const &imageType, Pointer<Byte> descriptor, int texelSize, Object::ID sampleId, bool useStencilAspect, OutOfBoundsBehavior outOfBoundsBehavior) const
{
	auto routine = state->routine;
	bool isArrayed = imageType.definition.word(5) != 0;
	auto dim = static_cast<spv::Dim>(imageType.definition.word(3));
	int dims = coordinate.componentCount - (isArrayed ? 1 : 0);

	SIMD::Int u = coordinate.Int(0);
	SIMD::Int v = SIMD::Int(0);

	if(coordinate.componentCount > 1)
	{
		v = coordinate.Int(1);
	}

	if(dim == spv::DimSubpassData)
	{
		u += routine->windowSpacePosition[0];
		v += routine->windowSpacePosition[1];
	}

	auto rowPitch = SIMD::Int(*Pointer<Int>(descriptor + (useStencilAspect
	                                                          ? OFFSET(vk::StorageImageDescriptor, stencilRowPitchBytes)
	                                                          : OFFSET(vk::StorageImageDescriptor, rowPitchBytes))));
	auto slicePitch = SIMD::Int(
	    *Pointer<Int>(descriptor + (useStencilAspect
	                                    ? OFFSET(vk::StorageImageDescriptor, stencilSlicePitchBytes)
	                                    : OFFSET(vk::StorageImageDescriptor, slicePitchBytes))));
	auto samplePitch = SIMD::Int(
	    *Pointer<Int>(descriptor + (useStencilAspect
	                                    ? OFFSET(vk::StorageImageDescriptor, stencilSamplePitchBytes)
	                                    : OFFSET(vk::StorageImageDescriptor, samplePitchBytes))));

	SIMD::Int ptrOffset = u * SIMD::Int(texelSize);

	if(dims > 1)
	{
		ptrOffset += v * rowPitch;
	}

	SIMD::Int w = 0;
	if((dims > 2) || isArrayed)
	{
		if(dims > 2)
		{
			w += coordinate.Int(2);
		}

		if(isArrayed)
		{
			w += coordinate.Int(dims);
		}

		ptrOffset += w * slicePitch;
	}

	if(dim == spv::DimSubpassData)
	{
		// Multiview input attachment access is to the layer corresponding to the current view
		ptrOffset += SIMD::Int(routine->viewID) * slicePitch;
	}

	SIMD::Int n = 0;
	if(sampleId != 0)
	{
		Operand sample(this, state, sampleId);
		if(!sample.isConstantZero())
		{
			n = sample.Int(0);
			ptrOffset += n * samplePitch;
		}
	}

	// If the out-of-bounds behavior is set to nullify, then each coordinate must be tested individually.
	// Other out-of-bounds behaviors work properly by just comparing the offset against the total size.
	if(outOfBoundsBehavior == OutOfBoundsBehavior::Nullify)
	{
		SIMD::UInt width = *Pointer<UInt>(descriptor + OFFSET(vk::StorageImageDescriptor, width));
		SIMD::Int oobMask = As<SIMD::Int>(CmpNLT(As<SIMD::UInt>(u), width));

		if(dims > 1)
		{
			SIMD::UInt height = *Pointer<UInt>(descriptor + OFFSET(vk::StorageImageDescriptor, height));
			oobMask |= As<SIMD::Int>(CmpNLT(As<SIMD::UInt>(v), height));
		}

		if((dims > 2) || isArrayed)
		{
			UInt depth = *Pointer<UInt>(descriptor + OFFSET(vk::StorageImageDescriptor, depth));
			if(dim == spv::DimCube) { depth *= 6; }
			oobMask |= As<SIMD::Int>(CmpNLT(As<SIMD::UInt>(w), SIMD::UInt(depth)));
		}

		if(sampleId != 0)
		{
			Operand sample(this, state, sampleId);
			if(!sample.isConstantZero())
			{
				SIMD::UInt sampleCount = *Pointer<UInt>(descriptor + OFFSET(vk::StorageImageDescriptor, sampleCount));
				oobMask |= As<SIMD::Int>(CmpNLT(As<SIMD::UInt>(n), sampleCount));
			}
		}

		constexpr int32_t OOB_OFFSET = 0x7FFFFFFF - 16;  // SIMD pointer offsets are signed 32-bit, so this is the largest offset (for 16-byte texels).
		static_assert(OOB_OFFSET >= vk::MAX_MEMORY_ALLOCATION_SIZE, "the largest offset must be guaranteed to be out-of-bounds");

		ptrOffset = (ptrOffset & ~oobMask) | (oobMask & SIMD::Int(OOB_OFFSET));  // oob ? OOB_OFFSET : ptrOffset  // TODO: IfThenElse()
	}

	return SIMD::Pointer(imageBase, imageSizeInBytes, ptrOffset);
}

SpirvShader::EmitResult SpirvShader::EmitImageRead(InsnIterator insn, EmitState *state) const
{
	auto &resultType = getType(Type::ID(insn.word(1)));
	auto imageId = Object::ID(insn.word(3));
	auto &image = getObject(imageId);
	auto &imageType = getType(image);

	Object::ID sampleId = 0;

	if(insn.wordCount() > 5)
	{
		int operand = 6;
		uint32_t imageOperands = insn.word(5);
		if(imageOperands & spv::ImageOperandsSampleMask)
		{
			sampleId = insn.word(operand++);
			imageOperands &= ~spv::ImageOperandsSampleMask;
		}
		// TODO(b/174475384)
		if(imageOperands & spv::ImageOperandsZeroExtendMask)
		{
			imageOperands &= ~spv::ImageOperandsZeroExtendMask;
		}
		else if(imageOperands & spv::ImageOperandsSignExtendMask)
		{
			imageOperands &= ~spv::ImageOperandsSignExtendMask;
		}

		// Should be no remaining image operands.
		if(imageOperands != 0)
		{
			UNSUPPORTED("Image operands 0x%08X", imageOperands);
		}
	}

	ASSERT(imageType.definition.opcode() == spv::OpTypeImage);
	auto dim = static_cast<spv::Dim>(imageType.definition.word(3));

	auto coordinate = Operand(this, state, insn.word(4));
	const DescriptorDecorations &d = descriptorDecorations.at(imageId);

	// For subpass data, format in the instruction is spv::ImageFormatUnknown. Get it from
	// the renderpass data instead. In all other cases, we can use the format in the instruction.
	auto vkFormat = (dim == spv::DimSubpassData)
	                    ? inputAttachmentFormats[d.InputAttachmentIndex]
	                    : SpirvFormatToVulkanFormat(static_cast<spv::ImageFormat>(imageType.definition.word(8)));

	// Depth+Stencil image attachments select aspect based on the Sampled Type of the
	// OpTypeImage. If float, then we want the depth aspect. If int, we want the stencil aspect.
	auto useStencilAspect = (vkFormat == VK_FORMAT_D32_SFLOAT_S8_UINT &&
	                         getType(imageType.definition.word(2)).opcode() == spv::OpTypeInt);

	if(useStencilAspect)
	{
		vkFormat = VK_FORMAT_S8_UINT;
	}

	auto pointer = state->getPointer(imageId);
	Pointer<Byte> binding = pointer.base;
	Pointer<Byte> imageBase = *Pointer<Pointer<Byte>>(binding + (useStencilAspect
	                                                                 ? OFFSET(vk::StorageImageDescriptor, stencilPtr)
	                                                                 : OFFSET(vk::StorageImageDescriptor, ptr)));

	auto imageSizeInBytes = *Pointer<Int>(binding + OFFSET(vk::StorageImageDescriptor, sizeInBytes));

	auto &dst = state->createIntermediate(insn.resultId(), resultType.componentCount);

	// VK_EXT_image_robustness requires replacing out-of-bounds access with zero.
	// TODO(b/162327166): Only perform bounds checks when VK_EXT_image_robustness is enabled.
	auto robustness = OutOfBoundsBehavior::Nullify;

	auto texelSize = vk::Format(vkFormat).bytes();
	auto texelPtr = GetTexelAddress(state, imageBase, imageSizeInBytes, coordinate, imageType, binding, texelSize, sampleId, useStencilAspect, robustness);

	// Gather packed texel data. Texels larger than 4 bytes occupy multiple SIMD::Int elements.
	// TODO(b/160531165): Provide gather abstractions for various element sizes.
	SIMD::Int packed[4];
	if(texelSize == 4 || texelSize == 8 || texelSize == 16)
	{
		for(auto i = 0; i < texelSize / 4; i++)
		{
			packed[i] = texelPtr.Load<SIMD::Int>(robustness, state->activeLaneMask());
			texelPtr += sizeof(float);
		}
	}
	else if(texelSize == 2)
	{
		SIMD::Int offsets = texelPtr.offsets();
		SIMD::Int mask = state->activeLaneMask() & texelPtr.isInBounds(2, robustness);

		for(int i = 0; i < SIMD::Width; i++)
		{
			If(Extract(mask, i) != 0)
			{
				packed[0] = Insert(packed[0], Int(*Pointer<Short>(texelPtr.base + Extract(offsets, i))), i);
			}
		}
	}
	else if(texelSize == 1)
	{
		SIMD::Int offsets = texelPtr.offsets();
		SIMD::Int mask = state->activeLaneMask() & texelPtr.isInBounds(1, robustness);

		for(int i = 0; i < SIMD::Width; i++)
		{
			If(Extract(mask, i) != 0)
			{
				packed[0] = Insert(packed[0], Int(*Pointer<Byte>(texelPtr.base + Extract(offsets, i))), i);
			}
		}
	}
	else
		UNREACHABLE("texelSize: %d", int(texelSize));

	// Format support requirements here come from two sources:
	// - Minimum required set of formats for loads from storage images
	// - Any format supported as a color or depth/stencil attachment, for input attachments
	switch(vkFormat)
	{
	case VK_FORMAT_R32G32B32A32_SFLOAT:
	case VK_FORMAT_R32G32B32A32_SINT:
	case VK_FORMAT_R32G32B32A32_UINT:
		dst.move(0, packed[0]);
		dst.move(1, packed[1]);
		dst.move(2, packed[2]);
		dst.move(3, packed[3]);
		break;
	case VK_FORMAT_R32_SINT:
	case VK_FORMAT_R32_UINT:
		dst.move(0, packed[0]);
		// Fill remaining channels with 0,0,1 (of the correct type)
		dst.move(1, SIMD::Int(0));
		dst.move(2, SIMD::Int(0));
		dst.move(3, SIMD::Int(1));
		break;
	case VK_FORMAT_R32_SFLOAT:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		dst.move(0, packed[0]);
		// Fill remaining channels with 0,0,1 (of the correct type)
		dst.move(1, SIMD::Float(0.0f));
		dst.move(2, SIMD::Float(0.0f));
		dst.move(3, SIMD::Float(1.0f));
		break;
	case VK_FORMAT_D16_UNORM:
		dst.move(0, SIMD::Float(packed[0] & SIMD::Int(0xFFFF)) * SIMD::Float(1.0f / 0xFFFF));
		dst.move(1, SIMD::Float(0.0f));
		dst.move(2, SIMD::Float(0.0f));
		dst.move(3, SIMD::Float(1.0f));
		break;
	case VK_FORMAT_R16G16B16A16_UNORM:
		dst.move(0, SIMD::Float(packed[0] & SIMD::Int(0xFFFF)) * SIMD::Float(1.0f / 0xFFFF));
		dst.move(1, SIMD::Float((packed[0] >> 16) & SIMD::Int(0xFFFF)) * SIMD::Float(1.0f / 0xFFFF));
		dst.move(2, SIMD::Float(packed[1] & SIMD::Int(0xFFFF)) * SIMD::Float(1.0f / 0xFFFF));
		dst.move(3, SIMD::Float((packed[1] >> 16) & SIMD::Int(0xFFFF)) * SIMD::Float(1.0f / 0xFFFF));
		break;
	case VK_FORMAT_R16G16B16A16_SNORM:
		dst.move(0, Max(SIMD::Float((packed[0] << 16) & SIMD::Int(0xFFFF0000)) * SIMD::Float(1.0f / 0x7FFF0000), SIMD::Float(-1.0f)));
		dst.move(1, Max(SIMD::Float(packed[0] & SIMD::Int(0xFFFF0000)) * SIMD::Float(1.0f / 0x7FFF0000), SIMD::Float(-1.0f)));
		dst.move(2, Max(SIMD::Float((packed[1] << 16) & SIMD::Int(0xFFFF0000)) * SIMD::Float(1.0f / 0x7FFF0000), SIMD::Float(-1.0f)));
		dst.move(3, Max(SIMD::Float(packed[1] & SIMD::Int(0xFFFF0000)) * SIMD::Float(1.0f / 0x7FFF0000), SIMD::Float(-1.0f)));
		break;
	case VK_FORMAT_R16G16B16A16_SINT:
		dst.move(0, (packed[0] << 16) >> 16);
		dst.move(1, packed[0] >> 16);
		dst.move(2, (packed[1] << 16) >> 16);
		dst.move(3, packed[1] >> 16);
		break;
	case VK_FORMAT_R16G16B16A16_UINT:
		dst.move(0, packed[0] & SIMD::Int(0xFFFF));
		dst.move(1, (packed[0] >> 16) & SIMD::Int(0xFFFF));
		dst.move(2, packed[1] & SIMD::Int(0xFFFF));
		dst.move(3, (packed[1] >> 16) & SIMD::Int(0xFFFF));
		break;
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		dst.move(0, halfToFloatBits(As<SIMD::UInt>(packed[0]) & SIMD::UInt(0x0000FFFF)));
		dst.move(1, halfToFloatBits((As<SIMD::UInt>(packed[0]) & SIMD::UInt(0xFFFF0000)) >> 16));
		dst.move(2, halfToFloatBits(As<SIMD::UInt>(packed[1]) & SIMD::UInt(0x0000FFFF)));
		dst.move(3, halfToFloatBits((As<SIMD::UInt>(packed[1]) & SIMD::UInt(0xFFFF0000)) >> 16));
		break;
	case VK_FORMAT_R8G8B8A8_SNORM:
	case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
		dst.move(0, Max(SIMD::Float((packed[0] << 24) & SIMD::Int(0xFF000000)) * SIMD::Float(1.0f / 0x7F000000), SIMD::Float(-1.0f)));
		dst.move(1, Max(SIMD::Float((packed[0] << 16) & SIMD::Int(0xFF000000)) * SIMD::Float(1.0f / 0x7F000000), SIMD::Float(-1.0f)));
		dst.move(2, Max(SIMD::Float((packed[0] << 8) & SIMD::Int(0xFF000000)) * SIMD::Float(1.0f / 0x7F000000), SIMD::Float(-1.0f)));
		dst.move(3, Max(SIMD::Float((packed[0]) & SIMD::Int(0xFF000000)) * SIMD::Float(1.0f / 0x7F000000), SIMD::Float(-1.0f)));
		break;
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
		dst.move(0, SIMD::Float(packed[0] & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF));
		dst.move(1, SIMD::Float((packed[0] >> 8) & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF));
		dst.move(2, SIMD::Float((packed[0] >> 16) & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF));
		dst.move(3, SIMD::Float((packed[0] >> 24) & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF));
		break;
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
		dst.move(0, ::sRGBtoLinear(SIMD::Float(packed[0] & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF)));
		dst.move(1, ::sRGBtoLinear(SIMD::Float((packed[0] >> 8) & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF)));
		dst.move(2, ::sRGBtoLinear(SIMD::Float((packed[0] >> 16) & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF)));
		dst.move(3, SIMD::Float((packed[0] >> 24) & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF));
		break;
	case VK_FORMAT_B8G8R8A8_UNORM:
		dst.move(0, SIMD::Float((packed[0] >> 16) & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF));
		dst.move(1, SIMD::Float((packed[0] >> 8) & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF));
		dst.move(2, SIMD::Float(packed[0] & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF));
		dst.move(3, SIMD::Float((packed[0] >> 24) & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF));
		break;
	case VK_FORMAT_B8G8R8A8_SRGB:
		dst.move(0, ::sRGBtoLinear(SIMD::Float((packed[0] >> 16) & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF)));
		dst.move(1, ::sRGBtoLinear(SIMD::Float((packed[0] >> 8) & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF)));
		dst.move(2, ::sRGBtoLinear(SIMD::Float(packed[0] & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF)));
		dst.move(3, SIMD::Float((packed[0] >> 24) & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF));
		break;
	case VK_FORMAT_R8G8B8A8_UINT:
	case VK_FORMAT_A8B8G8R8_UINT_PACK32:
		dst.move(0, As<SIMD::UInt>(packed[0]) & SIMD::UInt(0xFF));
		dst.move(1, (As<SIMD::UInt>(packed[0]) >> 8) & SIMD::UInt(0xFF));
		dst.move(2, (As<SIMD::UInt>(packed[0]) >> 16) & SIMD::UInt(0xFF));
		dst.move(3, (As<SIMD::UInt>(packed[0]) >> 24) & SIMD::UInt(0xFF));
		break;
	case VK_FORMAT_R8G8B8A8_SINT:
	case VK_FORMAT_A8B8G8R8_SINT_PACK32:
		dst.move(0, (packed[0] << 24) >> 24);
		dst.move(1, (packed[0] << 16) >> 24);
		dst.move(2, (packed[0] << 8) >> 24);
		dst.move(3, packed[0] >> 24);
		break;
	case VK_FORMAT_R8_UNORM:
		dst.move(0, SIMD::Float((packed[0] & SIMD::Int(0xFF))) * SIMD::Float(1.0f / 0xFF));
		dst.move(1, SIMD::Float(0.0f));
		dst.move(2, SIMD::Float(0.0f));
		dst.move(3, SIMD::Float(1.0f));
		break;
	case VK_FORMAT_R8_SNORM:
		dst.move(0, Max(SIMD::Float((packed[0] << 24) & SIMD::Int(0xFF000000)) * SIMD::Float(1.0f / 0x7F000000), SIMD::Float(-1.0f)));
		dst.move(1, SIMD::Float(0.0f));
		dst.move(2, SIMD::Float(0.0f));
		dst.move(3, SIMD::Float(1.0f));
		break;
	case VK_FORMAT_R8_UINT:
	case VK_FORMAT_S8_UINT:
		dst.move(0, As<SIMD::UInt>(packed[0]) & SIMD::UInt(0xFF));
		dst.move(1, SIMD::UInt(0));
		dst.move(2, SIMD::UInt(0));
		dst.move(3, SIMD::UInt(1));
		break;
	case VK_FORMAT_R8_SINT:
		dst.move(0, (packed[0] << 24) >> 24);
		dst.move(1, SIMD::Int(0));
		dst.move(2, SIMD::Int(0));
		dst.move(3, SIMD::Int(1));
		break;
	case VK_FORMAT_R8G8_UNORM:
		dst.move(0, SIMD::Float(packed[0] & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF));
		dst.move(1, SIMD::Float((packed[0] >> 8) & SIMD::Int(0xFF)) * SIMD::Float(1.0f / 0xFF));
		dst.move(2, SIMD::Float(0.0f));
		dst.move(3, SIMD::Float(1.0f));
		break;
	case VK_FORMAT_R8G8_SNORM:
		dst.move(0, Max(SIMD::Float((packed[0] << 24) & SIMD::Int(0xFF000000)) * SIMD::Float(1.0f / 0x7F000000), SIMD::Float(-1.0f)));
		dst.move(1, Max(SIMD::Float((packed[0] << 16) & SIMD::Int(0xFF000000)) * SIMD::Float(1.0f / 0x7F000000), SIMD::Float(-1.0f)));
		dst.move(2, SIMD::Float(0.0f));
		dst.move(3, SIMD::Float(1.0f));
		break;
	case VK_FORMAT_R8G8_UINT:
		dst.move(0, As<SIMD::UInt>(packed[0]) & SIMD::UInt(0xFF));
		dst.move(1, (As<SIMD::UInt>(packed[0]) >> 8) & SIMD::UInt(0xFF));
		dst.move(2, SIMD::UInt(0));
		dst.move(3, SIMD::UInt(1));
		break;
	case VK_FORMAT_R8G8_SINT:
		dst.move(0, (packed[0] << 24) >> 24);
		dst.move(1, (packed[0] << 16) >> 24);
		dst.move(2, SIMD::Int(0));
		dst.move(3, SIMD::Int(1));
		break;
	case VK_FORMAT_R16_SFLOAT:
		dst.move(0, halfToFloatBits(As<SIMD::UInt>(packed[0]) & SIMD::UInt(0x0000FFFF)));
		dst.move(1, SIMD::Float(0.0f));
		dst.move(2, SIMD::Float(0.0f));
		dst.move(3, SIMD::Float(1.0f));
		break;
	case VK_FORMAT_R16_UNORM:
		dst.move(0, SIMD::Float(packed[0] & SIMD::Int(0xFFFF)) * SIMD::Float(1.0f / 0xFFFF));
		dst.move(1, SIMD::Float(0.0f));
		dst.move(2, SIMD::Float(0.0f));
		dst.move(3, SIMD::Float(1.0f));
		break;
	case VK_FORMAT_R16_SNORM:
		dst.move(0, Max(SIMD::Float((packed[0] << 16) & SIMD::Int(0xFFFF0000)) * SIMD::Float(1.0f / 0x7FFF0000), SIMD::Float(-1.0f)));
		dst.move(1, SIMD::Float(0.0f));
		dst.move(2, SIMD::Float(0.0f));
		dst.move(3, SIMD::Float(1.0f));
		break;
	case VK_FORMAT_R16_UINT:
		dst.move(0, packed[0] & SIMD::Int(0xFFFF));
		dst.move(1, SIMD::UInt(0));
		dst.move(2, SIMD::UInt(0));
		dst.move(3, SIMD::UInt(1));
		break;
	case VK_FORMAT_R16_SINT:
		dst.move(0, (packed[0] << 16) >> 16);
		dst.move(1, SIMD::Int(0));
		dst.move(2, SIMD::Int(0));
		dst.move(3, SIMD::Int(1));
		break;
	case VK_FORMAT_R16G16_SFLOAT:
		dst.move(0, halfToFloatBits(As<SIMD::UInt>(packed[0]) & SIMD::UInt(0x0000FFFF)));
		dst.move(1, halfToFloatBits((As<SIMD::UInt>(packed[0]) & SIMD::UInt(0xFFFF0000)) >> 16));
		dst.move(2, SIMD::Float(0.0f));
		dst.move(3, SIMD::Float(1.0f));
		break;
	case VK_FORMAT_R16G16_UNORM:
		dst.move(0, SIMD::Float(packed[0] & SIMD::Int(0xFFFF)) * SIMD::Float(1.0f / 0xFFFF));
		dst.move(1, SIMD::Float(As<SIMD::UInt>(packed[0]) >> 16) * SIMD::Float(1.0f / 0xFFFF));
		dst.move(2, SIMD::Float(0.0f));
		dst.move(3, SIMD::Float(1.0f));
		break;
	case VK_FORMAT_R16G16_SNORM:
		dst.move(0, Max(SIMD::Float((packed[0] << 16) & SIMD::Int(0xFFFF0000)) * SIMD::Float(1.0f / 0x7FFF0000), SIMD::Float(-1.0f)));
		dst.move(1, Max(SIMD::Float(packed[0] & SIMD::Int(0xFFFF0000)) * SIMD::Float(1.0f / 0x7FFF0000), SIMD::Float(-1.0f)));
		dst.move(2, SIMD::Float(0.0f));
		dst.move(3, SIMD::Float(1.0f));
		break;
	case VK_FORMAT_R16G16_UINT:
		dst.move(0, packed[0] & SIMD::Int(0xFFFF));
		dst.move(1, (packed[0] >> 16) & SIMD::Int(0xFFFF));
		dst.move(2, SIMD::UInt(0));
		dst.move(3, SIMD::UInt(1));
		break;
	case VK_FORMAT_R16G16_SINT:
		dst.move(0, (packed[0] << 16) >> 16);
		dst.move(1, packed[0] >> 16);
		dst.move(2, SIMD::Int(0));
		dst.move(3, SIMD::Int(1));
		break;
	case VK_FORMAT_R32G32_SINT:
	case VK_FORMAT_R32G32_UINT:
		dst.move(0, packed[0]);
		dst.move(1, packed[1]);
		dst.move(2, SIMD::Int(0));
		dst.move(3, SIMD::Int(1));
		break;
	case VK_FORMAT_R32G32_SFLOAT:
		dst.move(0, packed[0]);
		dst.move(1, packed[1]);
		dst.move(2, SIMD::Float(0.0f));
		dst.move(3, SIMD::Float(1.0f));
		break;
	case VK_FORMAT_A2B10G10R10_UINT_PACK32:
		dst.move(0, packed[0] & SIMD::Int(0x3FF));
		dst.move(1, (packed[0] >> 10) & SIMD::Int(0x3FF));
		dst.move(2, (packed[0] >> 20) & SIMD::Int(0x3FF));
		dst.move(3, (packed[0] >> 30) & SIMD::Int(0x3));
		break;
	case VK_FORMAT_A2R10G10B10_UINT_PACK32:
		dst.move(2, packed[0] & SIMD::Int(0x3FF));
		dst.move(1, (packed[0] >> 10) & SIMD::Int(0x3FF));
		dst.move(0, (packed[0] >> 20) & SIMD::Int(0x3FF));
		dst.move(3, (packed[0] >> 30) & SIMD::Int(0x3));
		break;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		dst.move(0, SIMD::Float((packed[0]) & SIMD::Int(0x3FF)) * SIMD::Float(1.0f / 0x3FF));
		dst.move(1, SIMD::Float((packed[0] >> 10) & SIMD::Int(0x3FF)) * SIMD::Float(1.0f / 0x3FF));
		dst.move(2, SIMD::Float((packed[0] >> 20) & SIMD::Int(0x3FF)) * SIMD::Float(1.0f / 0x3FF));
		dst.move(3, SIMD::Float((packed[0] >> 30) & SIMD::Int(0x3)) * SIMD::Float(1.0f / 0x3));
		break;
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
		dst.move(2, SIMD::Float((packed[0]) & SIMD::Int(0x3FF)) * SIMD::Float(1.0f / 0x3FF));
		dst.move(1, SIMD::Float((packed[0] >> 10) & SIMD::Int(0x3FF)) * SIMD::Float(1.0f / 0x3FF));
		dst.move(0, SIMD::Float((packed[0] >> 20) & SIMD::Int(0x3FF)) * SIMD::Float(1.0f / 0x3FF));
		dst.move(3, SIMD::Float((packed[0] >> 30) & SIMD::Int(0x3)) * SIMD::Float(1.0f / 0x3));
		break;
	case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
		dst.move(0, SIMD::Float((packed[0] >> 12) & SIMD::Int(0xF)) * SIMD::Float(1.0f / 0xF));
		dst.move(1, SIMD::Float((packed[0] >> 8) & SIMD::Int(0xF)) * SIMD::Float(1.0f / 0xF));
		dst.move(2, SIMD::Float((packed[0] >> 4) & SIMD::Int(0xF)) * SIMD::Float(1.0f / 0xF));
		dst.move(3, SIMD::Float((packed[0]) & SIMD::Int(0xF)) * SIMD::Float(1.0f / 0xF));
		break;
	case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
		dst.move(0, SIMD::Float((packed[0] >> 4) & SIMD::Int(0xF)) * SIMD::Float(1.0f / 0xF));
		dst.move(1, SIMD::Float((packed[0] >> 8) & SIMD::Int(0xF)) * SIMD::Float(1.0f / 0xF));
		dst.move(2, SIMD::Float((packed[0] >> 12) & SIMD::Int(0xF)) * SIMD::Float(1.0f / 0xF));
		dst.move(3, SIMD::Float((packed[0]) & SIMD::Int(0xF)) * SIMD::Float(1.0f / 0xF));
		break;
	case VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT:
		dst.move(0, SIMD::Float((packed[0] >> 8) & SIMD::Int(0xF)) * SIMD::Float(1.0f / 0xF));
		dst.move(1, SIMD::Float((packed[0] >> 4) & SIMD::Int(0xF)) * SIMD::Float(1.0f / 0xF));
		dst.move(2, SIMD::Float((packed[0]) & SIMD::Int(0xF)) * SIMD::Float(1.0f / 0xF));
		dst.move(3, SIMD::Float((packed[0] >> 12) & SIMD::Int(0xF)) * SIMD::Float(1.0f / 0xF));
		break;
	case VK_FORMAT_A4B4G4R4_UNORM_PACK16_EXT:
		dst.move(0, SIMD::Float((packed[0]) & SIMD::Int(0xF)) * SIMD::Float(1.0f / 0xF));
		dst.move(1, SIMD::Float((packed[0] >> 4) & SIMD::Int(0xF)) * SIMD::Float(1.0f / 0xF));
		dst.move(2, SIMD::Float((packed[0] >> 8) & SIMD::Int(0xF)) * SIMD::Float(1.0f / 0xF));
		dst.move(3, SIMD::Float((packed[0] >> 12) & SIMD::Int(0xF)) * SIMD::Float(1.0f / 0xF));
		break;
	case VK_FORMAT_R5G6B5_UNORM_PACK16:
		dst.move(0, SIMD::Float((packed[0] >> 11) & SIMD::Int(0x1F)) * SIMD::Float(1.0f / 0x1F));
		dst.move(1, SIMD::Float((packed[0] >> 5) & SIMD::Int(0x3F)) * SIMD::Float(1.0f / 0x3F));
		dst.move(2, SIMD::Float((packed[0]) & SIMD::Int(0x1F)) * SIMD::Float(1.0f / 0x1F));
		dst.move(3, SIMD::Float(1.0f));
		break;
	case VK_FORMAT_B5G6R5_UNORM_PACK16:
		dst.move(0, SIMD::Float((packed[0]) & SIMD::Int(0x1F)) * SIMD::Float(1.0f / 0x1F));
		dst.move(1, SIMD::Float((packed[0] >> 5) & SIMD::Int(0x3F)) * SIMD::Float(1.0f / 0x3F));
		dst.move(2, SIMD::Float((packed[0] >> 11) & SIMD::Int(0x1F)) * SIMD::Float(1.0f / 0x1F));
		dst.move(3, SIMD::Float(1.0f));
		break;
	case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
		dst.move(0, SIMD::Float((packed[0] >> 11) & SIMD::Int(0x1F)) * SIMD::Float(1.0f / 0x1F));
		dst.move(1, SIMD::Float((packed[0] >> 6) & SIMD::Int(0x1F)) * SIMD::Float(1.0f / 0x1F));
		dst.move(2, SIMD::Float((packed[0] >> 1) & SIMD::Int(0x1F)) * SIMD::Float(1.0f / 0x1F));
		dst.move(3, SIMD::Float((packed[0]) & SIMD::Int(0x1)));
		break;
	case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
		dst.move(0, SIMD::Float((packed[0] >> 1) & SIMD::Int(0x1F)) * SIMD::Float(1.0f / 0x1F));
		dst.move(1, SIMD::Float((packed[0] >> 6) & SIMD::Int(0x1F)) * SIMD::Float(1.0f / 0x1F));
		dst.move(2, SIMD::Float((packed[0] >> 11) & SIMD::Int(0x1F)) * SIMD::Float(1.0f / 0x1F));
		dst.move(3, SIMD::Float((packed[0]) & SIMD::Int(0x1)));
		break;
	case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
		dst.move(0, SIMD::Float((packed[0] >> 10) & SIMD::Int(0x1F)) * SIMD::Float(1.0f / 0x1F));
		dst.move(1, SIMD::Float((packed[0] >> 5) & SIMD::Int(0x1F)) * SIMD::Float(1.0f / 0x1F));
		dst.move(2, SIMD::Float((packed[0]) & SIMD::Int(0x1F)) * SIMD::Float(1.0f / 0x1F));
		dst.move(3, SIMD::Float((packed[0] >> 15) & SIMD::Int(0x1)));
		break;
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		dst.move(0, halfToFloatBits((packed[0] << 4) & SIMD::Int(0x7FF0)));
		dst.move(1, halfToFloatBits((packed[0] >> 7) & SIMD::Int(0x7FF0)));
		dst.move(2, halfToFloatBits((packed[0] >> 17) & SIMD::Int(0x7FE0)));
		dst.move(3, SIMD::Float(1.0f));
		break;
	default:
		UNSUPPORTED("VkFormat %d", int(vkFormat));
		break;
	}

	return EmitResult::Continue;
}

SpirvShader::EmitResult SpirvShader::EmitImageWrite(InsnIterator insn, EmitState *state) const
{
	imageWriteEmitted = true;

	auto imageId = Object::ID(insn.word(1));
	auto &image = getObject(imageId);
	auto &imageType = getType(image);

	ASSERT(imageType.definition.opcode() == spv::OpTypeImage);

	Object::ID sampleId = 0;

	if(insn.wordCount() > 4)
	{
		int operand = 5;
		uint32_t imageOperands = insn.word(4);
		if(imageOperands & spv::ImageOperandsSampleMask)
		{
			sampleId = insn.word(operand++);
			imageOperands &= ~spv::ImageOperandsSampleMask;
		}
		// TODO(b/174475384)
		if(imageOperands & spv::ImageOperandsZeroExtendMask)
		{
			imageOperands &= ~spv::ImageOperandsZeroExtendMask;
		}
		else if(imageOperands & spv::ImageOperandsSignExtendMask)
		{
			imageOperands &= ~spv::ImageOperandsSignExtendMask;
		}

		// Should be no remaining image operands.
		if(imageOperands != 0)
		{
			UNSUPPORTED("Image operands 0x%08X", (int)imageOperands);
		}
	}

	auto coordinate = Operand(this, state, insn.word(2));
	auto texel = Operand(this, state, insn.word(3));

	Pointer<Byte> binding = state->getPointer(imageId).base;
	Pointer<Byte> imageBase = *Pointer<Pointer<Byte>>(binding + OFFSET(vk::StorageImageDescriptor, ptr));
	auto imageSizeInBytes = *Pointer<Int>(binding + OFFSET(vk::StorageImageDescriptor, sizeInBytes));

	SIMD::Int packed[4];
	int texelSize = 0;
	vk::Format format = SpirvFormatToVulkanFormat(static_cast<spv::ImageFormat>(imageType.definition.word(8)));
	switch(format)
	{
	case VK_FORMAT_R32G32B32A32_SFLOAT:
	case VK_FORMAT_R32G32B32A32_SINT:
	case VK_FORMAT_R32G32B32A32_UINT:
		texelSize = 16;
		packed[0] = texel.Int(0);
		packed[1] = texel.Int(1);
		packed[2] = texel.Int(2);
		packed[3] = texel.Int(3);
		break;
	case VK_FORMAT_R32_SFLOAT:
	case VK_FORMAT_R32_SINT:
	case VK_FORMAT_R32_UINT:
		texelSize = 4;
		packed[0] = texel.Int(0);
		break;
	case VK_FORMAT_R8G8B8A8_UNORM:
		texelSize = 4;
		packed[0] = (SIMD::UInt(Round(Min(Max(texel.Float(0), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(255.0f)))) |
		            ((SIMD::UInt(Round(Min(Max(texel.Float(1), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(255.0f)))) << 8) |
		            ((SIMD::UInt(Round(Min(Max(texel.Float(2), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(255.0f)))) << 16) |
		            ((SIMD::UInt(Round(Min(Max(texel.Float(3), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(255.0f)))) << 24);
		break;
	case VK_FORMAT_R8G8B8A8_SNORM:
		texelSize = 4;
		packed[0] = (SIMD::Int(Round(Min(Max(texel.Float(0), SIMD::Float(-1.0f)), SIMD::Float(1.0f)) * SIMD::Float(127.0f))) &
		             SIMD::Int(0xFF)) |
		            ((SIMD::Int(Round(Min(Max(texel.Float(1), SIMD::Float(-1.0f)), SIMD::Float(1.0f)) * SIMD::Float(127.0f))) &
		              SIMD::Int(0xFF))
		             << 8) |
		            ((SIMD::Int(Round(Min(Max(texel.Float(2), SIMD::Float(-1.0f)), SIMD::Float(1.0f)) * SIMD::Float(127.0f))) &
		              SIMD::Int(0xFF))
		             << 16) |
		            ((SIMD::Int(Round(Min(Max(texel.Float(3), SIMD::Float(-1.0f)), SIMD::Float(1.0f)) * SIMD::Float(127.0f))) &
		              SIMD::Int(0xFF))
		             << 24);
		break;
	case VK_FORMAT_R8G8B8A8_SINT:
	case VK_FORMAT_R8G8B8A8_UINT:
		texelSize = 4;
		packed[0] = (SIMD::UInt(texel.UInt(0) & SIMD::UInt(0xff))) |
		            (SIMD::UInt(texel.UInt(1) & SIMD::UInt(0xff)) << 8) |
		            (SIMD::UInt(texel.UInt(2) & SIMD::UInt(0xff)) << 16) |
		            (SIMD::UInt(texel.UInt(3) & SIMD::UInt(0xff)) << 24);
		break;
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		texelSize = 8;
		packed[0] = floatToHalfBits(texel.UInt(0), false) | floatToHalfBits(texel.UInt(1), true);
		packed[1] = floatToHalfBits(texel.UInt(2), false) | floatToHalfBits(texel.UInt(3), true);
		break;
	case VK_FORMAT_R16G16B16A16_SINT:
	case VK_FORMAT_R16G16B16A16_UINT:
		texelSize = 8;
		packed[0] = SIMD::UInt(texel.UInt(0) & SIMD::UInt(0xFFFF)) | (SIMD::UInt(texel.UInt(1) & SIMD::UInt(0xFFFF)) << 16);
		packed[1] = SIMD::UInt(texel.UInt(2) & SIMD::UInt(0xFFFF)) | (SIMD::UInt(texel.UInt(3) & SIMD::UInt(0xFFFF)) << 16);
		break;
	case VK_FORMAT_R32G32_SFLOAT:
	case VK_FORMAT_R32G32_SINT:
	case VK_FORMAT_R32G32_UINT:
		texelSize = 8;
		packed[0] = texel.Int(0);
		packed[1] = texel.Int(1);
		break;
	case VK_FORMAT_R16G16_SFLOAT:
		texelSize = 4;
		packed[0] = floatToHalfBits(texel.UInt(0), false) | floatToHalfBits(texel.UInt(1), true);
		break;
	case VK_FORMAT_R16G16_SINT:
	case VK_FORMAT_R16G16_UINT:
		texelSize = 4;
		packed[0] = SIMD::UInt(texel.UInt(0) & SIMD::UInt(0xFFFF)) | (SIMD::UInt(texel.UInt(1) & SIMD::UInt(0xFFFF)) << 16);
		break;
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		texelSize = 4;
		// Truncates instead of rounding. See b/147900455
		packed[0] = ((floatToHalfBits(As<SIMD::UInt>(Max(texel.Float(0), SIMD::Float(0.0f))), false) & SIMD::UInt(0x7FF0)) >> 4) |
		            ((floatToHalfBits(As<SIMD::UInt>(Max(texel.Float(1), SIMD::Float(0.0f))), false) & SIMD::UInt(0x7FF0)) << 7) |
		            ((floatToHalfBits(As<SIMD::UInt>(Max(texel.Float(2), SIMD::Float(0.0f))), false) & SIMD::UInt(0x7FE0)) << 17);
		break;
	case VK_FORMAT_R16_SFLOAT:
		texelSize = 2;
		packed[0] = floatToHalfBits(texel.UInt(0), false);
		break;
	case VK_FORMAT_R16G16B16A16_UNORM:
		texelSize = 8;
		packed[0] = SIMD::UInt(Round(Min(Max(texel.Float(0), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(0xFFFF))) |
		            (SIMD::UInt(Round(Min(Max(texel.Float(1), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(0xFFFF))) << 16);
		packed[1] = SIMD::UInt(Round(Min(Max(texel.Float(2), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(0xFFFF))) |
		            (SIMD::UInt(Round(Min(Max(texel.Float(3), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(0xFFFF))) << 16);
		break;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		texelSize = 4;
		packed[0] = (SIMD::UInt(Round(Min(Max(texel.Float(0), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(0x3FF)))) |
		            ((SIMD::UInt(Round(Min(Max(texel.Float(1), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(0x3FF)))) << 10) |
		            ((SIMD::UInt(Round(Min(Max(texel.Float(2), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(0x3FF)))) << 20) |
		            ((SIMD::UInt(Round(Min(Max(texel.Float(3), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(0x3)))) << 30);
		break;
	case VK_FORMAT_R16G16_UNORM:
		texelSize = 4;
		packed[0] = SIMD::UInt(Round(Min(Max(texel.Float(0), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(0xFFFF))) |
		            (SIMD::UInt(Round(Min(Max(texel.Float(1), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(0xFFFF))) << 16);
		break;
	case VK_FORMAT_R8G8_UNORM:
		texelSize = 2;
		packed[0] = SIMD::UInt(Round(Min(Max(texel.Float(0), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(0xFF))) |
		            (SIMD::UInt(Round(Min(Max(texel.Float(1), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(0xFF))) << 8);
		break;
	case VK_FORMAT_R16_UNORM:
		texelSize = 2;
		packed[0] = SIMD::UInt(Round(Min(Max(texel.Float(0), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(0xFFFF)));
		break;
	case VK_FORMAT_R8_UNORM:
		texelSize = 1;
		packed[0] = SIMD::UInt(Round(Min(Max(texel.Float(0), SIMD::Float(0.0f)), SIMD::Float(1.0f)) * SIMD::Float(0xFF)));
		break;
	case VK_FORMAT_R16G16B16A16_SNORM:
		texelSize = 8;
		packed[0] = (SIMD::Int(Round(Min(Max(texel.Float(0), SIMD::Float(-1.0f)), SIMD::Float(1.0f)) * SIMD::Float(0x7FFF))) & SIMD::Int(0xFFFF)) |
		            (SIMD::Int(Round(Min(Max(texel.Float(1), SIMD::Float(-1.0f)), SIMD::Float(1.0f)) * SIMD::Float(0x7FFF))) << 16);
		packed[1] = (SIMD::Int(Round(Min(Max(texel.Float(2), SIMD::Float(-1.0f)), SIMD::Float(1.0f)) * SIMD::Float(0x7FFF))) & SIMD::Int(0xFFFF)) |
		            (SIMD::Int(Round(Min(Max(texel.Float(3), SIMD::Float(-1.0f)), SIMD::Float(1.0f)) * SIMD::Float(0x7FFF))) << 16);
		break;
	case VK_FORMAT_R16G16_SNORM:
		texelSize = 4;
		packed[0] = (SIMD::Int(Round(Min(Max(texel.Float(0), SIMD::Float(-1.0f)), SIMD::Float(1.0f)) * SIMD::Float(0x7FFF))) & SIMD::Int(0xFFFF)) |
		            (SIMD::Int(Round(Min(Max(texel.Float(1), SIMD::Float(-1.0f)), SIMD::Float(1.0f)) * SIMD::Float(0x7FFF))) << 16);
		break;
	case VK_FORMAT_R8G8_SNORM:
		texelSize = 2;
		packed[0] = (SIMD::Int(Round(Min(Max(texel.Float(0), SIMD::Float(-1.0f)), SIMD::Float(1.0f)) * SIMD::Float(0x7F))) & SIMD::Int(0xFF)) |
		            (SIMD::Int(Round(Min(Max(texel.Float(1), SIMD::Float(-1.0f)), SIMD::Float(1.0f)) * SIMD::Float(0x7F))) << 8);
		break;
	case VK_FORMAT_R16_SNORM:
		texelSize = 2;
		packed[0] = SIMD::Int(Round(Min(Max(texel.Float(0), SIMD::Float(-1.0f)), SIMD::Float(1.0f)) * SIMD::Float(0x7FFF)));
		break;
	case VK_FORMAT_R8_SNORM:
		texelSize = 1;
		packed[0] = SIMD::Int(Round(Min(Max(texel.Float(0), SIMD::Float(-1.0f)), SIMD::Float(1.0f)) * SIMD::Float(0x7F)));
		break;
	case VK_FORMAT_R8G8_SINT:
	case VK_FORMAT_R8G8_UINT:
		texelSize = 2;
		packed[0] = SIMD::UInt(texel.UInt(0) & SIMD::UInt(0xFF)) | (SIMD::UInt(texel.UInt(1) & SIMD::UInt(0xFF)) << 8);
		break;
	case VK_FORMAT_R16_SINT:
	case VK_FORMAT_R16_UINT:
		texelSize = 2;
		packed[0] = SIMD::UInt(texel.UInt(0) & SIMD::UInt(0xFFFF));
		break;
	case VK_FORMAT_R8_SINT:
	case VK_FORMAT_R8_UINT:
		texelSize = 1;
		packed[0] = SIMD::UInt(texel.UInt(0) & SIMD::UInt(0xFF));
		break;
	case VK_FORMAT_A2B10G10R10_UINT_PACK32:
		texelSize = 4;
		packed[0] = (SIMD::UInt(texel.UInt(0) & SIMD::UInt(0x3FF))) |
		            (SIMD::UInt(texel.UInt(1) & SIMD::UInt(0x3FF)) << 10) |
		            (SIMD::UInt(texel.UInt(2) & SIMD::UInt(0x3FF)) << 20) |
		            (SIMD::UInt(texel.UInt(3) & SIMD::UInt(0x3)) << 30);
		break;
	default:
		UNSUPPORTED("VkFormat %d", int(format));
		break;
	}

	// "The integer texel coordinates are validated according to the same rules as for texel input coordinate
	//  validation. If the texel fails integer texel coordinate validation, then the write has no effect."
	// - https://www.khronos.org/registry/vulkan/specs/1.2/html/chap16.html#textures-output-coordinate-validation
	auto robustness = OutOfBoundsBehavior::Nullify;

	auto texelPtr = GetTexelAddress(state, imageBase, imageSizeInBytes, coordinate, imageType, binding, texelSize, sampleId, false, robustness);

	// Scatter packed texel data.
	// TODO(b/160531165): Provide scatter abstractions for various element sizes.
	if(texelSize == 4 || texelSize == 8 || texelSize == 16)
	{
		for(auto i = 0; i < texelSize / 4; i++)
		{
			texelPtr.Store(packed[i], robustness, state->activeStoresAndAtomicsMask());
			texelPtr += sizeof(float);
		}
	}
	else if(texelSize == 2)
	{
		SIMD::Int offsets = texelPtr.offsets();
		SIMD::Int mask = state->activeStoresAndAtomicsMask() & texelPtr.isInBounds(2, robustness);

		for(int i = 0; i < SIMD::Width; i++)
		{
			If(Extract(mask, i) != 0)
			{
				*Pointer<Short>(texelPtr.base + Extract(offsets, i)) = Short(Extract(packed[0], i));
			}
		}
	}
	else if(texelSize == 1)
	{
		SIMD::Int offsets = texelPtr.offsets();
		SIMD::Int mask = state->activeStoresAndAtomicsMask() & texelPtr.isInBounds(1, robustness);

		for(int i = 0; i < SIMD::Width; i++)
		{
			If(Extract(mask, i) != 0)
			{
				*Pointer<Byte>(texelPtr.base + Extract(offsets, i)) = Byte(Extract(packed[0], i));
			}
		}
	}
	else
		UNREACHABLE("texelSize: %d", int(texelSize));

	return EmitResult::Continue;
}

SpirvShader::EmitResult SpirvShader::EmitImageTexelPointer(InsnIterator insn, EmitState *state) const
{
	auto &resultType = getType(Type::ID(insn.word(1)));
	auto imageId = Object::ID(insn.word(3));
	auto &image = getObject(imageId);
	// Note: OpImageTexelPointer is unusual in that the image is passed by pointer.
	// Look through to get the actual image type.
	auto &imageType = getType(getType(image).element);
	Object::ID resultId = insn.word(2);

	ASSERT(imageType.opcode() == spv::OpTypeImage);
	ASSERT(resultType.storageClass == spv::StorageClassImage);
	ASSERT(getType(resultType.element).opcode() == spv::OpTypeInt);

	auto coordinate = Operand(this, state, insn.word(4));
	Object::ID sampleId = insn.word(5);

	Pointer<Byte> binding = state->getPointer(imageId).base;
	Pointer<Byte> imageBase = *Pointer<Pointer<Byte>>(binding + OFFSET(vk::StorageImageDescriptor, ptr));
	auto imageSizeInBytes = *Pointer<Int>(binding + OFFSET(vk::StorageImageDescriptor, sizeInBytes));

	// VK_EXT_image_robustness requires checking for out-of-bounds accesses.
	// TODO(b/162327166): Only perform bounds checks when VK_EXT_image_robustness is enabled.
	auto robustness = OutOfBoundsBehavior::Nullify;

	auto ptr = GetTexelAddress(state, imageBase, imageSizeInBytes, coordinate, imageType, binding, sizeof(uint32_t), sampleId, false, robustness);

	state->createPointer(resultId, ptr);

	return EmitResult::Continue;
}

SpirvShader::EmitResult SpirvShader::EmitSampledImageCombineOrSplit(InsnIterator insn, EmitState *state) const
{
	// Propagate the image pointer in both cases.
	// Consumers of OpSampledImage will look through to find the sampler pointer.

	Object::ID resultId = insn.word(2);
	Object::ID imageId = insn.word(3);

	state->createPointer(resultId, state->getPointer(imageId));

	return EmitResult::Continue;
}

}  // namespace sw