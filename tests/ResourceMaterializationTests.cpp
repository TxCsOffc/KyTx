#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/host_gpu/renderer/pipeline/flatSrtControl.h"
#include "graphics/host_gpu/renderer/pipeline/compiledSrtExperiment.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <filesystem>
#include <fstream>

// Deterministic clock for the actual live controller's poll/revalidation tests.
static uint64_t compiled_control_test_clock = 1'000'000'000ull;
namespace Libs::Graphics {
uint64_t ProfileClockNs() noexcept { return compiled_control_test_clock; }
uint64_t ProfileThreadId() noexcept { return 1; }
}

namespace {

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "ResourceMaterializationTests: failed: %s\n", text);
    std::abort();
  }
}

bool RejectSpecializationRead(void *userdata, uint64_t, std::span<uint32_t>) {
  ++*static_cast<uint32_t *>(userdata);
  return false;
}

Libs::Graphics::ShaderRecompiler::IR::Block &
AddValueBlock(Libs::Graphics::ShaderRecompiler::IR::Program &program) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto block = std::make_unique<Block>();
  auto *result = block.get();
  program.blocks.push_back(result);
  program.block_info.push_back({.id = 0});
  program.block_storage.push_back(std::move(block));
  return *result;
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan SrtPlan(uint64_t address) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &value_block = AddValueBlock(program);

  MemoryInfo memory;
  memory.kind = ResourceKind::ScalarAddress;
  memory.planning_only = true;
  program.memory_info.push_back(memory);
  const auto low = Value(static_cast<uint32_t>(address));
  const auto high = Value(static_cast<uint32_t>(address >> 32u));
  auto &handle =
      value_block.AppendNewInst(ValueOpcode::GetAddressResource, {low, high});
  auto &raw = value_block.AppendNewInst(
      ValueOpcode::LoadAddressU32,
      {Value(&handle), Value(0u), Value(0u), Value(true)});
  raw.SetFlags(MemoryFlags{.index = 0, .pc = 0x40});
  program.srt_reads.push_back({Value(&raw), 0});

  auto &srt = value_block.AppendNewInst(ValueOpcode::GetSrtResource);
  auto &flat = value_block.AppendNewInst(ValueOpcode::ReadConst,
                                         {Value(&srt), Value(0u)});
  DescriptorSource source;
  source.dwords[0] = Value(&flat);
  source.dwords[1] = Value(0u);
  source.dword_count = 2;
  program.descriptor_sources.push_back(source);
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan UnbasedFlatPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  AddValueBlock(program);
  program.info.uses_dma = true;
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan UserDataBufferPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &value_block = AddValueBlock(program);

  auto &user_data = value_block.AppendNewInst(
      ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(0))});
  DescriptorSource source;
  source.dwords[0] = Value(&user_data);
  source.dwords[1] = Value(0u);
  source.dwords[2] = Value(0u);
  source.dwords[3] = Value(0u);
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0});
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::Program MixedSamplerProgram() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);

  const auto AddSource = [&program](uint32_t dword_count) {
    DescriptorSource source;
    source.dword_count = dword_count;
    for (uint32_t i = 0; i < dword_count; i++) {
      source.dwords[i] = Value(0u);
    }
    program.descriptor_sources.push_back(source);
    return static_cast<uint32_t>(program.descriptor_sources.size() - 1u);
  };

  const auto image0 = AddSource(8);
  const auto image1 = AddSource(8);
  const auto sampler0 = AddSource(4);
  const auto sampler1 = AddSource(4);
  for (uint32_t index = 0; index < 2; ++index) {
    auto &value = block.AppendNewInst(
        ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(index))});
    program.descriptor_sources[sampler0 + index].dwords[0] = Value(&value);
  }
  program.info.images.push_back(
      {.source = image0,
       .resource_class = ImageResourceClass::Sampled,
       .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
       .dimension =
           Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D});
  program.info.images.push_back(
      {.source = image1,
       .resource_class = ImageResourceClass::Sampled,
       .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
       .dimension =
           Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D,
       .conversion_format =
           Libs::Graphics::Prospero::BufferFormat::k8_8_8_8UNorm});
  program.info.samplers.push_back({.source = sampler0});
  program.info.samplers.push_back({.source = sampler1});
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 0});
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 1});
  program.info.sampled_pairs.push_back({.image = 1, .sampler = 1});
  return program;
}

void TestMappedSrtUsesDirectReaderByDefault() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  const uint32_t dword = 0x12345678;
  auto plan = SrtPlan(reinterpret_cast<uint64_t>(&dword));
  uint32_t specialization_reads = 0;
  const SrtRuntime runtime{.userdata = &specialization_reads,
                           .read_specialization_memory =
                               RejectSpecializationRead};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "mapped SRT stage materialization failed");
  Check(specialization_reads == 0,
        "ordinary SRT read used the specialization reader");
  Check(snapshot.flattened_srt.size() == 1 &&
            snapshot.flattened_srt[0] == dword,
        "cache rematerialization did not use the direct reader by default");
}

void TestIntegerRuntimeValueFollowsSrtReads() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = SrtPlan(0x10000);
  const auto root = plan.descriptor_sources.front().dwords[0];
  Check(ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "integer SRT read was rejected");

  Block values;
  auto &comparison = values.AppendNewInst(ValueOpcode::FPOrdLessThanEqual32,
                                          {Value::F32(1.f), Value::F32(0.f)});
  auto &selection = values.AppendNewInst(
      ValueOpcode::SelectU32, {Value(&comparison), Value(1u), Value(0u)});
  plan.srt_reads[0].value = Value(&selection);
  Check(ValidateRuntimeValue(plan, root),
        "ordinary SRT validation rejected a floating-point dependency");
  Check(!ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "integer SRT validation missed a hidden floating-point dependency");

  auto &first =
      values.AppendNewInst(ValueOpcode::ReadFirstLane, {root, Value(true)});
  Check(!ValidateRuntimeValue(plan, Value(&first), RuntimeValueType::Integer),
        "read-first-lane lost integer-only SRT validation");

  auto &active = values.AppendNewInst(ValueOpcode::ReadFirstLane,
                                      {Value(&selection), Value(&comparison)});
  Check(!ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "floating-point execution mask was accepted as integer-only");

  auto &lane = values.AppendNewInst(
      ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)),
       Value(0u)});
  auto &mask =
      values.AppendNewInst(ValueOpcode::INotEqual32, {Value(&lane), Value(0u)});
  selection.SetArg(0, Value(&mask));
  active.SetArg(1, Value(&mask));
  Check(ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "nonuniform integer execution mask was rejected");
  auto &float_value =
      values.AppendNewInst(ValueOpcode::BitCastU32F32, {Value::F32(1.f)});
  selection.SetArg(2, Value(&float_value));
  Check(!ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "floating-point inactive arm was accepted as integer-only");

  plan.srt_reads[0].value = Value(&first);
  Check(!ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "cyclic SRT read-first-lane dependency was accepted");
}

void TestUnbasedFlatCacheHitMaterializes() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = UnbasedFlatPlan();
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, {}, snapshot, specialization),
        "unbased FLAT stage materialization failed");
  Check(snapshot.buffers.empty() && snapshot.images.empty(),
        "unbased FLAT plan produced unexpected descriptors");
}

void TestWrittenDescriptorUsesStrictReaderOnce() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  program.memory_info.push_back({.kind = ResourceKind::ScalarAddress});
  auto &handle = block.AppendNewInst(ValueOpcode::GetAddressResource,
                                     {Value(0x1000u), Value(0u)});
  auto &offset = block.AppendNewInst(ValueOpcode::GetUserData,
                                     {Value(static_cast<ScalarReg>(0))});
  auto &read = block.AppendNewInst(ValueOpcode::LoadAddressU32,
      {Value(&handle), Value(&offset), Value(0u), Value(false)});
  read.SetFlags(MemoryFlags{.index = 0});
  DescriptorSource source;
  source.dwords = {Value(&read), Value(0u), Value(4u), Value(0u)};
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0, .written = true});
  auto plan = ExtractResourcePlan(program);
  struct Reads { uint32_t ordinary = 0; uint32_t strict = 0; bool clean = false; } reads;
  const std::array<uint32_t, 1> user_data{4u};
  const SrtRuntime runtime{
      .user_data = user_data,
      .read_memory = +[](void *data, uint64_t, std::span<uint32_t> words) {
        ++static_cast<Reads *>(data)->ordinary;
        words[0] = 0x8000u;
        return true;
      },
      .userdata = &reads,
      .read_specialization_memory = +[](void *data, uint64_t address, std::span<uint32_t> words) {
        auto &reads = *static_cast<Reads *>(data);
        ++reads.strict;
        if (!reads.clean || address != 0x1004u) return false;
        words[0] = 0x8000u;
        return true;
      }};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization) &&
            reads.ordinary == 0 && reads.strict == 1,
        "GPU-dirty dynamic writable descriptor bypassed strict provenance");
  reads.clean = true;
  reads.strict = 0;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            reads.ordinary == 0 && reads.strict == 1 && snapshot.buffers[0].dwords[0] == 0x8000u,
        "writable descriptor was evaluated twice or scalar EXEC suppressed its read");
  DescriptorEvaluationStats stats;
  reads.strict = 0;
  Check(ShadowMaterializeResources(plan, runtime, snapshot, specialization, stats) &&
        reads.ordinary==0 && reads.strict==1 && stats.full_mismatches==0,
        "full shadow bypassed strict provenance or repeated a strict guest read");
}

