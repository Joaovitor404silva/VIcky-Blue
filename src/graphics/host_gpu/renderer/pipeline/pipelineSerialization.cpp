#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/parallelFor.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/Serialize.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fmt/format.h>
#include <optional>
#include <ranges>
#include <type_traits>
#include <unordered_set>
#include <xxhash.h>
#include <zstd.h>

#include "kytyShaderStoreVersion.h"

// The shader store: a header, then zstd-compressed records of everything a run created first.
// Records are only appended, so a store cut short by a crash keeps every complete record.
namespace Libs::Graphics {

namespace {

using ShaderRecompiler::IR::IoAll;
using ShaderRecompiler::IR::Reader;
using ShaderRecompiler::IR::Writer;

enum class Record : uint8_t { Module, GraphicsPipeline, TessellationLayout };

// Precedes each record's zstd frame.
struct Frame {
	uint32_t compressed_size = 0;
	uint32_t raw_size        = 0;
	uint64_t checksum        = 0; // XXH3 of the compressed bytes.
};

// Hash of the recompiler and pipeline sources (shader_store_version.cmake): it changes with
// any change to how records are encoded or what they hold, and not with unrelated rebuilds.
uint64_t BuildStamp() {
	return KYTY_SHADER_STORE_VERSION;
}

// What every record depends on besides its own fields; empty when the build is unknown.
std::vector<uint8_t> StoreHeader(const GraphicContext& graphics) {
	const auto stamp = BuildStamp();
	if (stamp == 0) {
		return {};
	}
	Writer header;
	IoAll(header, stamp, SpirvProfile(graphics),
	      graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize);
	return std::move(header.bytes);
}

// Record fields, shared by writing and reading.
template <class S, class Key> void IoKey(S& io, Key& key) {
	auto& [stage, hash, user_data_count, static_state] = key;
	IoAll(io, stage, hash, user_data_count, static_state);
}

template <class S, class Module> void IoModule(S& io, Module& stage) {
	// The handle comes from the SPIR-V; stored programs have no function code.
	auto& [id, handle, info, specialization, function_code, rect_parameters, mesh_task] = stage;
	IoAll(io, id, info, specialization, rect_parameters, mesh_task);
}

// Takes the next frame off `records`, decompressed into `raw` unless that is null. False, with
// `records` unchanged, when the frame is cut short or damaged.
bool NextFrame(std::span<const uint8_t>& records, std::vector<uint8_t>* raw) {
	Frame frame;
	if (records.size() < sizeof(frame)) {
		return false;
	}
	std::memcpy(&frame, records.data(), sizeof(frame));
	if (records.size() - sizeof(frame) < frame.compressed_size) {
		return false;
	}
	const auto compressed = records.subspan(sizeof(frame), frame.compressed_size);
	if (XXH3_64bits(compressed.data(), compressed.size()) != frame.checksum ||
	    ZSTD_getFrameContentSize(compressed.data(), compressed.size()) != frame.raw_size) {
		return false;
	}
	if (raw != nullptr) {
		raw->resize(frame.raw_size);
		if (ZSTD_decompress(raw->data(), raw->size(), compressed.data(), compressed.size()) !=
		    raw->size()) {
			return false;
		}
	}
	records = records.subspan(sizeof(frame) + compressed.size());
	return true;
}

std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& path) {
	std::vector<uint8_t> bytes;
	if (Common::File file;
	    Common::File::IsFileExisting(path) && file.Open(path, Common::File::Mode::Read)) {
		bytes.resize(file.Size());
		uint32_t read = 0;
		file.Read(bytes.data(), static_cast<uint32_t>(bytes.size()), &read);
		bytes.resize(read);
	}
	return bytes;
}

// The recipes keep each module's inputs: the stage's code and input info, the resource
// specialization and the push-data window. Unlike the store they do not depend on the recompiler
// build, only on their own encoding, so a store of another build can be rebuilt from them.
// Bump RecipeFormatVersion when that encoding, or a type it copies as bytes, changes meaning.
constexpr uint32_t RecipeMagic         = 0x4352594bu; // "KYRC"
constexpr uint32_t RecipeFormatVersion = 1u;

std::vector<uint8_t> RecipeHeader() {
	Writer header;
	IoAll(header, RecipeMagic, RecipeFormatVersion,
	      static_cast<uint32_t>(sizeof(ShaderVertexInputInfo)),
	      static_cast<uint32_t>(sizeof(ShaderPixelInputInfo)),
	      static_cast<uint32_t>(sizeof(ShaderComputeInputInfo)),
	      static_cast<uint32_t>(sizeof(GraphicsPipelineKey)),
	      static_cast<uint32_t>(sizeof(ShaderTessellationInputInfo)));
	return std::move(header.bytes);
}

// A stage's input info as bytes, without its runtime pointers.
template <class Info> std::vector<uint8_t> InputInfoBytes(const Info& info) {
	static_assert(std::is_trivially_copyable_v<Info>);
	Info copy  = info;
	copy.stage = {};
	std::vector<uint8_t> bytes(sizeof(Info));
	std::memcpy(bytes.data(), &copy, sizeof(Info));
	return bytes;
}

template <class Info> bool InputInfoFromBytes(std::span<const uint8_t> bytes, Info& info) {
	static_assert(std::is_trivially_copyable_v<Info>);
	if (bytes.size() != sizeof(Info)) return false;
	std::memcpy(&info, bytes.data(), sizeof(Info));
	return true;
}

} // namespace

