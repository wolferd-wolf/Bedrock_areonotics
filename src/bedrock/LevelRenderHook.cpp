#include "bedrock/LevelRenderHook.hpp"

#include "bedrock/CompatibilityProfile.hpp"
#include "bedrock/HeartbeatHook.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace aeronautics::bedrock {
namespace {

constexpr std::string_view expectedBuildId{
    "2e318db12824cadb2618754ab7c82fa96fb30659"};
constexpr std::uintmax_t expectedModuleFileSize = 349243744;
constexpr std::string_view rendererToken{"LevelRenderer"};
constexpr std::size_t renderSlotIndex = 24;
constexpr std::size_t pointerSize = sizeof(std::uintptr_t);
constexpr std::uint64_t heartbeatStabilityMilliseconds = 8000;
constexpr std::uint64_t heartbeatMaximumStallMilliseconds = 2000;
constexpr std::uint64_t maximumWritableScanBytes = 256ULL * 1024ULL * 1024ULL;

struct WritableRegion final {
    std::uintptr_t start{};
    std::uintptr_t end{};
    std::string permissions;
    std::string label;
};

struct TypeRecord final {
    std::uintptr_t nameAddress{};
    std::string name;
    std::vector<std::uintptr_t> typeInfos;
};

struct VtableRecord final {
    std::string typeName;
    std::uintptr_t typeInfo{};
    std::uintptr_t addressPoint{};
    std::uintptr_t slot24Address{};
    std::uintptr_t slot24Target{};
    bool slot24Executable{};
    std::string slot24Prefix;
    std::uint64_t writableReferences{};
    std::uintptr_t firstWritableReference{};
};

[[nodiscard]] bool containsRange(
    const MemoryRegion& region,
    std::uintptr_t address,
    std::size_t size) noexcept {
    return region.readable && address >= region.start && address < region.end &&
        size <= region.end - address;
}

[[nodiscard]] const MemoryRegion* findRegion(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    std::size_t size) noexcept {
    for (const auto& region : module.regions) {
        if (containsRange(region, address, size)) {
            return &region;
        }
    }
    return nullptr;
}

[[nodiscard]] bool readPointer(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    std::uintptr_t& output) noexcept {
    if (findRegion(module, address, sizeof(output)) == nullptr) {
        return false;
    }
    std::memcpy(&output, reinterpret_cast<const void*>(address), sizeof(output));
    return true;
}

[[nodiscard]] bool readSignedPointer(
    const ModuleFingerprint& module,
    std::uintptr_t address,
    std::ptrdiff_t& output) noexcept {
    if (findRegion(module, address, sizeof(output)) == nullptr) {
        return false;
    }
    std::memcpy(&output, reinterpret_cast<const void*>(address), sizeof(output));
    return true;
}

[[nodiscard]] std::string hexOffset(
    std::uintptr_t address,
    std::uintptr_t loadBase) {
    std::ostringstream stream;
    stream << "0x" << std::hex;
    if (address >= loadBase) {
        stream << (address - loadBase);
    } else {
        stream << address;
    }
    return stream.str();
}

[[nodiscard]] std::vector<WritableRegion> readWritableRegions() {
    std::vector<WritableRegion> regions;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream input(line);
        std::string range;
        std::string permissions;
        std::string offset;
        std::string device;
        std::string inode;
        if (!(input >> range >> permissions >> offset >> device >> inode)) {
            continue;
        }
        if (permissions.size() < 2 || permissions[0] != 'r' || permissions[1] != 'w') {
            continue;
        }
        const auto separator = range.find('-');
        if (separator == std::string::npos) {
            continue;
        }
        std::uintptr_t start = 0;
        std::uintptr_t end = 0;
        try {
            start = static_cast<std::uintptr_t>(
                std::stoull(range.substr(0, separator), nullptr, 16));
            end = static_cast<std::uintptr_t>(
                std::stoull(range.substr(separator + 1), nullptr, 16));
        } catch (...) {
            continue;
        }
        std::string label;
        std::getline(input, label);
        regions.push_back({start, end, permissions, label});
    }
    return regions;
}

[[nodiscard]] bool isDigit(char value) noexcept {
    return value >= '0' && value <= '9';
}

}  // namespace

LevelRenderHook::LevelRenderHook(
    ll::mod::NativeMod& mod,
    HeartbeatHook& heartbeat,
    LevelRenderBus& eventBus) noexcept
    : mMod(mod), mHeartbeat(heartbeat), mEventBus(eventBus) {}

LevelRenderHook::~LevelRenderHook() {
    uninstall();
}