void TestFailedMaterializationRejectsStage() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = UserDataBufferPlan();
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(!MaterializeResources(plan, {}, snapshot, specialization),
        "missing runtime user data did not reject the cached stage");
}

void TestFiniteImageRefreshReusesScalarReads(bool compiled=false) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  program.memory_info.push_back({.kind = ResourceKind::ScalarAddress});
  auto &handle = block.AppendNewInst(ValueOpcode::GetAddressResource,
                                     {Value(0x1000u), Value(0u)});
  auto &srt = block.AppendNewInst(ValueOpcode::GetSrtResource);
  for (uint32_t index = 0; index < 3; ++index) {
    auto &read = block.AppendNewInst(ValueOpcode::LoadAddressU32,
        {Value(&handle), Value(index * 4u), Value(0u), Value(true)});
    read.SetFlags(MemoryFlags{.index = 0});
    program.srt_reads.push_back({Value(&read), index});
    auto &flat = block.AppendNewInst(ValueOpcode::ReadConst,
                                     {Value(&srt), Value(index)});
    DescriptorSource source;
    source.dword_count = 8;
    source.dwords.fill(Value(0u));
    source.dwords[0] = Value(&flat);
    source.dwords[1] = Value(static_cast<uint32_t>(
        Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u);
    source.dwords[3] = Value(Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u));
    program.descriptor_sources.push_back(source);
  }
  DescriptorSource root;
  root.dword_count = 8;
  root.dwords.fill(Value(0u));
  root.indirect_image.emplace(DescriptorSource::IndirectImage{}).sources = {0, 1, 2, 1};
  program.descriptor_sources.push_back(root);
  program.info.images.push_back({
      .source = 3,
      .resource_class = ImageResourceClass::Sampled,
      .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
      .dimension = Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D});
  auto plan = ExtractResourcePlan(program);
  struct Reads {
    std::array<uint32_t, 3> words{0x100u, 0x200u, 0x200u};
    std::array<uint32_t, 3> counts{};
    uint32_t ordinary = 0;
  } reads;
  const SrtRuntime runtime{
      .read_memory = +[](void *data, uint64_t, std::span<uint32_t>) {
        ++static_cast<Reads *>(data)->ordinary;
        return false;
      },
      .userdata = &reads,
      .read_specialization_memory = +[](void *data, uint64_t address,
                                        std::span<uint32_t> words) {
        if (words.size() != 1 || address < 0x1000u || address >= 0x100cu ||
            (address & 3u) != 0) return false;
        auto &reads = *static_cast<Reads *>(data);
        const auto index = (address - 0x1000u) / 4u;
        ++reads.counts[index];
        words[0] = reads.words[index];
        return true;
      }, .compiled_flat_srt=compiled};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const auto capacities = [&] {
    return std::array{snapshot.images.capacity(), snapshot.flattened_srt.capacity(),
                      snapshot.specialization_reads.capacity(), specialization.images.capacity()};
  };
  const auto check_mapping = [&](std::array<uint32_t, 4> ordinals) {
    const auto offset = specialization.images[0].indirect_mapping_offset;
    Check(snapshot.images.size() == 2 && snapshot.flattened_srt[offset] == 4,
          "finite image candidates were not deduplicated");
    for (uint32_t key = 0; key < ordinals.size(); ++key) {
      Check(snapshot.flattened_srt[offset + 1u + key * 2u] == key &&
                snapshot.flattened_srt[offset + 2u + key * 2u] == ordinals[key],
            "finite image selector mapping is stale or incorrect");
    }
  };
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "finite image materialization failed");
  Check(reads.ordinary == 0 && reads.counts == std::array<uint32_t, 3>{1, 1, 1} &&
            snapshot.specialization_reads.size() == 3,
        "finite image candidates repeated scalar reads or bypassed clean provenance");
  check_mapping({0, 1, 1, 1});
  const auto warm_capacities = capacities();
  reads.words = {0x300u, 0x300u, 0x400u};
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "finite image refresh failed after descriptor changes");
  Check(reads.ordinary == 0 && reads.counts == std::array<uint32_t, 3>{2, 2, 2} &&
            snapshot.specialization_reads.size() == 3 &&
            snapshot.images[0].dwords[0] == 0x300u && snapshot.images[1].dwords[0] == 0x400u,
        "finite image refresh retained old scalar values or repeated reads");
  check_mapping({0, 0, 1, 0});
  Check(capacities() == warm_capacities,
        "finite image refresh grew reusable resource storage after warmup");
}

void TestMixedSamplerVariantsShareRuntimeDescriptor() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto program = MixedSamplerProgram();
  auto plan = ExtractResourcePlan(program);
  std::array<uint32_t, 2> user_data{0x11111111u, 0x22222222u};
  const SrtRuntime runtime{.user_data = user_data};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "mixed sampler materialization failed");
  ApplyResourceSpecialization(program, specialization);
  const auto &samplers = program.info.samplers;
  Check(snapshot.samplers.size() == 2 && samplers.size() == 3 &&
            samplers[0].snapshot_index == 0 && samplers[1].snapshot_index == 1 &&
            samplers[2].snapshot_index == 1 &&
            samplers[0].source == plan.info.samplers[0].source &&
            samplers[1].source == plan.info.samplers[1].source &&
            samplers[2].source == samplers[1].source &&
            !samplers[1].force_point_filtering && samplers[2].force_point_filtering &&
            program.info.sampled_pairs[2].sampler == 2,
        "native sampler variants lost their source identity or binding order");
  const auto capacity = snapshot.samplers.capacity();
  user_data[1] = 0x33333333u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.samplers.size() == 2 && snapshot.samplers.capacity() == capacity &&
            snapshot.samplers[samplers[0].snapshot_index].dwords[0] == user_data[0] &&
            snapshot.samplers[samplers[1].snapshot_index].dwords[0] == user_data[1] &&
            snapshot.samplers[samplers[2].snapshot_index].dwords[0] == user_data[1],
        "sampler variants retained stale or duplicated descriptors after refresh");
}

