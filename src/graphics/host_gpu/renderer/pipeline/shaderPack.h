#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERPACK_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERPACK_H_

#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/shader.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics {

// One deduplicated GPU shader record extracted from a raw shader pack file.
struct ShaderPackRecord {
	uint64_t                   hash                = 0;
	Prospero::ShaderBinaryType type                = Prospero::ShaderBinaryType::kCs;
	uint16_t                   scratch_size_dwords = 0;
	uint32_t                   dispatch_modifier   = 0;
	bool                       has_dispatch_modifier = false;
	std::span<const uint32_t>  code;
	std::vector<ShaderRegister> sh_registers;
	std::vector<ShaderRegister> cx_registers;
};

// Scans raw pack bytes for validated sgaProg records and returns one entry per unique code hash.
[[nodiscard]] std::vector<ShaderPackRecord> ScanShaderPack(std::span<const uint8_t> bytes);

// Populates the compute input fields that shader translation reads from a kCs pack record.
[[nodiscard]] bool ExtractPackComputeInput(const ShaderPackRecord& record,
                                           uint32_t                host_subgroup_size,
                                           ShaderComputeInputInfo& out,
                                           uint32_t&               user_data_count);

// Populates the pixel input fields that shader translation reads from a kPs pack record.
[[nodiscard]] bool ExtractPackPixelInput(const ShaderPackRecord& record,
                                         ShaderPixelInputInfo&   out,
                                         uint32_t&               user_data_count);

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERPACK_H_ */
