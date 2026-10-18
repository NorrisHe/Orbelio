#pragma once

// Allocation-backed resources only. Interfaces that just pass handles stay on device_context.hpp,
// so consumers of orbelio_vulkan_api never see vk_mem_alloc.h.
#include "device_context.hpp"
#include <vk_mem_alloc.h>