void ShaderStoreWriter::Open(const std::filesystem::path& path, std::span<const uint8_t> header,
                             uint64_t kept) {
	Common::File::CreateDirectories(path.parent_path());
	if (kept == 0) {
		if (m_file.Create(path)) {
			m_file.Write(header.data(), static_cast<uint32_t>(header.size()));
		}
	} else if (m_file.Open(path, Common::File::Mode::ReadWrite)) {
		m_file.Truncate(kept);
		m_file.Seek(kept);
	}
}

ShaderStoreWriter::ShaderStoreWriter() {
	m_thread = std::jthread([this](std::stop_token stop) {
		std::unique_lock lock(m_mutex);
		// Once stop is requested the wait returns false only when the queue is empty.
		while (m_ready.wait(lock, stop, [this] { return !m_jobs.empty(); })) {
			const auto job = std::move(m_jobs.front());
			m_jobs.pop();
			lock.unlock();
			job();
			lock.lock();
		}
	});
}

void ShaderStoreWriter::Append(std::vector<uint8_t>&& record) {
	Post([this, record = std::move(record)] {
		std::vector<uint8_t> bytes(sizeof(Frame) + ZSTD_compressBound(record.size()));
		const auto size = ZSTD_compress(bytes.data() + sizeof(Frame), bytes.size() - sizeof(Frame),
		                                record.data(), record.size(), 3);
		EXIT_IF(ZSTD_isError(size));
		const Frame frame {.compressed_size = static_cast<uint32_t>(size),
		                   .raw_size        = static_cast<uint32_t>(record.size()),
		                   .checksum        = XXH3_64bits(bytes.data() + sizeof(Frame), size)};
		std::memcpy(bytes.data(), &frame, sizeof(frame));
		// A failed write leaves a partial record; the next run drops it and everything after it.
		m_file.Write(bytes.data(), static_cast<uint32_t>(sizeof(Frame) + size));
	});
}

void ShaderStoreWriter::Post(Common::UniqueFunction<void>&& job) {
	{
		std::scoped_lock lock(m_mutex);
		m_jobs.push(std::move(job));
	}
	m_ready.notify_one();
}

void PipelineCache::WarmUp(const std::filesystem::path& path) {
	if (path.empty()) {
		return;
	}
	const auto header = StoreHeader(m_graphics);
	if (header.empty()) {
		return;
	}
	const auto start = std::chrono::steady_clock::now();
	const auto bytes = ReadWholeFile(path);
	Log::WriteToConsoleAndLog(fmt::format(
	    "Shader cache: compiling shaders and pipelines from {} ({:.1f} MB) with {} threads\n",
	    Common::PathToString(path), static_cast<double>(bytes.size()) / 1'000'000.0,
	    Common::ParallelThreads()));
	auto       records = std::span<const uint8_t>(bytes);
	const bool reusable =
	    records.size() >= header.size() && std::ranges::equal(records.first(header.size()), header);
	if (reusable) {
		records = records.subspan(header.size());
		while (LoadRecord(records)) {
		}
	}
	// A damaged record ends the store; the records before it stay.
	m_store.Open(path, header, reusable ? bytes.size() - records.size() : 0);
	// A store of another build, or none, is rebuilt from the recipes before the run.
	const auto recipes = OpenRecipes(path, !reusable);
	if (!reusable && !recipes.empty()) {
		RebuildFromRecipes(recipes);
	}

	const auto modules = MapModules();
	std::vector<Common::UniqueFunction<void>> jobs;
	jobs.reserve(m_graphics_pipelines.size() + modules.size());
	for (auto& node: m_graphics_pipelines) {
		jobs.emplace_back([this, &modules, &node] {
			CreateGraphicsPipeline(node.second, node.first, modules);
		});
	}
	// A compute pipeline depends only on its module.
	for (const auto* module: modules | std::views::values) {
		if (module != nullptr && module->info.stage == ShaderType::Compute) {
			jobs.emplace_back([this, module, &pipeline = m_compute_pipelines[module->id]] {
				CreatePipelineInternal(m_graphics, pipeline, *module, m_driver_cache);
			});
		}
	}
	Common::ParallelFor(jobs.size(), [&](size_t i) { jobs[i](); });
	// A key with a stage missing from the store keeps no pipeline.
	std::erase_if(m_graphics_pipelines,
	              [](const auto& entry) { return entry.second.pipeline == nullptr; });
	if (!m_graphics_pipelines.empty() || !m_compute_pipelines.empty()) {
		CheckpointDriverCache();
	}
	Log::WriteToConsoleAndLog(fmt::format(
	    "Shader cache: loaded {} programs, {} modules, {} pipelines in {} ms\n",
	    m_program_cache.size(), modules.size() - 1,
	    m_graphics_pipelines.size() + m_compute_pipelines.size(),
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
	                                                          start)
	        .count()));
}

