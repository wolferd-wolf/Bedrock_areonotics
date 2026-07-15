#include "bedrock/LevelClassDiscovery.hpp"

#include "bedrock/CompatibilityProfile.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace aeronautics::bedrock {
namespace {

constexpr std::string_view expectedBuildId{
    "2e318db12824cadb2618754ab7c82fa96fb30659"};
constexpr std::uintmax_t expectedModuleFileSize = 349243744;
constexpr std::size_t maximumNameHitsPerPattern = 64;
constexpr std::size_t maximumPointerHitsPerName = 64;
constexpr std::size_t maximumVtableCandidatesPerTypeInfo = 64;
constexpr std::size_t recordedEntriesPerCandidate = 32;

struct Pattern final {
    std::string_view label;
    std::string_view bytes;
};

constexpr std::array<Pattern, 6> patterns{{
    {"server_level_plain", "ServerLevel"},
    {"server_level_itanium", "11ServerLevel"},
    {"multiplayer_level_plain", "MultiPlayerLevel"},
    {"multiplayer_level_itanium", "16MultiPlayerLevel"},
    {"level_plain", "Level"},
    {"level_itanium", "5Level"},
}};

[[nodiscard]] const MemoryRegion* findRegion(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    std::size_t size) noexcept {
    for (const MemoryRegion& region : module.regions) {
        if (region.readable && address >= region.start && address < region.end &&
            size <= region.end - address) {
            return &region;
        }
    }
    return nullptr;
}

[[nodiscard]] bool readPointer(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    std::uintptr_t& value) noexcept {
    if (findRegion(module, address, sizeof(value)) == nullptr) {
        return false;
    }
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return true;
}

[[nodiscard]] bool isReadableNonExecutable(
    const ModuleFingerprint& module,
    std::uintptr_t address) noexcept {
    const MemoryRegion* region = findRegion(module, address, 1);
    return region != nullptr && !region->executable;
}

[[nodiscard]] bool isExecutable(
    const ModuleFingerprint& module,
    std::uintptr_t address) noexcept {
    return isExecutableAddress(module, address);
}

[[nodiscard]] std::vector<std::uintptr_t> findBytes(
    const ModuleFingerprint& module,
    std::string_view needle,
    std::size_t limit,
    const std::atomic_bool& stopRequested) {
    std::vector<std::uintptr_t> results;
    if (needle.empty()) {
        return results;
    }
    for (const MemoryRegion& region : module.regions) {
        if (stopRequested.load(std::memory_order_acquire) ||
            !region.readable || region.executable ||
            region.end <= region.start ||
            needle.size() > region.end - region.start) {
            continue;
        }
        const auto* begin = reinterpret_cast<const char*>(region.start);
        const auto* end = reinterpret_cast<const char*>(region.end - needle.size() + 1);
        for (const char* cursor = begin; cursor < end; ++cursor) {
            if (std::memcmp(cursor, needle.data(), needle.size()) == 0) {
                results.push_back(reinterpret_cast<std::uintptr_t>(cursor));
                if (results.size() >= limit) {
                    return results;
                }
            }
        }
    }
    return results;
}

[[nodiscard]] std::vector<std::uintptr_t> findPointerReferences(
    const ModuleFingerprint& module,
    std::uintptr_t target,
    std::size_t limit,
    const std::atomic_bool& stopRequested) {
    std::vector<std::uintptr_t> results;
    for (const MemoryRegion& region : module.regions) {
        if (stopRequested.load(std::memory_order_acquire) ||
            !region.readable || region.executable ||
            region.end <= region.start ||
            sizeof(std::uintptr_t) > region.end - region.start) {
            continue;
        }
        const std::uintptr_t first =
            (region.start + alignof(std::uintptr_t) - 1U) &
            ~(static_cast<std::uintptr_t>(alignof(std::uintptr_t)) - 1U);
        for (std::uintptr_t address = first;
             address <= region.end - sizeof(std::uintptr_t);
             address += sizeof(std::uintptr_t)) {
            std::uintptr_t value = 0;
            std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
            if (value == target) {
                results.push_back(address);
                if (results.size() >= limit) {
                    return results;
                }
            }
        }
    }
    return results;
}

[[nodiscard]] std::size_t countExecutableEntries(
    const ModuleFingerprint& module,
    std::uintptr_t firstEntry,
    std::size_t maximum) noexcept {
    std::size_t count = 0;
    for (; count < maximum; ++count) {
        std::uintptr_t value = 0;
        if (!readPointer(
                module,
                firstEntry + count * sizeof(std::uintptr_t),
                value) ||
            !isExecutable(module, value)) {
            break;
        }
    }
    return count;
}

}  // namespace

LevelClassDiscovery::LevelClassDiscovery(ll::mod::NativeMod& mod) noexcept
    : mMod(mod) {}

LevelClassDiscovery::~LevelClassDiscovery() {
    stop();
}

bool LevelClassDiscovery::start() {
    if (mWorker.joinable()) {
        return true;
    }
    mReportPath = mMod.getDataDir() / "level-class-discovery.txt";
    mStopRequested.store(false, std::memory_order_release);
    try {
        mWorker = std::thread([this] { scan(); });
    } catch (const std::system_error& error) {
        mMod.getLogger().error(
            "Could not start level class discovery: {}",
            error.what());
        return false;
    }
    return true;
}

void LevelClassDiscovery::stop() noexcept {
    mStopRequested.store(true, std::memory_order_release);
    if (mWorker.joinable()) {
        mWorker.join();
    }
}