void TestDescriptorGatherEvaluation() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.user_data_base = 8;
  auto& block = AddValueBlock(program);
  auto& data = block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(8))});
  auto& below = block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(7))});
  auto& above = block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(10))});
  auto& add = block.AppendNewInst(ValueOpcode::IAdd32, {Value(&data), Value(3u)});
  for (const auto value : {Value(&data), Value(&below), Value(&above), Value(&add), Value(uint16_t{7})}) {
    DescriptorSource source;
    source.dword_count = 3;
    source.dwords[0] = Value(0x12345678u);
    source.dwords[1] = value;
    source.dwords[2] = Value(9u);
    program.descriptor_sources.push_back(source);
  }
  auto plan = ExtractResourcePlan(program);
  Check(plan.descriptor_gathers[0].eligible && plan.descriptor_gathers[1].eligible &&
        plan.descriptor_gathers[2].eligible && !plan.descriptor_gathers[3].eligible &&
        !plan.descriptor_gathers[4].eligible, "gather accepted an unsupported expression");
  // Move assignment must carry the owning immutable plan and memo indices together.
  ResourcePlan moved;
  moved = std::move(plan);
  std::array<uint32_t, 2> data_words{11u, 22u};
  for (uint32_t refresh = 0; refresh < 3; ++refresh) {
    data_words[0] += 100u;
    const SrtRuntime runtime{.user_data=data_words};
    for (uint32_t source = 0; source < moved.descriptor_sources.size(); ++source) {
      DescriptorValue reference, candidate, shadow;
      SrtWalker off(moved, runtime);
      SrtWalker fast(moved, runtime, {}, nullptr, {}, DescriptorEvaluationMode::Fast);
      DescriptorEvaluationStats stats;
      SrtWalker checked(moved, runtime, {}, nullptr, {}, DescriptorEvaluationMode::Shadow, &stats);
      const bool a = off.EvaluateDescriptor(source, reference);
      const bool b = fast.EvaluateDescriptor(source, candidate);
      const bool c = checked.EvaluateDescriptor(source, shadow);
      Check(a==b && a==c && reference==candidate && reference==shadow,
            "gather changed descriptor bytes or partial failure output");
      Check(stats.descriptor_mismatches==0 && !moved.descriptor_gather_rejected, "shadow mismatch");
      if (source==0) Check(candidate.dwords[1]==data_words[0] && candidate.dwords[3]==0,
                           "gather retained old user data or nonzero trailing words");
      if (source==0) {
        data_words[0] ^= 0x44u;
        Check(off.EvaluateDescriptor(source, reference) && fast.EvaluateDescriptor(source, candidate) &&
              checked.EvaluateDescriptor(source, shadow) && reference==candidate && reference==shadow &&
              stats.descriptor_mismatches==0, "gather bypassed an existing per-session user-data memo");
      }
    }
  }
}

void TestFullDescriptorShadow() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto program = MixedSamplerProgram();
  auto plan = ExtractResourcePlan(program);
  std::array<uint32_t, 2> words{0x11111111u, 0x22222222u};
  ResourceSnapshot reference, checked, fast;
  ResourceSpecialization reference_key, checked_key, fast_key;
  DescriptorEvaluationStats stats;
  for (uint32_t refresh = 0; refresh < 3; ++refresh) {
    words[1] += 10;
    const SrtRuntime runtime{.user_data=words};
    Check(MaterializeResources(plan, runtime, reference, reference_key) &&
          ShadowMaterializeResources(plan, runtime, checked, checked_key, stats) &&
          MaterializeResources(plan, runtime, fast, fast_key, DescriptorEvaluationMode::Fast),
          "full gather materialization failed");
    Check(reference==checked && reference==fast && reference_key==checked_key && reference_key==fast_key,
          "full snapshot or specialization changed");
  }
  Check(stats.full_checks==3 && stats.full_mismatches==0 && stats.descriptor_checks>0 &&
        plan.descriptor_gather_verified, "full shadow did not validate eligible plan");
  plan.descriptor_gathers[0].words[0].value = 123u; // Deliberate candidate corruption.
  Check(ShadowMaterializeResources(plan, {.user_data=words}, checked, checked_key, stats) &&
        plan.descriptor_gather_rejected && stats.descriptor_mismatches>0,
        "shadow did not reject a descriptor mismatch");
  Check(MaterializeResources(plan, {.user_data=words}, fast, fast_key, DescriptorEvaluationMode::Fast) &&
        fast==checked && fast_key==checked_key, "rejected plan continued to use candidate output");
  // Fallback memory reads occur once, in their original reader, never again in candidate replay.
  auto fallback = SrtPlan(0x12340000);
  uint32_t calls=0;
  SrtRuntime runtime{.read_memory=+[](void* p, uint64_t, std::span<uint32_t> out) {
    ++*static_cast<uint32_t*>(p); out[0]=55; return true;
  }, .userdata=&calls};
  Check(ShadowMaterializeResources(fallback, runtime, checked, checked_key, stats) && calls==1,
        "shadow performed a second guest memory read");
  runtime.read_memory=+[](void* p, uint64_t, std::span<uint32_t>) {
    ++*static_cast<uint32_t*>(p); return false;
  };
  Check(!ShadowMaterializeResources(fallback, runtime, checked, checked_key, stats) && calls==2 &&
        stats.full_mismatches==0, "shadow changed a failed read or partial output");
}

void TestInactiveDescriptorRemainsInactive() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = UserDataBufferPlan();
  plan.control_flow.resize(3);
  plan.control_flow[0].condition=Value(false);
  plan.control_flow[0].successors={1,2};
  plan.control_flow[1].sources={0};
  const SrtRuntime runtime{.read_specialization_memory=+[](void*, uint64_t, std::span<uint32_t>) {
    return false;
  }};
  ResourceSnapshot off, checked;
  ResourceSpecialization off_key, checked_key;
  DescriptorEvaluationStats stats;
  Check(MaterializeResources(plan, runtime, off, off_key) &&
        ShadowMaterializeResources(plan, runtime, checked, checked_key, stats) &&
        off==checked && off_key==checked_key && stats.calls==0,
        "inactive descriptor was evaluated despite missing user data");
}

void TestSampledEvaluatorProfilePreservesOutput() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto program=MixedSamplerProgram();
  auto plan=ExtractResourcePlan(program);
  std::array<uint32_t,2> words{123,456};
  SrtRuntime runtime{.user_data=words};
  ResourceSnapshot reference, observed;
  ResourceSpecialization reference_key, observed_key;
  Check(MaterializeResources(plan,runtime,reference,reference_key),"profile reference failed");
  SrtEvaluationProfile profile;
  runtime.profile=&profile;
  const auto begin=SrtEvaluationProfile::Clock();
  Check(MaterializeResources(plan,runtime,observed,observed_key),"profiled materialization failed");
  const auto elapsed=SrtEvaluationProfile::Clock()-begin;
  Check(reference==observed && reference_key==observed_key,"profiling changed materialization output");
  uint64_t phases=0; for (const auto ns:profile.phases) phases+=ns;
  Check(phases<=elapsed,"exclusive materializer phase scopes overlap");
  const auto index=static_cast<size_t>(ValueOpcode::GetUserData);
  Check(profile.opcodes[index].misses==2 && profile.opcodes[index].words==2,
        "profile lost descriptor roots or user-data evaluation misses");
  {
    SrtWalker walker(plan,runtime);
    DescriptorValue a,b;
    Check(walker.EvaluateDescriptor(plan.info.samplers[0].source,a) &&
          walker.EvaluateDescriptor(plan.info.samplers[0].source,b) && a==b,
          "profile changed repeated descriptor evaluation");
  }
  Check(profile.opcodes[index].hits==1,"profile missed per-session memo reuse");
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan FlatRecipePlan(uint32_t count, bool buffer=false) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage=Libs::Graphics::ShaderType::Compute;
  program.user_data_base=8;
  program.srt_plan_complete=program.resource_tracking_complete=true;
  auto& block=AddValueBlock(program);
  program.memory_info.push_back({.kind=buffer ? ResourceKind::ScalarBuffer : ResourceKind::ScalarAddress});
  auto& low=block.AppendNewInst(ValueOpcode::GetUserData,{Value(static_cast<ScalarReg>(8))});
  auto& high=block.AppendNewInst(ValueOpcode::GetUserData,{Value(static_cast<ScalarReg>(9))});
  auto& records=block.AppendNewInst(ValueOpcode::GetUserData,{Value(static_cast<ScalarReg>(10))});
  auto& handle=block.AppendNewInst(buffer ? ValueOpcode::GetBufferResource : ValueOpcode::GetAddressResource,
      buffer ? std::initializer_list<Value>{Value(&low),Value(&high),Value(&records),Value(0u)} :
               std::initializer_list<Value>{Value(&low),Value(&high)});
  auto& srt=block.AppendNewInst(ValueOpcode::GetSrtResource);
  for (uint32_t i=0; i<count; ++i) {
    auto& fallback=block.AppendNewInst(ValueOpcode::IAdd32,{Value(i*4u),Value(0u)});
    auto& read=block.AppendNewInst(buffer ? ValueOpcode::ReadConstBuffer : ValueOpcode::LoadAddressU32,
        buffer ? std::initializer_list<Value>{Value(&handle),i%3==0 ? Value(&fallback) : Value(i*4u)} :
                 std::initializer_list<Value>{Value(&handle),i%3==0 ? Value(&fallback) : Value(i*4u),Value(0u),Value(true)});
    read.SetFlags(MemoryFlags{.index=0});
    program.srt_reads.push_back({Value(&read),i});
    auto& flat=block.AppendNewInst(ValueOpcode::ReadConst,{Value(&srt),Value(i)});
    DescriptorSource source;
    source.dwords={Value(&flat),Value(0u),Value(4u),Value(0u)};
    source.dword_count=4;
    program.descriptor_sources.push_back(source);
    program.info.buffers.push_back({.source=i});
  }
  return ExtractResourcePlan(program);
}

