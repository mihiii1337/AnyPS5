#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <span>

namespace {

constexpr std::uint32_t WaveSize = 32;
constexpr std::uint32_t Threads = 2 * WaveSize;
constexpr std::uint32_t NonzeroValue = 17;
constexpr std::uint32_t ZeroValue = 42;
alignas(256) std::array<std::uint32_t, Threads> Output{};

alignas(256) constexpr std::array<std::uint32_t, 9> Code{
    0xbeea0381u,
    0xbf860002u,
    0xbe880391u,
    0xbf820001u,
    0xbe8803aau,
    0x7e020208u,
    0xe0702000u, 0x80000100u,
    0xbf810000u,
};

std::array<std::uint32_t, 4> BufferDescriptor(std::span<std::uint32_t> values) {
    const auto address = reinterpret_cast<std::uintptr_t>(values.data());
    return {static_cast<std::uint32_t>(address),
            static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u),
            static_cast<std::uint32_t>(values.size()), 0x11016facu};
}

bool Run(AgcDriver::VulkanDevice& device) {
    Output.fill(0xdeadbeefu);
    const auto userData = BufferDescriptor(Output);
    const std::span<const std::uint32_t> code(Code);
    const auto codeAddress = reinterpret_cast<std::uintptr_t>(code.data());
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{codeAddress, std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, codeAddress, code, 0, {}},
        {WaveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128},
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, codeAddress);
    device.WaitIdle();

    bool passed = true;
    for (std::uint32_t wave = 0; wave < Threads / WaveSize; ++wave) {
        std::uint32_t nonzeroLanes = 0, zeroLanes = 0, unexpected = 0;
        for (std::uint32_t lane = 0; lane < WaveSize; ++lane) {
            const auto value = Output[wave * WaveSize + lane];
            if (value == NonzeroValue) nonzeroLanes |= 1u << lane;
            else if (value == ZeroValue) zeroLanes |= 1u << lane;
            else ++unexpected;
        }
        std::printf("wave=%u nonzero_lanes=0x%08x zero_lanes=0x%08x unexpected=%u\n",
                    wave, nonzeroLanes, zeroLanes, unexpected);
        if (nonzeroLanes != 0xffffffffu || zeroLanes || unexpected) passed = false;
    }
    return passed;
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        return Run(*device) ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