bool PipelineCache::LoadRecord(std::span<const uint8_t>& records) {
	auto                 rest = records;
	std::vector<uint8_t> raw;
	if (!NextFrame(rest, &raw)) {
		return false;
	}
	Reader     reader {raw};
	auto       type     = Record::Module;
	const auto complete = [&] { return reader.ok && reader.bytes.empty(); };
	IoAll(reader, type);
	switch (type) {
		case Record::Module: {
			ProgramKey            key;
			std::vector<uint8_t>  plan;
			ShaderModule          module;
			std::vector<uint32_t> spirv;
			IoKey(reader, key);
			IoAll(reader, plan);
			IoModule(reader, module);
			IoAll(reader, spirv);
			if (!complete()) {
				return false;
			}
			const auto [program, inserted] = m_program_cache.try_emplace(std::move(key));
			if (inserted &&
			    !ShaderRecompiler::IR::LoadResourcePlan(plan, program->second.resource_plan)) {
				m_program_cache.erase(program);
				return false;
			}
			if (!inserted) {
				// A later record's plan carries what the program learned since its first:
				// lowered runtime layout threshold and grown table slots.
				ShaderRecompiler::IR::ResourcePlan later;
				if (!ShaderRecompiler::IR::LoadResourcePlan(plan, later)) return false;
				ShaderRecompiler::IR::MergeRuntimeLayoutState(program->second.resource_plan, later);
			}
			module.module = CompileSPV(spirv, m_graphics.device);
			program->second.modules.push_back(std::move(module));
			break;
		}
		case Record::GraphicsPipeline: {
			GraphicsPipelineKey key;
			reader.Raw(&key, sizeof(key));
			if (!complete()) {
				return false;
			}
			// WarmUp creates the pipeline once every record is loaded.
			m_graphics_pipelines.try_emplace(key);
			break;
		}
		case Record::TessellationLayout: {
			uint64_t                    key = 0;
			ShaderTessellationInputInfo layout;
			IoAll(reader, key, layout);
			if (!complete()) {
				return false;
			}
			m_tess_layouts.try_emplace(key, layout);
			break;
		}
		default: return false;
	}
	records = rest;
	return true;
}

// Records are encoded here, since programs and modules change later.
void PipelineCache::StoreModule(const ProgramKey& key, const Program& program,
                                const ShaderModule& module, const std::vector<uint32_t>& spirv) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::Module);
	IoKey(record, key);
	IoAll(record, ShaderRecompiler::IR::SaveResourcePlan(program.resource_plan));
	IoModule(record, module);
	IoAll(record, spirv);
	m_store.Append(std::move(record.bytes));
}

void PipelineCache::StorePipeline(const GraphicsPipelineKey& key, bool recipe) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::GraphicsPipeline);
	record.Raw(&key, sizeof(key));
	if (recipe && m_recipes) {
		m_recipes.Append(std::vector<uint8_t>(record.bytes));
	}
	m_store.Append(std::move(record.bytes));
}

void PipelineCache::StoreLayout(uint64_t key, const ShaderTessellationInputInfo& layout,
                                bool recipe) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::TessellationLayout, key, layout);
	if (recipe && m_recipes) {
		m_recipes.Append(std::vector<uint8_t>(record.bytes));
	}
	m_store.Append(std::move(record.bytes));
}