void TestFlatSrtRecipeShadow() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  struct Reads { uint32_t calls=0, strict=0, value=0x8000; uint64_t fail=~uint64_t{0}; } reads;
  const auto reader=+[](void* p,uint64_t address,std::span<uint32_t> out) {
    auto& r=*static_cast<Reads*>(p); ++r.calls;
    if (address==r.fail) return false;
    out[0]=r.value+static_cast<uint32_t>(address); return true;
  };
  std::array<uint32_t,3> data{0x1000,0,64};
  SrtRuntime runtime{.user_data=data,.read_memory=reader,.userdata=&reads,
      .read_specialization_memory=+[](void* p,uint64_t address,std::span<uint32_t> out) {
        ++static_cast<Reads*>(p)->strict;
        auto& r=*static_cast<Reads*>(p); ++r.calls;
        if (address==r.fail) return false;
        out[0]=r.value+static_cast<uint32_t>(address); return true;
      }};
  for (bool buffer : {false,true}) {
    auto original=FlatRecipePlan(4,buffer);
    ResourcePlan plan;
    plan=std::move(original);
    ResourceSnapshot reference,checked,candidate;
    ResourceSpecialization reference_key,checked_key,candidate_key;
    FlatSrtEvaluationStats stats;
    for (uint32_t refresh=0; refresh<4; ++refresh) {
      data[0]+=0x100; reads.value+=100;
      reads.calls=0;
      Check(MaterializeResources(plan,runtime,reference,reference_key),"recipe reference failed");
      Check(reads.calls==4,"reference repeated a flat read in descriptor phase");
      reads.calls=0;
      Check(ShadowFlatSrtMaterializeResources(plan,runtime,checked,checked_key,stats) && reads.calls==4,
          "flat shadow repeated a guest read");
      auto fast=runtime; fast.compiled_flat_srt=true;
      reads.calls=0;
      Check(MaterializeResources(plan,fast,candidate,candidate_key) && reads.calls==4 &&
            reference==checked && reference==candidate && reference_key==checked_key && reference_key==candidate_key,
            "recipe changed complete output or current runtime values");
    }
    Check(plan.flat_srt_verified && stats.full_checks==4 && stats.full_mismatches==0 &&
          stats.recipe_evaluations==16 && stats.memo_hits>0,"recipe was not used/validated with shared memos");
    reads.fail=data[0]+4; reads.calls=0;
    Check(!ShadowFlatSrtMaterializeResources(plan,runtime,checked,checked_key,stats) &&
          reads.calls==2 && stats.full_mismatches==0,"recipe changed reader failure prefix");
    reads.fail=~uint64_t{0};
    auto missing=runtime; missing.user_data=std::span(data).first(1); // high register outside runtime span
    Check(!ShadowFlatSrtMaterializeResources(plan,missing,checked,checked_key,stats) &&
          stats.full_mismatches==0,"recipe bypassed user-data bounds");
    if (buffer) {
      data[2]=2; // undersized buffer: no word can be read
      reads.calls=0;
      Check(!ShadowFlatSrtMaterializeResources(plan,runtime,checked,checked_key,stats) && reads.calls==0 &&
            stats.full_mismatches==0,"recipe bypassed buffer bounds");
      data[2]=64;
    }
    // Strict/ordinary sessions stay distinct, and control-flow reachability is unchanged.
    plan.clean_flat_slots.assign(4,1);
    plan.control_flow.resize(3);
    plan.control_flow[0].condition=Value(false); plan.control_flow[0].successors={1,2};
    plan.control_flow[1].srt_reads={0,1}; plan.control_flow[1].sources={0,1};
    plan.control_flow[2].srt_reads={2,3}; plan.control_flow[2].sources={2,3};
    reads.calls=reads.strict=0;
    Check(ShadowFlatSrtMaterializeResources(plan,runtime,checked,checked_key,stats) && reads.calls==2 &&
          reads.strict==2 && checked.buffers[0].dwords[0]==0 && stats.full_mismatches==0,
          "recipe changed clean provenance or inactive branch");
    plan.control_flow.clear(); plan.clean_flat_slots.clear();
    // Corrupt metadata deliberately; full comparison must reject and return reference output.
    for (auto& recipe:plan.flat_srt_recipes) if (recipe.eligible) { recipe.immediate_offset+=4; break; }
    Check(ShadowFlatSrtMaterializeResources(plan,runtime,checked,checked_key,stats) &&
          plan.flat_srt_rejected && stats.full_mismatches==1,"recipe corruption was not rejected");
    auto fast=runtime; fast.compiled_flat_srt=true;
    Check(MaterializeResources(plan,fast,candidate,candidate_key) && candidate==checked && candidate_key==checked_key,
          "rejected recipe continued to run");
  }
  auto signed_plan=SrtPlan(0);
  signed_plan.memory_info[0].offset=static_cast<uint32_t>(-4);
  BuildFlatSrtRecipes(signed_plan);
  FlatSrtEvaluationStats stats;
  ResourceSnapshot snapshot; ResourceSpecialization key;
  Check(!ShadowFlatSrtMaterializeResources(signed_plan,runtime,snapshot,key,stats) && stats.full_mismatches==0,
        "negative address underflow semantics changed");
  auto cyclic=SrtPlan(0x1000);
  auto* raw=cyclic.srt_reads[0].value.ResolveInstruction();
  auto* handle=raw->Arg(0).ResolveInstruction();
  handle->SetArg(0,Value(raw));
  BuildFlatSrtRecipes(cyclic);
  Check(!ShadowFlatSrtMaterializeResources(cyclic,runtime,snapshot,key,stats) && stats.full_mismatches==0,
        "recipe bypassed memo cycle detection");
}

// Compiled-walker equivalence: random value graphs are materialized through the compiled plan
// and through SrtWalker; results, snapshots and the order of guest reads must be identical.

struct SplitMix {
  uint64_t state;

  uint64_t Next() {
    uint64_t z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30u)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27u)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31u);
  }
  uint32_t Below(uint32_t count) {
    return static_cast<uint32_t>(Next() % count);
  }
  bool Chance(uint32_t percent) { return Below(100) < percent; }
};

constexpr uint64_t FuzzMemoryBase = 0x40000;
constexpr uint32_t FuzzMemoryWords = 1024;

uint32_t FuzzPointer(uint32_t index) {
  return static_cast<uint32_t>(FuzzMemoryBase) + (index % (FuzzMemoryWords - 64u)) * 4u;
}

struct FuzzRead {
  uint64_t address;
  size_t bytes;
  bool strict;
  bool operator==(const FuzzRead &) const = default;
};

struct FuzzMemory {
  std::vector<uint32_t> words = std::vector<uint32_t>(FuzzMemoryWords);
  uint32_t strict_salt = 0;
  uint32_t strict_modulus = 7; // Zero: strict reads never fail.
  std::vector<FuzzRead> *log = nullptr;
};

bool ReadFuzzMemory(FuzzMemory &memory, uint64_t address,
                    std::span<uint32_t> values, bool strict) {
  memory.log->push_back({address, values.size_bytes(), strict});
  if (address < FuzzMemoryBase || (address & 3u) != 0u ||
      address - FuzzMemoryBase >
          FuzzMemoryWords * sizeof(uint32_t) - values.size_bytes()) {
    return false;
  }
  // Strict reads reject some GPU-dirty words.
  if (strict && memory.strict_modulus != 0u &&
      ((address >> 2u) * 2654435761u + memory.strict_salt) % memory.strict_modulus == 0u) {
    return false;
  }
  std::copy_n(memory.words.begin() + (address - FuzzMemoryBase) / 4u,
              values.size(), values.begin());
  return true;
}

bool ReadFuzzOrdinary(void *userdata, uint64_t address,
                      std::span<uint32_t> values) {
  return ReadFuzzMemory(*static_cast<FuzzMemory *>(userdata), address, values,
                        false);
}

bool ReadFuzzStrict(void *userdata, uint64_t address,
                    std::span<uint32_t> values) {
  return ReadFuzzMemory(*static_cast<FuzzMemory *>(userdata), address, values,
                        true);
}


