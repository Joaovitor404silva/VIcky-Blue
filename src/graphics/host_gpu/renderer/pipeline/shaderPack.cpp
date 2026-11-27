#include "graphics/host_gpu/renderer/pipeline/shaderPack.h"

#include "common/alignment.h"
#include "graphics/guest_gpu/pm4.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <unordered_set>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

constexpr uint8_t  kProgMagic[8]      = {'s', 'g', 'a', 'P', 'r', 'o', 'g', '\0'};
constexpr uint32_t kAgcFileHeader     = 0x34333231u; // "1234"
constexpr uint32_t kAgcVersion        = 0x00000018u;
constexpr uint32_t kBarefootFooterLen = 0x30u;

template <typename T>
bool ReadValue(std::span<const uint8_t> bytes, size_t offset, T& out) {
	if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
		return false;
	}
	std::memcpy(&out, bytes.data() + offset, sizeof(T));
	return true;
}

uint32_t ProgramSizeFromFooter(std::span<const uint8_t> shader_bytes) {
	const auto size = static_cast<uint32_t>(shader_bytes.size());
	if (size < kBarefootFooterLen) {
		return size;
	}
	uint32_t footer[12] {};
	std::memcpy(footer, shader_bytes.data() + (size - kBarefootFooterLen), sizeof(footer));
	const auto end = Common::AlignUp(uint64_t {footer[5]}, 16u) + footer[6];
	if (std::memcmp(footer, "barefoot", 8) != 0 || footer[4] != 1u || footer[5] == 0u ||
	    end % sizeof(uint32_t) != 0u || end > size - kBarefootFooterLen) {
		return size;
	}
	return static_cast<uint32_t>(end);
}

// Resolves a relative pointer field inside an AGC header blob against the header's bounds.
bool ResolveHeaderRelativeOffset(std::span<const uint8_t> header_bytes, size_t field_offset,
                                 size_t elem_size, size_t count, size_t& target_offset) {
	uint64_t rel = 0;
	if (!ReadValue(header_bytes, field_offset, rel) || rel == 0u) {
		return false;
	}
	if (rel > header_bytes.size() || field_offset > header_bytes.size() - rel) {
		return false;
	}
	const auto target = field_offset + static_cast<size_t>(rel);
	if (count > (header_bytes.size() - target) / elem_size) {
		return false;
	}
	target_offset = target;
	return true;
}

uint32_t PsSystemInputBase(uint32_t ps_input_addr) {
	uint32_t reg = 0;
	if ((ps_input_addr & 0x00000001u) != 0u) reg += 2u; // persp_sample
	if ((ps_input_addr & 0x00000002u) != 0u) reg += 2u; // persp_center
	if ((ps_input_addr & 0x00000004u) != 0u) reg += 2u; // persp_centroid
	if ((ps_input_addr & 0x00000008u) != 0u) reg += 3u; // persp_pull
	if ((ps_input_addr & 0x00000010u) != 0u) reg += 2u; // linear_sample
	if ((ps_input_addr & 0x00000020u) != 0u) reg += 2u; // linear_center
	if ((ps_input_addr & 0x00000040u) != 0u) reg += 2u; // linear_centroid
	if ((ps_input_addr & 0x00000080u) != 0u) reg += 1u; // line_stipple
	return reg;
}

} // namespace

