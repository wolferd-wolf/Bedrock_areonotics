#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aeronautics::bedrock {

struct MemoryRegion {
    std::uintptr_t start{};
    std::uintptr_t end{};
    bool readable{};
    bool executable{};
};

struct ModuleFingerprint {
    std::string moduleName;
    std::filesystem::path path;
    std::uintmax_t fileSize{};
    std::uintptr_t loadBase{};
    std::string buildId;
    std::vector<MemoryRegion> regions;
};

struct CompatibilityProfile final {
    static constexpr std::string_view minecraftVersion{"1.26.33.1"};
    static constexpr std::string_view moduleName{"libminecraftpe.so"};

    // Sourced from LeviLaunchroid's maintained preloader signature rules for
    // Minecraft 1.26.30.00 and newer. This callback is already used by the
    // launcher as a read-only menu-state probe.
    static constexpr std::string_view heartbeatSignature{
        "? ? ? D1 ? ? ? A9 ? ? ? F9 ? ? ? A9 ? ? ? 91 ? ? ? D5 "
        "F3 03 00 AA ? ? ? F9 ? ? ? F8 ? ? ? F9 ? ? ? 38 ? ? ? F9 "
        "E8 03 00 91 20 01 3F D6 ? ? ? F9 ? ? ? B5 ? ? ? ? ? ? ? 91 "
        "? ? ? ? ? ? ? 91 ? ? ? ? ? ? ? 91 ? ? ? ? ? ? ? 91 ? ? ? 52 "
        "? ? ? 95"};
};

[[nodiscard]] std::optional<ModuleFingerprint>
inspectLoadedModule(std::string_view moduleName);

[[nodiscard]] bool isExecutableAddress(
    const ModuleFingerprint& module,
    std::uintptr_t address) noexcept;

[[nodiscard]] std::string readInstructionPrefix(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    std::size_t byteCount);

[[nodiscard]] bool writeCompatibilityReport(
    const ModuleFingerprint& module,
    std::uintptr_t heartbeatAddress,
    const std::filesystem::path& outputPath,
    std::string& errorMessage);

}  // namespace aeronautics::bedrock