class FuzzPlanBuilder {
public:
  explicit FuzzPlanBuilder(uint64_t seed) : m_random{seed}, m_safe(m_random.Chance(55)) {
    using namespace Libs::Graphics::ShaderRecompiler::IR;
    m_program.stage = Libs::Graphics::ShaderType::Compute;
    m_program.srt_plan_complete = true;
    m_program.resource_tracking_complete = true;
    m_block = &AddValueBlock(m_program);
  }

  Libs::Graphics::ShaderRecompiler::IR::ResourcePlan Build() {
    using namespace Libs::Graphics::ShaderRecompiler::IR;
    for (uint32_t reg = 0; reg < 12; reg++) {
      m_u32.push_back(Emit(ValueOpcode::GetUserData,
                           {Value(static_cast<ScalarReg>(reg))}));
      if (reg < 6) {
        m_ptr.push_back(m_u32.back());
      }
    }
    m_ptr.push_back(Value(FuzzPointer(m_random.Below(FuzzMemoryWords))));
    if (!m_safe && m_random.Chance(30)) {
      // Beyond the runtime user data of some refreshes.
      m_u32.push_back(Emit(ValueOpcode::GetUserData,
                           {Value(static_cast<ScalarReg>(20))}));
    }
    for (uint32_t i = 0; i < 6; i++) {
      m_u32.push_back(Value(RandomImmediate()));
    }
    m_u1.push_back(Value(true));
    m_u1.push_back(Value(false));
    m_u64.push_back(Emit(ValueOpcode::GetShaderBase, {}));
    const auto steps = 30 + m_random.Below(90);
    for (uint32_t step = 0; step < steps; step++) {
      Step();
      if (m_random.Chance(12)) {
        const auto slot = static_cast<uint32_t>(m_program.srt_reads.size());
        m_program.srt_reads.push_back({PickRecent(m_u32), slot});
      }
    }
    for (uint32_t i = 0; i < 3; i++) {
      m_program.srt_reads.push_back(
          {RawRead(), static_cast<uint32_t>(m_program.srt_reads.size())});
    }
    AddResources();
    return ExtractResourcePlan(m_program);
  }

  // Values of the extra single-dword sources: [0] control-flow conditions, [1..4] fill words.
  // Safe plans only read valid pointers and avoid failing operations.
  bool Safe() const { return m_safe; }
  uint32_t ConditionSource() const { return m_condition_source; }
  uint32_t FillSource() const { return m_fill_source; }
  uint32_t BufferSource(uint32_t index) const { return m_buffer_sources[index]; }
  uint32_t ImageSource(uint32_t index) const { return m_image_sources[index]; }

private:
  using Value = Libs::Graphics::ShaderRecompiler::IR::Value;
  using ValueOpcode = Libs::Graphics::ShaderRecompiler::IR::ValueOpcode;

  Value Emit(ValueOpcode opcode, std::initializer_list<Value> args,
             uint64_t flags = 0) {
    return Value(&m_block->AppendNewInst(opcode, args, flags));
  }

  uint32_t RandomImmediate() {
    switch (m_random.Below(5)) {
    case 0: return m_random.Below(40);
    case 1: return FuzzPointer(m_random.Below(FuzzMemoryWords));
    case 2: return 0xffffffffu - m_random.Below(4);
    case 3: return std::bit_cast<uint32_t>(static_cast<float>(m_random.Below(1000)) * 0.25f);
    default: return static_cast<uint32_t>(m_random.Next());
    }
  }

  Value Pick(const std::vector<Value> &pool) {
    return pool[m_random.Below(static_cast<uint32_t>(pool.size()))];
  }

  // Prefer recent values so graphs grow deep.
  Value PickRecent(const std::vector<Value> &pool) {
    const auto size = static_cast<uint32_t>(pool.size());
    const auto window = std::min<uint32_t>(size, 8u);
    return m_random.Chance(60) ? pool[size - 1u - m_random.Below(window)]
                               : Pick(pool);
  }

  uint32_t ExtractRange() { return m_safe || m_random.Chance(90) ? 2u : 3u; }

  Value MaybeIdentity(Value value) {
    return m_random.Chance(10) ? Emit(ValueOpcode::Identity, {value}) : value;
  }

  Value RawRead() {
    using namespace Libs::Graphics::ShaderRecompiler::IR;
    const bool buffer = m_random.Chance(30);
    MemoryInfo memory;
    memory.kind = buffer ? ResourceKind::ScalarBuffer : ResourceKind::ScalarAddress;
    const auto choice = m_random.Below(10);
    memory.offset = m_safe        ? m_random.Below(32) * 4u
                    : choice == 0 ? static_cast<uint32_t>(-4 * static_cast<int32_t>(m_random.Below(4)))
                    : choice == 1 ? m_random.Below(64)
                                  : m_random.Below(32) * 4u;
    const auto index = static_cast<uint32_t>(m_program.memory_info.size());
    m_program.memory_info.push_back(memory);
    const auto low = m_safe || m_random.Chance(60) ? PickRecent(m_ptr) : Pick(m_u32);
    const auto high = m_safe || m_random.Chance(85) ? Value(0u) : Pick(m_u32);
    const auto offset =
        m_safe || m_random.Chance(60) ? Value(m_random.Below(16) * 4u) : Pick(m_u32);
    const auto flags = std::bit_cast<uint64_t>(MemoryFlags{.index = index, .pc = index * 4u});
    if (buffer) {
      const auto stride = Value((m_random.Below(3) * 16u) << 16u);
      const auto records = m_safe || m_random.Chance(80) ? Value(0x10000u) : Pick(m_u32);
      const auto handle = Emit(ValueOpcode::GetBufferResource,
                               {MaybeIdentity(low), stride, records, Value(0u)});
      return Emit(ValueOpcode::ReadConstBuffer, {MaybeIdentity(handle), offset}, flags);
    }
    const auto handle = Emit(ValueOpcode::GetAddressResource, {MaybeIdentity(low), high});
    return Emit(ValueOpcode::LoadAddressU32,
                {MaybeIdentity(handle), offset, Value(0u), Value(true)}, flags);
  }