std::vector<ShaderPackRecord> ScanShaderPack(std::span<const uint8_t> bytes) {
	std::vector<ShaderPackRecord> records;
	std::unordered_set<uint64_t>  seen_hashes;

	size_t pos = 0;
	while (pos + sizeof(kProgMagic) <= bytes.size()) {
		const auto it = std::search(bytes.begin() + static_cast<ptrdiff_t>(pos), bytes.end(),
		                            std::begin(kProgMagic), std::end(kProgMagic));
		if (it == bytes.end()) {
			break;
		}
		const auto off = static_cast<size_t>(it - bytes.begin());
		pos            = off + 1u;

		uint32_t hdr_size    = 0;
		uint32_t shader_size = 0;
		if (!ReadValue(bytes, off + 8u, hdr_size) || !ReadValue(bytes, off + 12u, shader_size)) {
			continue;
		}
		if (hdr_size < sizeof(Shader) || shader_size == 0u ||
		    shader_size % sizeof(uint32_t) != 0u) {
			continue;
		}
		const size_t hdr_off = off + 16u;
		if (hdr_off > bytes.size() || hdr_size > bytes.size() - hdr_off) {
			continue;
		}
		const size_t code_off = hdr_off + hdr_size;
		if (code_off > bytes.size() || shader_size > bytes.size() - code_off) {
			continue;
		}

		const auto hdr_bytes = bytes.subspan(hdr_off, hdr_size);
		Shader     hdr {};
		std::memcpy(&hdr, hdr_bytes.data(), sizeof(hdr));
		if (hdr.file_header != kAgcFileHeader || hdr.version != kAgcVersion ||
		    hdr.header_size != hdr_size || hdr.shader_size != shader_size) {
			continue;
		}

		const auto code_bytes = bytes.subspan(code_off, shader_size);
		const auto prog_size  = ProgramSizeFromFooter(code_bytes);
		if (prog_size == 0u || prog_size % sizeof(uint32_t) != 0u || prog_size > shader_size) {
			continue;
		}

		const auto hash = XXH3_64bits(code_bytes.data(), prog_size);
		pos             = code_off + shader_size;
		if (!seen_hashes.insert(hash).second) {
			continue;
		}

		ShaderPackRecord rec;
		rec.hash                = hash;
		rec.type                = static_cast<Prospero::ShaderBinaryType>(hdr.type);
		rec.scratch_size_dwords = hdr.scratch_size_dw_per_thread;
		rec.code                = std::span<const uint32_t>(
		    reinterpret_cast<const uint32_t*>(code_bytes.data()), prog_size / sizeof(uint32_t));

		size_t sh_off = 0;
		if (hdr.num_sh_registers > 0u &&
		    ResolveHeaderRelativeOffset(hdr_bytes, offsetof(Shader, sh_registers),
		                                sizeof(ShaderRegister), hdr.num_sh_registers, sh_off)) {
			rec.sh_registers.resize(hdr.num_sh_registers);
			std::memcpy(rec.sh_registers.data(), hdr_bytes.data() + sh_off,
			            rec.sh_registers.size() * sizeof(ShaderRegister));
		}

		size_t cx_off = 0;
		if (hdr.num_cx_registers > 0u &&
		    ResolveHeaderRelativeOffset(hdr_bytes, offsetof(Shader, cx_registers),
		                                sizeof(ShaderRegister), hdr.num_cx_registers, cx_off)) {
			rec.cx_registers.resize(hdr.num_cx_registers);
			std::memcpy(rec.cx_registers.data(), hdr_bytes.data() + cx_off,
			            rec.cx_registers.size() * sizeof(ShaderRegister));
		}

		size_t specials_off = 0;
		if (ResolveHeaderRelativeOffset(hdr_bytes, offsetof(Shader, specials),
		                                sizeof(ShaderSpecialRegs), 1u, specials_off)) {
			ShaderSpecialRegs specials {};
			std::memcpy(&specials, hdr_bytes.data() + specials_off, sizeof(specials));
			rec.dispatch_modifier     = specials.dispatch_modifier;
			rec.has_dispatch_modifier = true;
		}

		records.push_back(std::move(rec));
	}

	return records;
}

bool ExtractPackComputeInput(const ShaderPackRecord& record, uint32_t host_subgroup_size,
                             ShaderComputeInputInfo& out, uint32_t& user_data_count) {
	if (record.type != Prospero::ShaderBinaryType::kCs) {
		return false;
	}
	bool has_rsrc1 = false;
	bool has_rsrc2 = false;
	out            = {};
	out.host_subgroup_size         = host_subgroup_size;
	out.scratch_size_dwords        = record.scratch_size_dwords;
	out.wave_size                  = record.has_dispatch_modifier
	                                     ? Pm4::ComputeWaveSize(record.dispatch_modifier)
	                                     : 64u;
	out.dispatch_thread_dimensions = false;

	for (const auto& reg: record.sh_registers) {
		switch (reg.offset) {
			case Pm4::COMPUTE_PGM_RSRC1:
				has_rsrc1      = true;
				out.float_mode = static_cast<uint8_t>((reg.value >> Pm4::COMPUTE_PGM_RSRC1_FLOAT_MODE_SHIFT) &
				                                      Pm4::COMPUTE_PGM_RSRC1_FLOAT_MODE_MASK);
				break;
			case Pm4::COMPUTE_PGM_RSRC2: {
				has_rsrc2 = true;
				const auto user_sgpr = (reg.value >> Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_SHIFT) &
				                       Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_MASK;
				user_data_count        = user_sgpr;
				out.workgroup_register = static_cast<int>(user_sgpr);
				out.group_id[0]        = ((reg.value >> Pm4::COMPUTE_PGM_RSRC2_TGID_X_EN_SHIFT) &
				                          Pm4::COMPUTE_PGM_RSRC2_TGID_X_EN_MASK) != 0u;
				out.group_id[1]        = ((reg.value >> Pm4::COMPUTE_PGM_RSRC2_TGID_Y_EN_SHIFT) &
				                          Pm4::COMPUTE_PGM_RSRC2_TGID_Y_EN_MASK) != 0u;
				out.group_id[2]        = ((reg.value >> Pm4::COMPUTE_PGM_RSRC2_TGID_Z_EN_SHIFT) &
				                          Pm4::COMPUTE_PGM_RSRC2_TGID_Z_EN_MASK) != 0u;
				out.tg_size_en         = ((reg.value >> Pm4::COMPUTE_PGM_RSRC2_TG_SIZE_EN_SHIFT) &
				                          Pm4::COMPUTE_PGM_RSRC2_TG_SIZE_EN_MASK) != 0u;
				const auto tidig       = (reg.value >> Pm4::COMPUTE_PGM_RSRC2_TIDIG_COMP_CNT_SHIFT) &
				                         Pm4::COMPUTE_PGM_RSRC2_TIDIG_COMP_CNT_MASK;
				out.thread_ids_num     = static_cast<int>(tidig + 1u);
				const auto lds_blocks  = (reg.value >> Pm4::COMPUTE_PGM_RSRC2_LDS_SIZE_SHIFT) &
				                         Pm4::COMPUTE_PGM_RSRC2_LDS_SIZE_MASK;
				out.lds_size_dwords    = lds_blocks * 128u;
				break;
			}
			case Pm4::COMPUTE_NUM_THREAD_X: out.threads_num[0] = reg.value; break;
			case Pm4::COMPUTE_NUM_THREAD_Y: out.threads_num[1] = reg.value; break;
			case Pm4::COMPUTE_NUM_THREAD_Z: out.threads_num[2] = reg.value; break;
			default: break;
		}
	}

	return has_rsrc1 && has_rsrc2 && (out.wave_size == 32u || out.wave_size == 64u);
}