void PipelineCache::StoreRecipe(const ProgramKey& key, const Program& program,
                                const ShaderModule& module,
                                const ShaderRecompiler::CompileOptions& options,
                                std::span<const uint32_t> code,
                                ShaderRecompiler::IR::PushData::Window push_data) {
	if (!m_recipes) {
		return;
	}
	std::vector<uint8_t> input;
	switch (key.stage) {
		case ShaderType::Pixel: input = InputInfoBytes(*options.input_info.pixel); break;
		case ShaderType::Compute: input = InputInfoBytes(*options.input_info.compute); break;
		default: input = InputInfoBytes(*options.input_info.vertex); break;
	}
	Writer record;
	IoAll(record, Record::Module);
	IoKey(record, key);
	IoAll(record, module.id, input, options.wave_size, options.user_data_base,
	      static_cast<uint32_t>(options.user_data.size()),
	      std::vector<uint32_t>(code.begin(), code.end()),
	      std::vector<uint32_t>(options.back_code.begin(), options.back_code.end()),
	      push_data.begin, push_data.end, program.specialization,
	      program.resource_plan.runtime_layout_min_candidates,
	      program.resource_plan.runtime_layout_slots);
	m_recipes.Append(std::move(record.bytes));
}

std::vector<std::vector<uint8_t>> PipelineCache::OpenRecipes(const std::filesystem::path& store,
                                                             bool                         load) {
	const auto path   = std::filesystem::path(store).replace_extension(".recipes");
	const auto header = RecipeHeader();
	const auto bytes  = ReadWholeFile(path);
	std::vector<std::vector<uint8_t>> records;
	uint64_t                          kept = 0;
	if (bytes.size() >= header.size() &&
	    std::ranges::equal(std::span(bytes).first(header.size()), header)) {
		auto                 rest = std::span<const uint8_t>(bytes).subspan(header.size());
		std::vector<uint8_t> raw;
		while (NextFrame(rest, load ? &raw : nullptr)) {
			if (load) records.push_back(std::move(raw));
		}
		// A damaged record ends the recipes; the records before it stay.
		kept = bytes.size() - rest.size();
	}
	m_recipes.Open(path, header, kept);
	return records;
}