  void Step() {
    using namespace Libs::Graphics::ShaderRecompiler::IR;
    constexpr std::array binary32{
        ValueOpcode::IAdd32, ValueOpcode::ISub32, ValueOpcode::IMul32,
        ValueOpcode::UMin32, ValueOpcode::BitwiseAnd32, ValueOpcode::BitwiseOr32,
        ValueOpcode::BitwiseXor32, ValueOpcode::ShiftLeftLogical32,
        ValueOpcode::ShiftRightLogical32, ValueOpcode::ShiftRightArithmetic32};
    constexpr std::array compare32{
        ValueOpcode::IEqual32, ValueOpcode::INotEqual32, ValueOpcode::ULessThan32,
        ValueOpcode::UGreaterThan32, ValueOpcode::SGreaterThanEqual32};
    constexpr std::array logical{ValueOpcode::LogicalAnd, ValueOpcode::LogicalOr,
                                 ValueOpcode::LogicalXor};
    constexpr std::array binary64{
        ValueOpcode::IAdd64, ValueOpcode::ISub64, ValueOpcode::IMul64,
        ValueOpcode::BitwiseAnd64, ValueOpcode::ShiftLeftLogical64,
        ValueOpcode::ShiftRightLogical64, ValueOpcode::ShiftRightArithmetic64};
    switch (m_random.Below(24)) {
    case 0:
    case 1:
    case 2:
      m_u32.push_back(RawRead());
      m_ptr.push_back(m_u32.back());
      break;
    case 3:
    case 4:
      m_u32.push_back(Emit(binary32[m_random.Below(binary32.size())],
                           {MaybeIdentity(PickRecent(m_u32)), Pick(m_u32)}));
      break;
    case 5:
      m_u1.push_back(Emit(compare32[m_random.Below(compare32.size())],
                          {PickRecent(m_u32), Pick(m_u32)}));
      break;
    case 6:
      m_u1.push_back(m_random.Chance(50)
                         ? Emit(logical[m_random.Below(logical.size())],
                                {Pick(m_u1), Pick(m_u1)})
                         : Emit(m_random.Chance(50) ? ValueOpcode::LogicalNot
                                                    : ValueOpcode::ConditionRef,
                                {Pick(m_u1)}));
      break;
    case 7:
      m_u32.push_back(Emit(ValueOpcode::SelectU32,
                           {PickRecent(m_u1), PickRecent(m_u32), Pick(m_u32)}));
      break;
    case 8: {
      // An EXEC-masked read: selects on the mask take their active operand.
      const auto mask = PickRecent(m_u1);
      const auto active = Emit(ValueOpcode::SelectU32,
                               {mask, PickRecent(m_u32), Pick(m_u32)});
      const auto inner = m_random.Chance(50)
                             ? Emit(ValueOpcode::IAdd32, {active, Pick(m_u32)})
                             : active;
      m_u32.push_back(Emit(ValueOpcode::ReadFirstLane,
                           {inner, m_random.Chance(85) ? mask : Pick(m_u1)}));
      break;
    }
    case 9: {
      const auto packed = Emit(ValueOpcode::CompositeConstructU64,
                               {PickRecent(m_u32), Pick(m_u32)});
      m_u64.push_back(packed);
      m_u32.push_back(Emit(ValueOpcode::CompositeExtractU64,
                           {packed, Value(m_random.Below(ExtractRange()))}));
      break;
    }
    case 10: {
      const auto source = Emit(m_random.Chance(50) ? ValueOpcode::CompositeConstructU32x2
                                                   : ValueOpcode::IAddCarry32,
                               {PickRecent(m_u32), Pick(m_u32)});
      m_u32.push_back(Emit(ValueOpcode::CompositeExtractU32x2,
                           {MaybeIdentity(source),
                            Value(m_random.Below(ExtractRange()))}));
      break;
    }
    case 11:
      m_u64.push_back(Emit(binary64[m_random.Below(binary64.size())],
                           {PickRecent(m_u64), Pick(m_u64)}));
      m_u32.push_back(Emit(ValueOpcode::CompositeExtractU64,
                           {m_u64.back(), Value(m_random.Below(2u))}));
      break;
    case 12: {
      const auto op = m_random.Below(4);
      if (op == 0) {
        m_u32.push_back(Emit(ValueOpcode::ConvertF32U32, {PickRecent(m_u32)}));
      } else if (op == 1 && !m_safe) {
        m_u32.push_back(Emit(ValueOpcode::ConvertU32F32, {PickRecent(m_u32)}));
      } else if (op == 2) {
        m_u32.push_back(Emit(ValueOpcode::FPMul32, {PickRecent(m_u32), Value::F32(0.5f)}));
      } else {
        m_u32.push_back(Emit(ValueOpcode::FPTrunc32, {PickRecent(m_u32)}));
      }
      break;
    }
    case 13: {
      const auto op = m_random.Below(3);
      if (op == 0) {
        m_u1.push_back(Emit(ValueOpcode::FPIsNan32, {PickRecent(m_u32)}));
      } else {
        const auto flush = std::bit_cast<uint8_t>(FPCompareFlags{m_random.Chance(50)});
        m_u1.push_back(Emit(op == 1 ? ValueOpcode::FPOrdLessThanEqual32
                                    : ValueOpcode::FPOrdGreaterThanEqual32,
                            {PickRecent(m_u32), Pick(m_u32)}, flush));
      }
      break;
    }
    case 14: {
      const auto op = m_random.Below(3);
      const auto offset = m_safe              ? Value(m_random.Below(17))
                          : m_random.Chance(80) ? Value(m_random.Below(33))
                                                : Pick(m_u32);
      const auto width = m_safe              ? Value(m_random.Below(16))
                         : m_random.Chance(80) ? Value(m_random.Below(33))
                                               : Pick(m_u32);
      if (op == 2) {
        m_u32.push_back(Emit(ValueOpcode::BitFieldInsert,
                             {PickRecent(m_u32), Pick(m_u32), offset, width}));
      } else {
        m_u32.push_back(Emit(op == 0 ? ValueOpcode::BitFieldUExtract
                                     : ValueOpcode::BitFieldSExtract,
                             {PickRecent(m_u32), offset, width}));
      }
      break;
    }
    case 15:
      m_u32.push_back(Emit(m_random.Chance(50) ? ValueOpcode::BitwiseNot32
                                               : ValueOpcode::BitCastU32F32,
                           {PickRecent(m_u32)}));
      break;
    case 16: {
      // Invariant (removed by the plan) or genuinely divergent phis.
      auto &phi = m_block->AppendNewInst(ValueOpcode::Phi, {},
                                         static_cast<uint64_t>(Type::U32));
      const auto first = PickRecent(m_u32);
      phi.AddPhiOperand(m_block, first);
      phi.AddPhiOperand(m_block, m_safe || m_random.Chance(50) ? first : Pick(m_u32));
      if (m_random.Chance(30)) {
        phi.AddPhiOperand(m_block, Value(&phi));
      }
      m_u32.push_back(Value(&phi));
      break;
    }
    case 17:
    case 18: {
      const auto slots = static_cast<uint32_t>(m_program.srt_reads.size());
      // SSA values cannot reach their own slot; out-of-range slots stay invalid.
      const auto slot = slots != 0u && (m_safe || m_random.Chance(90))
                            ? m_random.Below(slots)
                            : 1000u + m_random.Below(3);
      const auto srt = Emit(ValueOpcode::GetSrtResource, {});
      m_u32.push_back(Emit(ValueOpcode::ReadConst, {srt, Value(slot)}));
      break;
    }
    case 19:
      if (!m_safe && m_random.Chance(20)) {
        m_u32.push_back(Emit(ValueOpcode::UndefU32, {}));
      } else {
        m_u32.push_back(Value(RandomImmediate()));
      }
      break;
    case 20:
      m_u64.push_back(Emit(ValueOpcode::GetShaderBase, {}));
      m_u32.push_back(Emit(ValueOpcode::CompositeExtractU64, {m_u64.back(), Value(0u)}));
      break;
    default:
      m_u32.push_back(Emit(ValueOpcode::IAdd32,
                           {PickRecent(m_u32), Value(m_random.Below(8) * 4u)}));
      break;
    }
  }

  uint32_t AddSource(std::initializer_list<Value> dwords) {
    using namespace Libs::Graphics::ShaderRecompiler::IR;
    DescriptorSource source;
    for (const auto dword : dwords) {
      source.dwords[source.dword_count++] = dword;
    }
    m_program.descriptor_sources.push_back(source);
    return static_cast<uint32_t>(m_program.descriptor_sources.size() - 1u);
  }

  Value Word() { return m_random.Chance(70) ? PickRecent(m_u32) : Pick(m_u32); }

  void AddResources() {
    using namespace Libs::Graphics::ShaderRecompiler::IR;
    namespace Prospero = Libs::Graphics::Prospero;
    const auto buffers = 1u + m_random.Below(3);
    for (uint32_t i = 0; i < buffers; i++) {
      const auto dword3 = m_random.Chance(70) ? Value(0u) : Word();
      m_buffer_sources.push_back(AddSource({Word(), Word(), Word(), dword3}));
      m_program.info.buffers.push_back(
          {.source = m_buffer_sources.back(), .written = m_random.Chance(40),
           .formatted = m_random.Chance(50)});
    }
    const auto valid_format = Value(
        static_cast<uint32_t>(Prospero::BufferFormat::k32_32_32_32Float) << 20u);
    const auto valid_type = Value(
        Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Prospero::ImageType::kColor2D) << 28u));
    const auto images = 1u + m_random.Below(2);
    for (uint32_t i = 0; i < images; i++) {
      const bool valid = m_random.Chance(60);
      m_image_sources.push_back(
          AddSource({Word(), valid ? valid_format : Word(), Value(0u),
                     valid ? valid_type : Word(), Value(0u), Value(0u), Value(0u),
                     Value(0u)}));
      m_program.info.images.push_back(
          {.source = m_image_sources.back(),
           .resource_class = ImageResourceClass::Sampled,
           .numeric_class = Prospero::TextureNumericClass::Float,
           .dimension = Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D});
    }
    const auto sampler = AddSource({Word(), Word(), Word(), Word()});
    m_program.info.samplers.push_back({.source = sampler});
    for (uint32_t i = 0; i < images; i++) {
      m_program.info.sampled_pairs.push_back({.image = i, .sampler = 0});
    }
    m_condition_source = AddSource({PickRecent(m_u1)});
    m_fill_source = AddSource({Word(), Word(), Word(), Word()});
  }

  SplitMix m_random;
  bool m_safe = false;
  Libs::Graphics::ShaderRecompiler::IR::Program m_program;
  Libs::Graphics::ShaderRecompiler::IR::Block *m_block = nullptr;
  std::vector<Value> m_u32;
  std::vector<Value> m_u1;
  std::vector<Value> m_u64;
  std::vector<Value> m_ptr;
  std::vector<uint32_t> m_buffer_sources;
  std::vector<uint32_t> m_image_sources;
  uint32_t m_condition_source = 0;
  uint32_t m_fill_source = 0;
};

