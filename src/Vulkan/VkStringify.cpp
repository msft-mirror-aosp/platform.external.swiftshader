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

#include "VkStringify.hpp"

#include "System/Debug.hpp"

#include <vulkan/vk_ext_provoking_vertex.h>
#include <vulkan/vk_google_filtering_precision.h>
#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_NAMESPACE vkhpp
#include <vulkan/vulkan.hpp>

namespace vk {

std::string Stringify(VkStructureType value)
{
#ifndef NDEBUG
	std::string ret = "";
	switch(static_cast<int>(value))
	{
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT:
		ret = "PhysicalDeviceProvokingVertexFeaturesEXT";
		break;
	case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT:
		ret = "PipelineRasterizationProvokingVertexStateCreateInfoEXT";
		break;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_PROPERTIES_EXT:
		ret = "PhysicalDeviceProvokingVertexPropertiesEXT";
		break;
	case VK_STRUCTURE_TYPE_SAMPLER_FILTERING_PRECISION_GOOGLE:
		ret = "SamplerFilteringPrecisionGOOGLE";
		break;
	default:
		ret = vkhpp::to_string(static_cast<vkhpp::StructureType>(value));
		break;
	}

	return ret;
#else
	return std::to_string(static_cast<int>(value));
#endif
}

}  // namespace vk
