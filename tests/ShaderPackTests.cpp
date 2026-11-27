#include "graphics/host_gpu/renderer/pipeline/shaderPack.h"

#include "graphics/guest_gpu/pm4.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <xxhash.h>

using namespace Libs::Graphics;

namespace {

void AppendU32(std::vector<uint8_t>& dst, uint32_t value) {
	const auto offset = dst.size();
	dst.resize(offset + sizeof(value));
	std::memcpy(dst.data() + offset, &value, sizeof(value));
}

void AppendBytes(std::vector<uint8_t>& dst, const void* data, size_t size) {
	const auto offset = dst.size();
	dst.resize(offset + size);
	std::memcpy(dst.data() + offset, data, size);
}

struct SyntheticRecordArgs {
	Prospero::ShaderBinaryType  type                = Prospero::ShaderBinaryType::kCs;
	uint16_t                    scratch_dwords      = 0;
	std::vector<ShaderRegister> sh_registers;
	std::vector<ShaderRegister> cx_registers;
	bool                        include_specials    = false;
	uint32_t                    dispatch_modifier   = 0;
	std::vector<uint32_t>       code_words;
	std::vector<uint32_t>       literal_words;
};

// Builds a synthetic sgaProg record with an AGC Shader header ("1234", v0x18) and barefoot footer.
std::vector<uint8_t> BuildSyntheticRecord(const SyntheticRecordArgs& args) {
	// Align code to 16 bytes before appending literals and the 0x30-byte barefoot footer.
	std::vector<uint8_t> binary;
	AppendBytes(binary, args.code_words.data(), args.code_words.size() * sizeof(uint32_t));
	const auto code_bytes = static_cast<uint32_t>(binary.size());
	while (binary.size() % 16u != 0u) {
		binary.push_back(0);
	}
	const auto literal_bytes = static_cast<uint32_t>(args.literal_words.size() * sizeof(uint32_t));
	if (literal_bytes != 0u) {
		AppendBytes(binary, args.literal_words.data(), literal_bytes);
	}
	// Extra padding between program and footer to verify footer-based sizing.
	AppendU32(binary, 0xdeadbeefu);

	std::array<uint32_t, 12> footer {};
	std::memcpy(footer.data(), "barefoot", 8);
	footer[4] = 1u;
	footer[5] = code_bytes;
	footer[6] = literal_bytes;
	AppendBytes(binary, footer.data(), sizeof(footer));

	// Build AGC Shader header with relative pointers inside the header blob.
	std::vector<uint8_t> header(sizeof(Shader), 0);
	auto set_rel_ptr = [&](size_t field_offset, size_t target_offset) {
		const auto delta = static_cast<uint64_t>(target_offset - field_offset);
		std::memcpy(header.data() + field_offset, &delta, sizeof(delta));
	};

	if (!args.cx_registers.empty()) {
		const auto off = header.size();
		AppendBytes(header, args.cx_registers.data(),
		            args.cx_registers.size() * sizeof(ShaderRegister));
		set_rel_ptr(offsetof(Shader, cx_registers), off);
	}
	if (!args.sh_registers.empty()) {
		const auto off = header.size();
		AppendBytes(header, args.sh_registers.data(),
		            args.sh_registers.size() * sizeof(ShaderRegister));
		set_rel_ptr(offsetof(Shader, sh_registers), off);
	}
	if (args.include_specials) {
		ShaderSpecialRegs specials {};
		specials.dispatch_modifier = args.dispatch_modifier;
		const auto off             = header.size();
		AppendBytes(header, &specials, sizeof(specials));
		set_rel_ptr(offsetof(Shader, specials), off);
	}

	Shader hdr {};
	std::memcpy(&hdr, header.data(), sizeof(hdr));
	hdr.file_header                = 0x34333231u;
	hdr.version                    = 0x00000018u;
	hdr.header_size                = static_cast<uint32_t>(header.size());
	hdr.shader_size                = static_cast<uint32_t>(binary.size());
	hdr.scratch_size_dw_per_thread = args.scratch_dwords;
	hdr.type                       = static_cast<uint8_t>(args.type);
	hdr.num_cx_registers           = static_cast<uint8_t>(args.cx_registers.size());
	hdr.num_sh_registers           = static_cast<uint8_t>(args.sh_registers.size());
	std::memcpy(header.data(), &hdr, sizeof(hdr));

	std::vector<uint8_t> out;
	AppendBytes(out, "sgaProg\0", 8);
	AppendU32(out, static_cast<uint32_t>(header.size()));
	AppendU32(out, static_cast<uint32_t>(binary.size()));
	AppendBytes(out, header.data(), header.size());
	AppendBytes(out, binary.data(), binary.size());
	return out;
}

bool Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		return false;
	}
	return true;
}

} // namespace