void TestCompiledWalkerMatchesReference() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  uint32_t successes = 0;
  uint32_t failures = 0;
  for (uint64_t seed = 1; seed <= 1500; seed++) {
    FuzzPlanBuilder builder(seed);
    auto plan = builder.Build();
    SplitMix random{seed * 7919u};
    // Plan attributes that the extraction derives from real shaders.
    if (random.Chance(40)) {
      const auto condition = plan.descriptor_sources[builder.ConditionSource()].dwords[0];
      plan.control_flow.resize(4);
      plan.control_flow[0] = {.condition = condition, .successors = {1, 2}};
      plan.control_flow[1] = {.condition = random.Chance(50) ? condition : Value{},
                              .successors = {3, 2},
                              .sources = {builder.BufferSource(0)}};
      plan.control_flow[2] = {.successors = {3}, .sources = {builder.ImageSource(0)}};
      plan.control_flow[3] = {.sources = {builder.FillSource()}};
    }
    if (random.Chance(30)) {
      const auto &fill = plan.descriptor_sources[builder.FillSource()];
      plan.uniform_fill.fill = {.kind = UniformFillKind::Buffer,
                                .words = 1u + random.Below(4)};
      for (uint32_t i = 0; i < plan.uniform_fill.fill.words; i++) {
        plan.uniform_fill.values[i] = random.Chance(80) ? fill.dwords[0] : fill.dwords[i];
      }
    }
    for (auto &slot : plan.clean_flat_slots) {
      slot = random.Chance(15) ? 1u : 0u;
    }
    // Extraction captures reads only for plans that require the strict reader.
    plan.capture_specialization_reads = random.Chance(25);
    plan.requires_specialization_memory =
        plan.capture_specialization_reads || random.Chance(10);
    Check(CompileSrtPlan(plan) != nullptr, "fuzz plan was not compiled");

    ResourceSnapshot reference_snapshot;
    ResourceSnapshot compiled_snapshot;
    ResourceSpecialization reference_specialization;
    ResourceSpecialization compiled_specialization;
    for (uint32_t refresh = 0; refresh < 6; refresh++) {
      FuzzMemory memory;
      const bool safe = builder.Safe();
      memory.strict_salt = static_cast<uint32_t>(random.Next());
      memory.strict_modulus = std::array{0u, 7u, 31u}[random.Below(3)];
      for (auto &word : memory.words) {
        word = safe || random.Chance(40) ? FuzzPointer(random.Below(FuzzMemoryWords))
               : random.Chance(50)       ? random.Below(64)
                                         : static_cast<uint32_t>(random.Next());
      }
      std::vector<uint32_t> user_data(safe || random.Chance(85) ? 16u : 8u);
      for (auto &word : user_data) {
        word = safe || random.Chance(60) ? FuzzPointer(random.Below(FuzzMemoryWords))
                                         : random.Below(64);
      }
      const SrtRuntime runtime{
          .user_data = user_data,
          .shader_base = random.Chance(50) ? FuzzMemoryBase : random.Next(),
          .read_memory = ReadFuzzOrdinary,
          .userdata = &memory,
          .read_specialization_memory = random.Chance(85) ? ReadFuzzStrict : nullptr};
      std::vector<FuzzRead> reference_reads;
      std::vector<FuzzRead> compiled_reads;
      memory.log = &reference_reads;
      const bool reference = MaterializeResources(
          plan, runtime, reference_snapshot, reference_specialization);
      memory.log = &compiled_reads;
      const bool compiled = MaterializeCompiledSrtResources(plan, runtime, compiled_snapshot,
                                                 compiled_specialization);
      Check(reference == compiled, "compiled walker changed the refresh result");
      Check(reference_reads == compiled_reads,
            "compiled walker changed the guest reads");
      Check((reference_snapshot == compiled_snapshot),
            "compiled walker changed the resource snapshot");
      Check(reference_specialization == compiled_specialization,
            "compiled walker changed the resource specialization");
      (reference ? successes : failures)++;
    }
  }
  // Both outcomes must be common for the comparison to mean anything.
  Check(successes > 2000 && failures > 2000,
        "fuzz plans did not exercise both refresh outcomes");
  std::printf("CompiledSrt differential: 9000 refreshes, %u successful, %u failed, identical reads and outputs\n", successes, failures);
}

void TestCompiledWalkerDirectReads() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  // The production reader copies guest memory directly.
  const std::array<uint32_t, 2> words{0x12345678u, 0x9abcdef0u};
  auto plan = SrtPlan(reinterpret_cast<uint64_t>(words.data()));
  Check(CompileSrtPlan(plan) != nullptr, "direct SRT plan was not compiled");
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  ResourceSnapshot reference_snapshot;
  ResourceSpecialization reference_specialization;
  uint32_t specialization_reads = 0;
  const SrtRuntime runtime{.userdata = &specialization_reads,
                           .read_specialization_memory = RejectSpecializationRead};
  Check(MaterializeCompiledSrtResources(plan, runtime, snapshot, specialization) &&
            MaterializeResources(plan, runtime, reference_snapshot,
                                          reference_specialization),
        "direct SRT refresh failed");
  Check(snapshot.flattened_srt == std::vector<uint32_t>{words[0]} &&
            (snapshot == reference_snapshot) && specialization_reads == 0,
        "compiled walker changed a direct SRT read");
}


void TestCompiledSrtShadowAndFallback() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  struct Reads { uint32_t calls = 0; bool success = true; } reads;
  const std::array<uint32_t,3> data {0x1000, 0, 512};
  SrtRuntime runtime {.user_data=data, .read_memory=+[](void* p, uint64_t address, std::span<uint32_t> out) {
    auto& r=*static_cast<Reads*>(p); ++r.calls;
    if (!r.success) return false;
    out[0]=0x8000+static_cast<uint32_t>(address); return true;
  }, .userdata=&reads};
  auto plan=FlatRecipePlan(4);
  ResourceSnapshot snapshot; ResourceSpecialization specialization;
  CompiledSrtEvaluationStats stats;
  Check(ShadowCompiledSrtMaterializeResources(plan,runtime,snapshot,specialization,stats) &&
        reads.calls==4 && stats.full_checks==1 && stats.full_mismatches==0 && plan.compiled_srt_verified,
        "compiled shadow repeated guest reads or failed full validation");
  const auto reference_snapshot=snapshot;
  reads.calls=0; reads.success=false;
  Check(!ShadowCompiledSrtMaterializeResources(plan,runtime,snapshot,specialization,stats) &&
        reads.calls==1 && stats.full_mismatches==0,
        "compiled shadow changed failed-read behavior");
  // Inject an incorrect compiled address. Replay rejects the changed read order/address,
  // returns the original output and never follows the bad pointer into guest memory.
  reads.calls=0; reads.success=true;
  auto& nodes=const_cast<CompiledSrtPlan*>(plan.compiled_srt.get())->nodes;
  bool injected=false;
  for (auto& node:nodes) if (node.shape==CompiledSrtPlan::Shape::LeafAddressRead) {
    node.imm+=4; injected=true; break;
  }
  Check(injected,"shadow rejection case has no compiled address");
  Check(ShadowCompiledSrtMaterializeResources(plan,runtime,snapshot,specialization,stats) &&
        reads.calls==4 && snapshot==reference_snapshot && stats.full_mismatches==1 && plan.compiled_srt_rejected,
        "compiled mismatch did not reject or preserve reference output");
  // The compiler visits every descriptor, including an unreferenced source the
  // interpreter never evaluates. Declining that graph must preserve the refresh.
  auto fallback=FlatRecipePlan(1);
  auto& extract=fallback.value_storage.emplace_back(ValueOpcode::CompositeExtractU32x2);
  extract.SetArg(0,Value(1u)); extract.SetArg(1,Value(0u));
  DescriptorSource unused;
  unused.dword_count=1;
  unused.dwords[0]=Value(&extract);
  fallback.descriptor_sources.push_back(unused);
  Check(CompileSrtPlan(fallback)==nullptr,"unsupported plan unexpectedly compiled");
  ResourceSnapshot expected; ResourceSpecialization expected_key;
  Check(MaterializeResources(fallback,runtime,expected,expected_key) &&
        MaterializeCompiledSrtResources(fallback,runtime,snapshot,specialization) &&
        snapshot==expected && specialization==expected_key,
        "unsupported compiled plan did not use the reference fallback");
}