void PipelineCache::RebuildFromRecipes(std::span<const std::vector<uint8_t>> records) {
	const auto start = std::chrono::steady_clock::now();
	struct Recipe {
		ProgramKey                                   key;
		uint64_t                                     id = 0;
		std::vector<uint8_t>                         input;
		uint32_t                                     wave_size       = 64;
		uint32_t                                     user_data_base  = 0;
		uint32_t                                     user_data_count = 0;
		std::vector<uint32_t>                        code;
		std::vector<uint32_t>                        back_code;
		ShaderRecompiler::IR::PushData::Window       push_data;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		uint32_t                                     min_candidates = 0;
		std::vector<uint32_t>                        slots;
	};
	struct Built {
		ShaderRecompiler::IR::ResourcePlan plan;
		BuiltModule                        module;
	};
	std::vector<Recipe>          recipes;
	std::unordered_set<uint64_t> ids;
	std::atomic<size_t>          skipped = 0;
	for (const auto& raw: records) {
		Reader     reader {raw};
		auto       type     = Record::Module;
		const auto complete = [&] { return reader.ok && reader.bytes.empty(); };
		IoAll(reader, type);
		switch (type) {
			case Record::Module: {
				Recipe recipe;
				IoKey(reader, recipe.key);
				IoAll(reader, recipe.id, recipe.input, recipe.wave_size, recipe.user_data_base,
				      recipe.user_data_count, recipe.code, recipe.back_code, recipe.push_data.begin,
				      recipe.push_data.end, recipe.specialization, recipe.min_candidates,
				      recipe.slots);
				if (!complete()) {
					skipped++;
				} else if (ids.insert(recipe.id).second) {
					recipes.push_back(std::move(recipe));
				}
				break;
			}
			case Record::GraphicsPipeline: {
				GraphicsPipelineKey key;
				reader.Raw(&key, sizeof(key));
				// WarmUp creates the pipeline once the modules are built.
				if (complete() && m_graphics_pipelines.try_emplace(key).second) {
					StorePipeline(key, false);
				}
				break;
			}
			case Record::TessellationLayout: {
				uint64_t                    key = 0;
				ShaderTessellationInputInfo layout;
				IoAll(reader, key, layout);
				if (complete() && m_tess_layouts.try_emplace(key, layout).second) {
					StoreLayout(key, layout, false);
				}
				break;
			}
			default: skipped++; break;
		}
	}

	std::vector<std::optional<Built>> built(recipes.size());
	const bool                        debug_info = Config::GraphicsDebugDumpEnabled();
	Common::ParallelFor(recipes.size(), [&](size_t i) {
		const auto& recipe = recipes[i];
		try {
			ShaderVertexInputInfo            vertex {};
			ShaderPixelInputInfo             pixel {};
			ShaderComputeInputInfo           compute {};
			ShaderRecompiler::CompileOptions options;
			bool                             input = false;
			switch (recipe.key.stage) {
				case ShaderType::Pixel:
					input                    = InputInfoFromBytes(recipe.input, pixel);
					options.input_info.pixel = &pixel;
					break;
				case ShaderType::Compute:
					input                      = InputInfoFromBytes(recipe.input, compute);
					options.input_info.compute = &compute;
					break;
				default:
					input                     = InputInfoFromBytes(recipe.input, vertex);
					options.input_info.vertex = &vertex;
					break;
			}
			if (!input) {
				skipped++;
				return;
			}
			const std::vector<uint32_t> user_data(recipe.user_data_count);
			options.stage           = recipe.key.stage;
			options.wave_size       = recipe.wave_size;
			options.user_data_base  = recipe.user_data_base;
			options.shader_hash     = recipe.key.hash;
			options.dump_ir         = false;
			options.emit_debug_info = debug_info;
			options.user_data       = user_data;
			options.back_code       = recipe.back_code;
			// Only programs without GPU-selected calls have recipes.
			const auto source = ShaderRecompiler::PrepareShaderSource(recipe.code, options);
			if (source.unsupported_call_pc != UINT32_MAX || !source.calls.empty()) {
				skipped++;
				return;
			}
			auto        translated = ShaderRecompiler::TranslateProgram(source.decoded, options);
			const auto& info       = translated.program.info;
			const auto& resources  = recipe.specialization;
			if (resources.buffers.size() < info.buffers.size() ||
			    resources.images.size() < info.images.size() ||
			    resources.samplers.size() < info.samplers.size()) {
				skipped++;
				return;
			}
			auto plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
			plan.runtime_layout_min_candidates = recipe.min_candidates;
			plan.runtime_layout_slots          = recipe.slots;
			auto module = BuildModule(m_graphics, recipe.key, options, std::move(translated),
			                          recipe.specialization, {}, recipe.push_data);
			built[i].emplace(Built {.plan = std::move(plan), .module = std::move(module)});
		} catch (...) {
			// Resource tracking that fails (UnsupportedComputeShader) or anything else the
			// recompiler throws leaves the program to compile at run time.
			skipped++;
		}
	});

	size_t rebuilt = 0;
	for (size_t i = 0; i < recipes.size(); i++) {
		if (!built[i]) {
			continue;
		}
		auto& [plan, module]           = *built[i];
		const auto [program, inserted] = m_program_cache.try_emplace(recipes[i].key);
		if (inserted) {
			program->second.resource_plan = std::move(plan);
		} else {
			ShaderRecompiler::IR::MergeRuntimeLayoutState(program->second.resource_plan, plan);
		}
		StoreModule(program->first, program->second, module.module, module.spirv);
		program->second.modules.push_back(std::move(module.module));
		rebuilt++;
	}
	Log::WriteToConsoleAndLog(fmt::format(
	    "Shader cache: rebuilt {} modules from recipes ({} skipped) in {} ms\n", rebuilt,
	    skipped.load(),
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
	                                                          start)
	        .count()));
}

PipelineCache::ModuleMap PipelineCache::MapModules() const {
	ModuleMap modules {{0, nullptr}};
	for (const auto& program: m_program_cache | std::views::values) {
		for (const auto& module: program.modules) {
			modules.emplace(module.id, &module);
		}
	}
	return modules;
}

bool PipelineCache::CreateGraphicsPipeline(Pipeline& pipeline, const GraphicsPipelineKey& key,
                                           const ModuleMap& modules) {
	std::array<const ShaderModule*, 4> stages {};
	for (size_t i = 0; i < stages.size(); i++) {
		const auto found = modules.find(key.stage_ids[i]);
		if (found == modules.end()) {
			return false;
		}
		stages[i] = found->second;
	}
	CreatePipelineInternal(m_graphics, pipeline, key, stages, m_driver_cache);
	return true;
}

} // namespace Libs::Graphics