int main() {
	std::vector<uint8_t> pack;
	AppendBytes(pack, "SGD5", 4);
	AppendU32(pack, 0);

	// 1. Valid compute shader (wave32 via dispatch_modifier bit 15 = 0x8000, or Pm4::ComputeWaveSize).
	const std::vector<uint32_t> cs_code = {0xbf810000u, 0xbf800000u, 0xbf800000u, 0xbf800000u};
	const uint32_t cs_rsrc1 = (0xc0u << Pm4::COMPUTE_PGM_RSRC1_FLOAT_MODE_SHIFT);
	const uint32_t cs_rsrc2 = (12u << Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_SHIFT) |
	                          (1u << Pm4::COMPUTE_PGM_RSRC2_TGID_X_EN_SHIFT) |
	                          (1u << Pm4::COMPUTE_PGM_RSRC2_TGID_Y_EN_SHIFT) |
	                          (1u << Pm4::COMPUTE_PGM_RSRC2_TG_SIZE_EN_SHIFT) |
	                          (2u << Pm4::COMPUTE_PGM_RSRC2_TIDIG_COMP_CNT_SHIFT) |
	                          (4u << Pm4::COMPUTE_PGM_RSRC2_LDS_SIZE_SHIFT);
	const auto cs_rec = BuildSyntheticRecord({
	    .type              = Prospero::ShaderBinaryType::kCs,
	    .scratch_dwords    = 16,
	    .sh_registers      = {{Pm4::COMPUTE_PGM_RSRC1, cs_rsrc1},
	                          {Pm4::COMPUTE_PGM_RSRC2, cs_rsrc2},
	                          {Pm4::COMPUTE_NUM_THREAD_X, 8},
	                          {Pm4::COMPUTE_NUM_THREAD_Y, 8},
	                          {Pm4::COMPUTE_NUM_THREAD_Z, 1}},
	    .include_specials  = true,
	    .dispatch_modifier = 1u << Pm4::COMPUTE_DISPATCH_INITIATOR_CS_W32_EN_SHIFT,
	    .code_words        = cs_code,
	});
	AppendBytes(pack, cs_rec.data(), cs_rec.size());
	// Duplicate compute record to verify hash deduplication.
	AppendBytes(pack, cs_rec.data(), cs_rec.size());

	// 2. Valid pixel shader (wave32 via SPI_PS_IN_CONTROL bit 15).
	const std::vector<uint32_t> ps_code = {0xbf810000u, 0xbf800001u, 0xbf800002u, 0xbf800003u};
	const uint32_t ps_rsrc1 = (0x80u << Pm4::SPI_SHADER_PGM_RSRC1_PS_FLOAT_MODE_SHIFT);
	const uint32_t ps_rsrc2 = (10u << Pm4::SPI_SHADER_PGM_RSRC2_PS_USER_SGPR_SHIFT) |
	                          (2u << Pm4::SPI_SHADER_PGM_RSRC2_PS_EXTRA_LDS_SIZE_SHIFT);
	const uint32_t ps_in_ctl = 3u | 0x8000u; // 3 interpolants, wave32
	const uint32_t ps_ena    = 0x00000302u;  // persp_center + pos_xy
	const auto ps_rec = BuildSyntheticRecord({
	    .type           = Prospero::ShaderBinaryType::kPs,
	    .scratch_dwords = 8,
	    .sh_registers   = {{Pm4::SPI_SHADER_PGM_RSRC1_PS, ps_rsrc1},
	                       {Pm4::SPI_SHADER_PGM_RSRC2_PS, ps_rsrc2}},
	    .cx_registers   = {{Pm4::SPI_PS_IN_CONTROL, ps_in_ctl},
	                       {Pm4::SPI_PS_INPUT_ENA, ps_ena},
	                       {Pm4::SPI_PS_INPUT_ADDR, ps_ena}},
	    .code_words     = ps_code,
	});
	AppendBytes(pack, ps_rec.data(), ps_rec.size());

	// 3. Truncated/corrupt record that must be ignored safely.
	AppendBytes(pack, "sgaProg\0", 8);
	AppendU32(pack, 0x1000u);
	AppendU32(pack, 0x1000u);

	const auto records = ScanShaderPack(pack);
	if (!Check(records.size() == 2u, "expected 2 deduplicated valid records")) return 1;

	const auto expected_cs_hash = XXH3_64bits(cs_code.data(), cs_code.size() * sizeof(uint32_t));
	const auto expected_ps_hash = XXH3_64bits(ps_code.data(), ps_code.size() * sizeof(uint32_t));
	if (!Check(records[0].hash == expected_cs_hash, "CS hash mismatch")) return 1;
	if (!Check(records[0].type == Prospero::ShaderBinaryType::kCs, "CS type mismatch")) return 1;
	if (!Check(records[0].code.size() == cs_code.size(), "CS code size mismatch")) return 1;

	ShaderComputeInputInfo cs_info {};
	uint32_t               cs_user_data = 0;
	if (!Check(ExtractPackComputeInput(records[0], 32u, cs_info, cs_user_data),
	           "ExtractPackComputeInput failed")) return 1;
	if (!Check(cs_user_data == 12u, "CS user_data_count mismatch")) return 1;
	if (!Check(cs_info.wave_size == 32u, "CS wave_size mismatch")) return 1;
	if (!Check(cs_info.threads_num[0] == 8u && cs_info.threads_num[1] == 8u &&
	               cs_info.threads_num[2] == 1u,
	           "CS threads_num mismatch")) return 1;
	if (!Check(cs_info.group_id[0] && cs_info.group_id[1] && !cs_info.group_id[2],
	           "CS group_id mismatch")) return 1;
	if (!Check(cs_info.tg_size_en && cs_info.thread_ids_num == 3 &&
	               cs_info.workgroup_register == 12 && cs_info.scratch_size_dwords == 16u,
	           "CS rsrc2/scratch mismatch")) return 1;

	if (!Check(records[1].hash == expected_ps_hash, "PS hash mismatch")) return 1;
	if (!Check(records[1].type == Prospero::ShaderBinaryType::kPs, "PS type mismatch")) return 1;

	ShaderPixelInputInfo ps_info {};
	uint32_t             ps_user_data = 0;
	if (!Check(ExtractPackPixelInput(records[1], ps_info, ps_user_data),
	           "ExtractPackPixelInput failed")) return 1;
	if (!Check(ps_user_data == 10u, "PS user_data_count mismatch")) return 1;
	if (!Check(ps_info.wave_size == 32u, "PS wave_size mismatch")) return 1;
	if (!Check(ps_info.float_mode == 0x80u && ps_info.scratch_size_dwords == 8u,
	           "PS float_mode/scratch mismatch")) return 1;
	if (!Check(ps_info.ps_perspective_center_vgpr == 0u && ps_info.ps_system_input_base == 2u &&
	               ps_info.ps_pos_x && ps_info.ps_pos_y && !ps_info.ps_pos_z,
	           "PS system input mapping mismatch")) return 1;

	std::printf("ShaderPackTests: all cases passed\n");
	return 0;
}