void LevelClassDiscovery::scan() noexcept {
    const auto startedAt = std::chrono::steady_clock::now();
    std::ofstream output(mReportPath, std::ios::trunc);
    if (!output) {
        return;
    }
    output << "schema=1\n"
           << "state=scanning\n"
           << "discovery_method=read_only_itanium_rtti_level_class_scan\n"
           << "minecraft_code_bytes_modified=0\n"
           << "target_classes=ServerLevel,MultiPlayerLevel,Level\n";
    output.flush();

    const auto module = inspectLoadedModule(CompatibilityProfile::moduleName);
    if (!module) {
        output << "state=failed\nfailure_reason=libminecraftpe.so is not loaded\n";
        return;
    }
    output << "module_build_id=" << module->buildId << '\n'
           << "module_file_size=" << module->fileSize << '\n';
    if (module->buildId != expectedBuildId ||
        module->fileSize != expectedModuleFileSize) {
        output << "state=failed\nfailure_reason=Minecraft fingerprint mismatch\n";
        return;
    }

    std::size_t totalNameHits = 0;
    std::size_t totalTypeInfoCandidates = 0;
    std::size_t totalVtableCandidates = 0;

    for (const Pattern& pattern : patterns) {
        if (mStopRequested.load(std::memory_order_acquire)) {
            break;
        }
        const auto nameHits = findBytes(
            *module,
            pattern.bytes,
            maximumNameHitsPerPattern,
            mStopRequested);
        output << "pattern." << pattern.label << ".name_hit_count="
               << nameHits.size() << '\n';
        totalNameHits += nameHits.size();

        for (std::size_t nameIndex = 0;
             nameIndex < nameHits.size();
             ++nameIndex) {
            const std::uintptr_t nameAddress = nameHits[nameIndex];
            output << "pattern." << pattern.label << ".name_hit."
                   << nameIndex << ".offset=0x" << std::hex
                   << (nameAddress - module->loadBase) << std::dec << '\n';

            const auto typeInfoNamePointerRefs = findPointerReferences(
                *module,
                nameAddress,
                maximumPointerHitsPerName,
                mStopRequested);
            output << "pattern." << pattern.label << ".name_hit."
                   << nameIndex << ".typeinfo_name_pointer_ref_count="
                   << typeInfoNamePointerRefs.size() << '\n';
            totalTypeInfoCandidates += typeInfoNamePointerRefs.size();

            for (std::size_t typeIndex = 0;
                 typeIndex < typeInfoNamePointerRefs.size();
                 ++typeIndex) {
                const std::uintptr_t namePointerField =
                    typeInfoNamePointerRefs[typeIndex];
                if (namePointerField < sizeof(std::uintptr_t)) {
                    continue;
                }
                const std::uintptr_t typeInfoAddress =
                    namePointerField - sizeof(std::uintptr_t);
                if (!isReadableNonExecutable(*module, typeInfoAddress)) {
                    continue;
                }
                output << "pattern." << pattern.label << ".name_hit."
                       << nameIndex << ".typeinfo." << typeIndex
                       << ".offset=0x" << std::hex
                       << (typeInfoAddress - module->loadBase) << std::dec << '\n';

                const auto typeInfoRefs = findPointerReferences(
                    *module,
                    typeInfoAddress,
                    maximumVtableCandidatesPerTypeInfo,
                    mStopRequested);
                for (std::size_t refIndex = 0;
                     refIndex < typeInfoRefs.size();
                     ++refIndex) {
                    const std::uintptr_t typeInfoField = typeInfoRefs[refIndex];
                    const std::uintptr_t firstEntry =
                        typeInfoField + sizeof(std::uintptr_t);
                    const std::size_t executableEntries =
                        countExecutableEntries(
                            *module,
                            firstEntry,
                            recordedEntriesPerCandidate);
                    if (executableEntries < 4) {
                        continue;
                    }
                    const std::size_t candidateIndex = totalVtableCandidates++;
                    output << "vtable_candidate." << candidateIndex
                           << ".pattern=" << pattern.label << '\n'
                           << "vtable_candidate." << candidateIndex
                           << ".typeinfo_offset=0x" << std::hex
                           << (typeInfoAddress - module->loadBase) << std::dec << '\n'
                           << "vtable_candidate." << candidateIndex
                           << ".address_point_offset=0x" << std::hex
                           << (firstEntry - module->loadBase) << std::dec << '\n'
                           << "vtable_candidate." << candidateIndex
                           << ".recorded_executable_entries="
                           << executableEntries << '\n';
                    for (std::size_t entry = 0;
                         entry < executableEntries;
                         ++entry) {
                        std::uintptr_t target = 0;
                        if (!readPointer(
                                *module,
                                firstEntry + entry * sizeof(std::uintptr_t),
                                target)) {
                            break;
                        }
                        output << "vtable_candidate." << candidateIndex
                               << ".entry." << entry << ".target_offset=0x"
                               << std::hex << (target - module->loadBase)
                               << std::dec << '\n';
                    }
                }
            }
        }
    }

    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt);
    output << "total_name_hits=" << totalNameHits << '\n'
           << "total_typeinfo_candidates=" << totalTypeInfoCandidates << '\n'
           << "total_vtable_candidates=" << totalVtableCandidates << '\n'
           << "scan_duration_ms=" << duration.count() << '\n'
           << "state="
           << (mStopRequested.load(std::memory_order_acquire)
                   ? "cancelled"
                   : "complete")
           << '\n'
           << "next_target_method=ServerLevel::_subTick_or_MultiPlayerLevel::_subTick\n";
    mMod.getLogger().info(
        "Read-only level class discovery finished: names={}, typeinfo={}, vtables={}, report={}",
        totalNameHits,
        totalTypeInfoCandidates,
        totalVtableCandidates,
        mReportPath);
}

}  // namespace aeronautics::bedrock