bool ExtractPackPixelInput(const ShaderPackRecord& record, ShaderPixelInputInfo& out,
                           uint32_t& user_data_count) {
	if (record.type != Prospero::ShaderBinaryType::kPs) {
		return false;
	}
	bool     has_rsrc1     = false;
	bool     has_rsrc2     = false;
	uint32_t ps_in_control = 0;
	uint32_t ps_input_ena  = 0;
	uint32_t ps_input_addr = 0;
	out                    = {};
	out.scratch_size_dwords = record.scratch_size_dwords;

	for (const auto& reg: record.sh_registers) {
		switch (reg.offset) {
			case Pm4::SPI_SHADER_PGM_RSRC1_PS:
				has_rsrc1 = true;
				out.float_mode = static_cast<uint8_t>(
				    (reg.value >> Pm4::SPI_SHADER_PGM_RSRC1_PS_FLOAT_MODE_SHIFT) &
				    Pm4::SPI_SHADER_PGM_RSRC1_PS_FLOAT_MODE_MASK);
				break;
			case Pm4::SPI_SHADER_PGM_RSRC2_PS: {
				has_rsrc2 = true;
				const auto lo = (reg.value >> Pm4::SPI_SHADER_PGM_RSRC2_PS_USER_SGPR_SHIFT) &
				                Pm4::SPI_SHADER_PGM_RSRC2_PS_USER_SGPR_MASK;
				const auto msb = (reg.value >> Pm4::SPI_SHADER_PGM_RSRC2_PS_USER_SGPR_MSB_SHIFT) &
				                 Pm4::SPI_SHADER_PGM_RSRC2_PS_USER_SGPR_MSB_MASK;
				user_data_count = lo + (msb << 5u);
				const auto extra_lds =
				    (reg.value >> Pm4::SPI_SHADER_PGM_RSRC2_PS_EXTRA_LDS_SIZE_SHIFT) &
				    Pm4::SPI_SHADER_PGM_RSRC2_PS_EXTRA_LDS_SIZE_MASK;
				out.lds_size_dwords = extra_lds * 128u;
				break;
			}
			default: break;
		}
	}

	for (const auto& reg: record.cx_registers) {
		switch (reg.offset) {
			case Pm4::SPI_PS_IN_CONTROL: ps_in_control = reg.value; break;
			case Pm4::SPI_PS_INPUT_ENA: ps_input_ena = reg.value; break;
			case Pm4::SPI_PS_INPUT_ADDR: ps_input_addr = reg.value; break;
			default: break;
		}
	}

	if (!has_rsrc1 || !has_rsrc2) {
		return false;
	}
	if (ps_input_ena != ps_input_addr) {
		return false;
	}

	out.input_num = ps_in_control & 0x3fu;
	out.wave_size = (ps_in_control & 0x8000u) != 0u ? 32u : 64u;
	out.ps_system_input_base = PsSystemInputBase(ps_input_addr);

	const uint32_t active_inputs = ps_input_ena & ps_input_addr;
	if ((active_inputs & 0x00000001u) != 0u) {
		out.ps_perspective_sample_vgpr = 0u;
	}
	if ((active_inputs & 0x00000002u) != 0u) {
		out.ps_perspective_center_vgpr = (active_inputs & 0x00000001u) != 0u ? 2u : 0u;
	}
	if ((active_inputs & 0x00000004u) != 0u) {
		out.ps_perspective_centroid_vgpr = 2u * static_cast<uint32_t>(std::popcount(active_inputs & 0x3u));
	}
	out.ps_pos_x      = (active_inputs & 0x00000100u) != 0u;
	out.ps_pos_y      = (active_inputs & 0x00000200u) != 0u;
	out.ps_pos_z      = (active_inputs & 0x00000400u) != 0u;
	out.ps_pos_w      = (active_inputs & 0x00000800u) != 0u;
	out.ps_front_face = (active_inputs & 0x00001000u) != 0u;
	out.ps_ancillary  = (active_inputs & 0x00002000u) != 0u;
	return true;
}

} // namespace Libs::Graphics