void TestCompiledSrtLiveControl() {
  using namespace Libs::Graphics;
  using namespace ShaderRecompiler::IR;
  const auto control_path=std::filesystem::temp_directory_path() /
    ("kyty-compiled-srt-test-"+std::to_string(SrtEvaluationProfile::Clock())+".control");
  const auto output_path=control_path.string()+".csv";
#ifdef _WIN32
  _putenv_s("KYTY_COMPILED_SRT_CONTROL_FILE",control_path.string().c_str());
  _putenv_s("KYTY_COMPILED_SRT_CSV",output_path.c_str());
#else
  setenv("KYTY_COMPILED_SRT_CONTROL_FILE",control_path.string().c_str(),1);
  setenv("KYTY_COMPILED_SRT_CSV",output_path.c_str(),1);
#endif
  const auto request=[&](const char* mode) {
    std::ofstream file(control_path); file << mode << '\n'; file.close();
    compiled_control_test_clock+=1'000'000'000ull;
  };
  auto plan=FlatRecipePlan(4);
  const std::array<uint32_t,3> data{0x1000,0,512};
  uint32_t reads=0;
  SrtRuntime runtime{.user_data=data,.read_memory=+[](void* p,uint64_t address,std::span<uint32_t> out) {
    ++*static_cast<uint32_t*>(p); out[0]=0x8000+static_cast<uint32_t>(address); return true;
  },.userdata=&reads};
  ResourceSnapshot snapshot; ResourceSpecialization key;
  const auto refresh=[&] { return MaterializeCompiledSrtExperiment(plan,runtime,snapshot,key); };
  request("off");
  Check(refresh() && !plan.compiled_srt_built,"off control compiled or enabled the graph");
  const auto expected=snapshot;
  request("shadow"); reads=0;
  Check(refresh() && reads==4 && plan.compiled_srt_full_checks==1 && plan.compiled_srt_verified,
        "live shadow did not validate or repeated guest reads");
  request("on");
  Check(refresh() && snapshot==expected && plan.compiled_srt_full_checks==1,
        "live on did not use the validated graph");
  // Inject a compiled address fault just before the plan's periodic on comparison.
  for (auto& node:const_cast<CompiledSrtPlan*>(plan.compiled_srt.get())->nodes)
    if (node.shape==CompiledSrtPlan::Shape::LeafAddressRead) { node.imm+=4; break; }
  plan.compiled_srt_sequence=511; reads=0;
  Check(refresh() && reads==4 && snapshot==expected && plan.compiled_srt_rejected,
        "enabled periodic check did not reject and preserve reference output");
  request("on"); reads=0;
  Check(refresh() && reads==4 && snapshot==expected,
        "live request re-enabled a rejected graph");
  // Off transition flushes the previous stage. Check actual candidate accounting.
  std::ifstream csv(output_path); std::string content((std::istreambuf_iterator<char>(csv)),{});
  Check(content.find(",on,total,")!=std::string::npos &&
        content.find(",2,1,0,1,0,")!=std::string::npos,
        "controller did not record one candidate and one periodic comparison");
  std::filesystem::remove(control_path);
  std::printf("CompiledSrt live-control recovery: PASS; CSV %s\n",output_path.c_str());
}

void BenchmarkFlatSrtRecipes(bool compiled_graph = false) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan=FlatRecipePlan(96);
  const std::array<uint32_t,3> data{0x1000,0,512};
  SrtRuntime runtime{.user_data=data,.read_memory=+[](void*,uint64_t address,std::span<uint32_t> out) {
    out[0]=0x8000+static_cast<uint32_t>(address); return true;
  }};
  ResourceSnapshot snapshot; ResourceSpecialization key;
  std::puts("mode,calls,materializer_ms,ns_per_call");
  for (uint32_t round=0; round<12; ++round) {
    const bool candidate=(round%2)!=0;
    runtime.compiled_flat_srt=!compiled_graph && candidate;
    const auto refresh=[&] { return compiled_graph && candidate ?
      MaterializeCompiledSrtResources(plan,runtime,snapshot,key) : MaterializeResources(plan,runtime,snapshot,key); };
    for (uint32_t i=0;i<100;++i) Check(refresh(),"benchmark warmup failed");
    const auto begin=SrtEvaluationProfile::Clock();
    for (uint32_t i=0;i<5000;++i) Check(refresh(),"benchmark failed");
    const auto elapsed=SrtEvaluationProfile::Clock()-begin;
    std::printf("%s,5000,%.6f,%.3f\n",candidate ? "on" : "off",elapsed/1e6,elapsed/5000.0);
  }
}

} // namespace

namespace Common {

int DbgExitHandler(const char *, int, std::string_view) { std::abort(); }

int DbgExitHandler(const char *, int, fmt::text_style, std::string_view) {
  std::abort();
}

int DbgExitIfHandler(const char *, const char *, int) { return 1; }

void DbgExit(int) { std::abort(); }

} // namespace Common

int main(int argc, char** argv) {
  if (argc==2 && std::string_view(argv[1])=="--benchmark-flat-srt") { BenchmarkFlatSrtRecipes(); return 0; }
  if (argc==2 && std::string_view(argv[1])=="--benchmark-compiled-srt") { BenchmarkFlatSrtRecipes(true); return 0; }
  {
    using namespace Libs::Graphics;
    using ShaderRecompiler::IR::DescriptorEvaluationMode;
    Check(ChooseFlatSrtAction(DescriptorEvaluationMode::Off,true,true,10,1,false)==FlatSrtAction::Reference,
          "off enabled a validated recipe");
    Check(ChooseFlatSrtAction(DescriptorEvaluationMode::Fast,true,false,0,1,false)==FlatSrtAction::Compare,
          "new plan enabled without a complete comparison");
    Check(ChooseFlatSrtAction(DescriptorEvaluationMode::Fast,true,false,1,2,false)==FlatSrtAction::Reference,
          "zero executed coverage was treated as verified");
    Check(ChooseFlatSrtAction(DescriptorEvaluationMode::Fast,true,true,1,2,false)==FlatSrtAction::Candidate,
          "validated plan did not execute candidate");
    Check(ChooseFlatSrtAction(DescriptorEvaluationMode::Fast,true,true,1,512,false)==FlatSrtAction::Compare,
          "enabled plan lost periodic revalidation");
    Check(ChooseFlatSrtAction(DescriptorEvaluationMode::Fast,false,true,1,2,false)==FlatSrtAction::Reference &&
          ChooseFlatSrtAction(DescriptorEvaluationMode::Fast,true,true,1,2,true)==FlatSrtAction::Reference,
          "unsupported or rejected plan enabled");
    Check(ChooseFlatSrtAction(DescriptorEvaluationMode::Shadow,true,true,1,64,false)==FlatSrtAction::Compare &&
          ChooseFlatSrtAction(DescriptorEvaluationMode::Shadow,false,false,1,64,false)==FlatSrtAction::Reference,
          "shadow sampling policy lost plan-local gating");
  }
  TestCompiledWalkerMatchesReference();
  TestCompiledWalkerDirectReads();
  TestCompiledSrtShadowAndFallback();
  TestCompiledSrtLiveControl();
  TestFlatSrtRecipeShadow();
  TestSampledEvaluatorProfilePreservesOutput();
  TestDescriptorGatherEvaluation();
  TestFullDescriptorShadow();
  TestInactiveDescriptorRemainsInactive();
  TestMappedSrtUsesDirectReaderByDefault();
  TestIntegerRuntimeValueFollowsSrtReads();
  TestUnbasedFlatCacheHitMaterializes();
  TestWrittenDescriptorUsesStrictReaderOnce();
  TestFailedMaterializationRejectsStage();
  TestFiniteImageRefreshReusesScalarReads();
  TestFiniteImageRefreshReusesScalarReads(true);
  TestMixedSamplerVariantsShareRuntimeDescriptor();
  std::puts("ResourceMaterializationTests: all cases passed");
  return 0;
}

// Keep this focused standalone target self-contained by amalgamating its small
// typed-IR implementation set.
#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