bool LevelRenderHook::install() {
    if (mRunning.load(std::memory_order_acquire)) {
        return true;
    }

    mStatusPath = mMod.getDataDir() / "level-render-source-status.txt";
    mCensusPath = mMod.getDataDir() / "renderer-rtti-census.txt";
    mStopRequested.store(false, std::memory_order_release);
    mCompleted.store(false, std::memory_order_relaxed);
    mFailed.store(false, std::memory_order_relaxed);
    mCensusRuns.store(0, std::memory_order_relaxed);
    mTypeNamesFound.store(0, std::memory_order_relaxed);
    mTypeInfosFound.store(0, std::memory_order_relaxed);
    mVtablesFound.store(0, std::memory_order_relaxed);
    mExecutableSlot24Targets.store(0, std::memory_order_relaxed);
    mWritableVptrReferences.store(0, std::memory_order_relaxed);
    mReadableBytesScanned.store(0, std::memory_order_relaxed);
    mWritableBytesScanned.store(0, std::memory_order_relaxed);
    mFingerprintValidated.store(false, std::memory_order_relaxed);

    writeStatus("waiting_for_primary_heartbeat");
    try {
        mWorker = std::thread([this] { workerLoop(); });
    } catch (const std::system_error& error) {
        mFailed.store(true, std::memory_order_relaxed);
        writeStatus("worker_start_failed");
        mMod.getLogger().error(
            "Renderer RTTI census worker failed to start: {}", error.what());
        return false;
    }

    mRunning.store(true, std::memory_order_release);
    mMod.getLogger().info(
        "Read-only renderer RTTI census armed; render patching disabled; Minecraft code bytes modified=0");
    return true;
}

void LevelRenderHook::uninstall() noexcept {
    mStopRequested.store(true, std::memory_order_release);
    if (mWorker.joinable()) {
        mWorker.join();
    }
    mRunning.store(false, std::memory_order_release);
    writeStatus("stopped");
}

void LevelRenderHook::workerLoop() noexcept {
    using namespace std::chrono_literals;
    std::uint64_t previousCount = mHeartbeat.callCount();
    auto firstActivity = std::chrono::steady_clock::time_point{};
    auto lastAdvance = std::chrono::steady_clock::time_point{};

    while (!mStopRequested.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        const std::uint64_t count = mHeartbeat.callCount();
        if (count > previousCount) {
            previousCount = count;
            lastAdvance = now;
            if (firstActivity == std::chrono::steady_clock::time_point{}) {
                firstActivity = now;
                writeStatus("validating_primary_heartbeat_stability");
            }
        }

        if (firstActivity != std::chrono::steady_clock::time_point{}) {
            const auto stableFor = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - firstActivity);
            const auto stalledFor = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastAdvance);
            if (stalledFor.count() >
                static_cast<std::int64_t>(heartbeatMaximumStallMilliseconds)) {
                firstActivity = {};
                writeStatus("waiting_for_primary_heartbeat");
            } else if (stableFor.count() >=
                static_cast<std::int64_t>(heartbeatStabilityMilliseconds)) {
                break;
            }
        }
        std::this_thread::sleep_for(100ms);
    }

    if (mStopRequested.load(std::memory_order_acquire)) {
        return;
    }
    writeStatus("scanning_read_only_renderer_metadata");
    runCensus();
    writeStatus(mFailed.load(std::memory_order_relaxed) ? "census_failed" : "census_complete");
}

void LevelRenderHook::runCensus() noexcept {
    ++mCensusRuns;
    const auto module = inspectLoadedModule(CompatibilityProfile::moduleName);
    if (!module || module->buildId != expectedBuildId ||
        module->fileSize != expectedModuleFileSize) {
        mFailed.store(true, std::memory_order_relaxed);
        return;
    }
    mFingerprintValidated.store(true, std::memory_order_relaxed);

    std::vector<TypeRecord> types;
    std::set<std::uintptr_t> seenNames;

    for (const auto& region : module->regions) {
        if (!region.readable || region.executable || region.end <= region.start) {
            continue;
        }
        mReadableBytesScanned.fetch_add(region.end - region.start, std::memory_order_relaxed);
        const auto* bytes = reinterpret_cast<const char*>(region.start);
        const std::size_t length = static_cast<std::size_t>(region.end - region.start);
        for (std::size_t index = 0; index + rendererToken.size() < length; ++index) {
            if (std::memcmp(bytes + index, rendererToken.data(), rendererToken.size()) != 0) {
                continue;
            }
            std::size_t start = index;
            std::size_t digits = 0;
            while (start > 0 && digits < 4 && isDigit(bytes[start - 1])) {
                --start;
                ++digits;
            }
            if (digits == 0) {
                continue;
            }
            std::size_t end = index + rendererToken.size();
            while (end < length && end - start < 120 && bytes[end] != '\0') {
                const unsigned char value = static_cast<unsigned char>(bytes[end]);
                if (value < 0x20 || value > 0x7e) {
                    break;
                }
                ++end;
            }
            if (end >= length || bytes[end] != '\0') {
                continue;
            }
            const std::uintptr_t address = region.start + start;
            if (seenNames.insert(address).second) {
                types.push_back({address, std::string(bytes + start, end - start), {}});
            }
            index = end;
        }
    }

    mTypeNamesFound.store(types.size(), std::memory_order_relaxed);

    // Itanium type_info stores the type-name pointer at +8. Locate every
    // pointer to each discovered name inside readable non-executable module data.
    for (auto& type : types) {
        for (const auto& region : module->regions) {
            if (!region.readable || region.executable || region.end <= region.start) {
                continue;
            }
            const std::uintptr_t aligned =
                (region.start + pointerSize - 1U) & ~(pointerSize - 1U);
            for (std::uintptr_t address = aligned;
                 address + pointerSize <= region.end;
                 address += pointerSize) {
                std::uintptr_t value = 0;
                std::memcpy(&value, reinterpret_cast<const void*>(address), pointerSize);
                if (value == type.nameAddress && address >= module->loadBase + pointerSize) {
                    type.typeInfos.push_back(address - pointerSize);
                }
            }
        }
        std::sort(type.typeInfos.begin(), type.typeInfos.end());
        type.typeInfos.erase(
            std::unique(type.typeInfos.begin(), type.typeInfos.end()),
            type.typeInfos.end());
        mTypeInfosFound.fetch_add(type.typeInfos.size(), std::memory_order_relaxed);
    }

    std::vector<VtableRecord> vtables;
    std::set<std::uintptr_t> seenVtables;
    for (const auto& type : types) {
        for (const std::uintptr_t typeInfo : type.typeInfos) {
            for (const auto& region : module->regions) {
                if (!region.readable || region.executable || region.end <= region.start) {
                    continue;
                }
                const std::uintptr_t aligned =
                    (region.start + pointerSize - 1U) & ~(pointerSize - 1U);
                for (std::uintptr_t address = aligned;
                     address + pointerSize <= region.end;
                     address += pointerSize) {
                    std::uintptr_t value = 0;
                    std::memcpy(&value, reinterpret_cast<const void*>(address), pointerSize);
                    if (value != typeInfo || address < region.start + pointerSize) {
                        continue;
                    }
                    std::ptrdiff_t offsetToTop = 1;
                    if (!readSignedPointer(*module, address - pointerSize, offsetToTop) ||
                        offsetToTop != 0) {
                        continue;
                    }
                    const std::uintptr_t addressPoint = address + pointerSize;
                    if (!seenVtables.insert(addressPoint).second) {
                        continue;
                    }
                    const std::uintptr_t slotAddress =
                        addressPoint + renderSlotIndex * pointerSize;
                    std::uintptr_t target = 0;
                    if (!readPointer(*module, slotAddress, target)) {
                        continue;
                    }
                    const bool executable = isExecutableAddress(*module, target);
                    vtables.push_back({
                        type.name,
                        typeInfo,
                        addressPoint,
                        slotAddress,
                        target,
                        executable,
                        executable ? readInstructionPrefix(*module, target, 16) : std::string{},
                        0,
                        0});
                    if (executable) {
                        mExecutableSlot24Targets.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }

    mVtablesFound.store(vtables.size(), std::memory_order_relaxed);

    // Search readable+writable mappings for object vptrs equal to discovered
    // address points. This is read-only and capped to bound phone-side cost.
    std::uint64_t remaining = maximumWritableScanBytes;
    const auto writableRegions = readWritableRegions();
    for (const auto& region : writableRegions) {
        if (remaining < pointerSize || region.end <= region.start) {
            break;
        }
        const std::uint64_t regionBytes = region.end - region.start;
        const std::uint64_t scanBytes = std::min(regionBytes, remaining);
        const std::uintptr_t scanEnd = region.start + scanBytes;
        const std::uintptr_t aligned =
            (region.start + pointerSize - 1U) & ~(pointerSize - 1U);
        for (std::uintptr_t address = aligned;
             address + pointerSize <= scanEnd;
             address += pointerSize) {
            std::uintptr_t value = 0;
            std::memcpy(&value, reinterpret_cast<const void*>(address), pointerSize);
            for (auto& vtable : vtables) {
                if (value == vtable.addressPoint) {
                    ++vtable.writableReferences;
                    if (vtable.firstWritableReference == 0) {
                        vtable.firstWritableReference = address;
                    }
                    mWritableVptrReferences.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        mWritableBytesScanned.fetch_add(scanBytes, std::memory_order_relaxed);
        remaining -= scanBytes;
    }

    std::ofstream output(mCensusPath, std::ios::trunc);
    if (!output) {
        mFailed.store(true, std::memory_order_relaxed);
        return;
    }

    output << "schema=1\n"
           << "mode=read_only_live_renderer_rtti_census\n"
           << "minecraft_version=1.26.33.1\n"
           << "module_build_id=" << module->buildId << '\n'
           << "module_file_size=" << module->fileSize << '\n'
           << "module_load_base=0x" << std::hex << module->loadBase << std::dec << '\n'
           << "render_patch_attempted=false\n"
           << "minecraft_code_bytes_modified=0\n"
           << "type_names_found=" << types.size() << '\n'
           << "type_infos_found=" << mTypeInfosFound.load() << '\n'
           << "vtables_found=" << vtables.size() << '\n'
           << "executable_slot24_targets=" << mExecutableSlot24Targets.load() << '\n'
           << "writable_vptr_references=" << mWritableVptrReferences.load() << '\n'
           << "writable_scan_cap_bytes=" << maximumWritableScanBytes << '\n'
           << "columns=type_name,name_offset,typeinfo_offset,vtable_address_point_offset,slot24_address_offset,slot24_target_offset,slot24_executable,slot24_prefix,writable_vptr_references,first_writable_reference\n";

    for (const auto& vtable : vtables) {
        std::uintptr_t nameAddress = 0;
        for (const auto& type : types) {
            if (type.name == vtable.typeName &&
                std::find(type.typeInfos.begin(), type.typeInfos.end(), vtable.typeInfo) !=
                    type.typeInfos.end()) {
                nameAddress = type.nameAddress;
                break;
            }
        }
        output << vtable.typeName << ','
               << hexOffset(nameAddress, module->loadBase) << ','
               << hexOffset(vtable.typeInfo, module->loadBase) << ','
               << hexOffset(vtable.addressPoint, module->loadBase) << ','
               << hexOffset(vtable.slot24Address, module->loadBase) << ','
               << hexOffset(vtable.slot24Target, module->loadBase) << ','
               << (vtable.slot24Executable ? 1 : 0) << ','
               << '"' << vtable.slot24Prefix << '"' << ','
               << vtable.writableReferences << ",0x" << std::hex
               << vtable.firstWritableReference << std::dec << '\n';
    }

    output << "\n[type_names]\n";
    for (const auto& type : types) {
        output << type.name << ",name=" << hexOffset(type.nameAddress, module->loadBase)
               << ",typeinfo_count=" << type.typeInfos.size();
        for (const auto typeInfo : type.typeInfos) {
            output << ",typeinfo=" << hexOffset(typeInfo, module->loadBase);
        }
        output << '\n';
    }

    mCompleted.store(true, std::memory_order_release);
}

void LevelRenderHook::writeStatus(const char* state) noexcept {
    std::ofstream output(mStatusPath, std::ios::trunc);
    if (!output) {
        return;
    }
    output << "schema=3\n"
           << "state=" << state << '\n'
           << "source=read_only_renderer_rtti_census\n"
           << "source_mode=no_render_callback_installed\n"
           << "geometry_submission=disabled\n"
           << "render_patch_attempted=false\n"
           << "patch_ever_installed=false\n"
           << "patch_currently_installed=false\n"
           << "safe_to_unload=true\n"
           << "minecraft_code_bytes_modified=0\n"
           << "fingerprint_validated="
           << (mFingerprintValidated.load(std::memory_order_relaxed) ? "true" : "false") << '\n'
           << "census_runs=" << mCensusRuns.load(std::memory_order_relaxed) << '\n'
           << "type_names_found=" << mTypeNamesFound.load(std::memory_order_relaxed) << '\n'
           << "type_infos_found=" << mTypeInfosFound.load(std::memory_order_relaxed) << '\n'
           << "vtables_found=" << mVtablesFound.load(std::memory_order_relaxed) << '\n'
           << "executable_slot24_targets="
           << mExecutableSlot24Targets.load(std::memory_order_relaxed) << '\n'
           << "writable_vptr_references="
           << mWritableVptrReferences.load(std::memory_order_relaxed) << '\n'
           << "readable_bytes_scanned="
           << mReadableBytesScanned.load(std::memory_order_relaxed) << '\n'
           << "writable_bytes_scanned="
           << mWritableBytesScanned.load(std::memory_order_relaxed) << '\n'
           << "census_complete="
           << (mCompleted.load(std::memory_order_relaxed) ? "true" : "false") << '\n'
           << "census_failed="
           << (mFailed.load(std::memory_order_relaxed) ? "true" : "false") << '\n'
           << "event_bus_published_events=" << mEventBus.publishedEvents() << '\n'
           << "event_bus_delivered_callbacks=" << mEventBus.deliveredCallbacks() << '\n'
           << "census_file=renderer-rtti-census.txt\n";
}

}  // namespace aeronautics::bedrock
