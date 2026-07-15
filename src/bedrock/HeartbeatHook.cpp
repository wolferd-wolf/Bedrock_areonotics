#include "bedrock/HeartbeatHook.hpp"

#include "bedrock/CompatibilityProfile.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <vector>

#include <pl/memory/Signature.hpp>

namespace aeronautics::bedrock {
namespace {

constexpr std::size_t maximumRecordedReferences = 256;
constexpr std::size_t maximumDispatchCandidates = 32;
constexpr std::size_t maximumNeighbourEntries = 17;
constexpr std::size_t maximumTableWalkEntries = 512;
constexpr std::size_t indirectSearchInstructionWindow = 4;
constexpr std::uintptr_t arm64InstructionAlignment = 4;
constexpr std::uintptr_t pointerAlignment = 8;
constexpr std::uint32_t arm64BranchOpcodeMask = 0xFC000000U;
constexpr std::uint32_t arm64BranchOpcode = 0x14000000U;
constexpr std::uint32_t arm64BranchLinkOpcode = 0x94000000U;
constexpr std::uint32_t arm64ImmediateMask = 0x03FFFFFFU;
constexpr std::uint32_t arm64ImmediateSignBit = 0x02000000U;
constexpr std::uint32_t arm64LdrUnsigned64Mask = 0xFFC00000U;
constexpr std::uint32_t arm64LdrUnsigned64Opcode = 0xF9400000U;
constexpr std::uint32_t arm64RegisterBranchMask = 0xFFFFFC1FU;
constexpr std::uint32_t arm64BranchRegisterOpcode = 0xD61F0000U;
constexpr std::uint32_t arm64BranchLinkRegisterOpcode = 0xD63F0000U;
constexpr std::uint64_t cancellationCheckStride = 1024U * 1024U;

[[nodiscard]] std::uintptr_t alignUp(
    std::uintptr_t value,
    std::uintptr_t alignment) noexcept {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

[[nodiscard]] std::uintptr_t decodeArm64BranchDestination(
    std::uintptr_t instructionAddress,
    std::uint32_t instruction) noexcept {
    std::int64_t immediate =
        static_cast<std::int64_t>(instruction & arm64ImmediateMask);
    if ((instruction & arm64ImmediateSignBit) != 0U) {
        immediate |= ~static_cast<std::int64_t>(arm64ImmediateMask);
    }
    const std::int64_t displacement = immediate * 4;
    return static_cast<std::uintptr_t>(
        static_cast<std::int64_t>(instructionAddress) + displacement);
}

[[nodiscard]] bool decodeArm64LdrUnsigned64(
    std::uint32_t instruction,
    std::uint8_t& destinationRegister,
    std::uint8_t& baseRegister,
    std::uint32_t& byteOffset) noexcept {
    if ((instruction & arm64LdrUnsigned64Mask) != arm64LdrUnsigned64Opcode) {
        return false;
    }

    destinationRegister = static_cast<std::uint8_t>(instruction & 0x1FU);
    baseRegister = static_cast<std::uint8_t>((instruction >> 5U) & 0x1FU);
    const std::uint32_t immediate = (instruction >> 10U) & 0xFFFU;
    byteOffset = immediate * 8U;
    return true;
}

[[nodiscard]] bool decodeArm64RegisterBranch(
    std::uint32_t instruction,
    std::uint8_t& sourceRegister,
    bool& link) noexcept {
    const std::uint32_t opcode = instruction & arm64RegisterBranchMask;
    if (opcode == arm64BranchLinkRegisterOpcode) {
        link = true;
    } else if (opcode == arm64BranchRegisterOpcode) {
        link = false;
    } else {
        return false;
    }

    sourceRegister = static_cast<std::uint8_t>((instruction >> 5U) & 0x1FU);
    return true;
}

[[nodiscard]] const MemoryRegion* findContainingRegion(
    const std::vector<MemoryRegion>& regions,
    std::uintptr_t address) noexcept {
    for (const MemoryRegion& region : regions) {
        if (address >= region.start && address < region.end) {
            return &region;
        }
    }
    return nullptr;
}

[[nodiscard]] bool isExecutableModuleAddress(
    const std::vector<MemoryRegion>& regions,
    std::uintptr_t address) noexcept {
    const MemoryRegion* const region = findContainingRegion(regions, address);
    return region != nullptr && region->readable && region->executable;
}

[[nodiscard]] std::uintptr_t readPointer(std::uintptr_t address) noexcept {
    std::uintptr_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

enum class PointerValueKind : std::uint8_t {
    Null,
    SmallScalar,
    ModuleExecutable,
    ModuleReadableData,
    ModuleOther,
    OutsideModule,
};

[[nodiscard]] PointerValueKind classifyPointerValue(
    const std::vector<MemoryRegion>& regions,
    std::uintptr_t value) noexcept {
    if (value == 0) {
        return PointerValueKind::Null;
    }
    if (value < 4096U) {
        return PointerValueKind::SmallScalar;
    }

    const MemoryRegion* const region = findContainingRegion(regions, value);
    if (region == nullptr) {
        return PointerValueKind::OutsideModule;
    }
    if (region->readable && region->executable) {
        return PointerValueKind::ModuleExecutable;
    }
    if (region->readable) {
        return PointerValueKind::ModuleReadableData;
    }
    return PointerValueKind::ModuleOther;
}

[[nodiscard]] const char* pointerValueKindName(PointerValueKind kind) noexcept {
    switch (kind) {
    case PointerValueKind::Null:
        return "null";
    case PointerValueKind::SmallScalar:
        return "small_scalar";
    case PointerValueKind::ModuleExecutable:
        return "module_executable";
    case PointerValueKind::ModuleReadableData:
        return "module_readable_data";
    case PointerValueKind::ModuleOther:
        return "module_other";
    case PointerValueKind::OutsideModule:
        return "outside_module";
    }
    return "unknown";
}

}  // namespace

struct HeartbeatHook::DiscoveryState final {
    struct BranchReference final {
        std::uintptr_t address{};
        bool link{};
    };

    struct TableNeighbour final {
        std::int32_t relativeIndex{};
        std::uintptr_t entryAddress{};
        std::uintptr_t value{};
        PointerValueKind kind{PointerValueKind::Null};
    };

    struct DispatchTableCandidate final {
        std::uintptr_t pointerAddress{};
        std::uintptr_t runStart{};
        std::uintptr_t runEnd{};
        std::uint32_t entryCount{};
        std::uint32_t slotIndex{};
        std::uint32_t slotOffsetBytes{};
        bool vtableLike{};
        std::vector<TableNeighbour> neighbours;
    };

    struct IndirectCallReference final {
        std::uintptr_t address{};
        std::uint32_t slotOffsetBytes{};
        std::uint8_t objectRegister{};
        std::uint8_t vtableRegister{};
        std::uint8_t functionRegister{};
        std::uint8_t vtableLoadDistance{};
        std::uint8_t branchDistance{};
        bool link{};
    };

    std::uintptr_t moduleLoadBase{};
    std::uintptr_t targetAddress{};
    std::uintmax_t moduleFileSize{};
    std::string moduleBuildId;
    std::vector<MemoryRegion> regions;
    std::vector<BranchReference> branchReferences;
    std::vector<std::uintptr_t> pointerReferences;
    std::vector<DispatchTableCandidate> dispatchTables;
    std::vector<IndirectCallReference> indirectCalls;
    std::uint64_t executableBytesScanned{};
    std::uint64_t readableDataBytesScanned{};
    std::uint64_t indirectExecutableBytesScanned{};
    std::uint64_t directBranchReferenceCount{};
    std::uint64_t pointerReferenceCount{};
    std::uint64_t indirectCallReferenceCount{};
    std::uint64_t scanDurationMilliseconds{};
    bool scanStarted{};
    bool scanComplete{};
    bool scanCancelled{};
};

std::atomic<HeartbeatHook*> HeartbeatHook::sActive{nullptr};

HeartbeatHook::HeartbeatHook(ll::mod::NativeMod& mod) noexcept
    : mMod(mod) {}

HeartbeatHook::~HeartbeatHook() {
    uninstall();
}

bool HeartbeatHook::install() {
    if (installed()) {
        return true;
    }

    const auto module = inspectLoadedModule(CompatibilityProfile::moduleName);
    if (!module) {
        mMod.getLogger().error(
            "Compatibility gate failed: {} is not loaded",
            CompatibilityProfile::moduleName);
        return false;
    }

    const std::uintptr_t target = pl::memory::resolveSignature(
        CompatibilityProfile::heartbeatSignature,
        CompatibilityProfile::moduleName);
    if (target == 0) {
        mMod.getLogger().error(
            "Compatibility gate failed: 1.26.33.1 heartbeat signature was not found");
        return false;
    }
    if ((target & 0x3U) != 0U || !isExecutableAddress(*module, target)) {
        mMod.getLogger().error(
            "Compatibility gate failed: heartbeat target 0x{:x} is not an aligned executable address",
            target);
        return false;
    }

    const std::string prefix = readInstructionPrefix(*module, target, 32);
    if (prefix.empty()) {
        mMod.getLogger().error(
            "Compatibility gate failed: could not read heartbeat target prefix");
        return false;
    }

    const std::filesystem::path reportPath =
        mMod.getDataDir() / "compatibility-report.txt";
    std::string reportError;
    if (!writeCompatibilityReport(*module, target, reportPath, reportError)) {
        mMod.getLogger().warn(
            "Could not write compatibility report {}: {}",
            reportPath,
            reportError);
    }

    try {
        mDiscovery = std::make_unique<DiscoveryState>();
        mDiscovery->moduleLoadBase = module->loadBase;
        mDiscovery->targetAddress = target;
        mDiscovery->moduleFileSize = module->fileSize;
        mDiscovery->moduleBuildId = module->buildId;
        mDiscovery->regions = module->regions;
        mDiscovery->branchReferences.reserve(maximumRecordedReferences);
        mDiscovery->pointerReferences.reserve(maximumRecordedReferences);
        mDiscovery->dispatchTables.reserve(maximumDispatchCandidates);
        mDiscovery->indirectCalls.reserve(maximumRecordedReferences);
    } catch (const std::bad_alloc&) {
        mMod.getLogger().error(
            "Compatibility gate passed, but indirect dispatch discovery storage could not be allocated");
        return false;
    }

    mStatusPath = mMod.getDataDir() / "heartbeat-status.txt";
    mDiscoveryPath = mMod.getDataDir() / "tick-discovery-profile.txt";
    mCallCount.store(0, std::memory_order_relaxed);
    mInFlightCallbacks.store(0, std::memory_order_relaxed);
    mStopRequested.store(false, std::memory_order_release);
    mFirstCallbackLogged.store(false, std::memory_order_release);
    mOriginalStorage = nullptr;
    mOriginalCallable.store(nullptr, std::memory_order_release);
    sActive.store(this, std::memory_order_release);

    mHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(target),
        reinterpret_cast<pl::memory::FuncPtr>(&HeartbeatHook::detour),
        &mOriginalStorage,
        pl::memory::HookPriority::Low);

    if (!mHook.installed() || mOriginalStorage == nullptr) {
        mHook.reset();
        clearActiveRegistration();
        mOriginalStorage = nullptr;
        mDiscovery.reset();
        mMod.getLogger().error(
            "Compatibility gate passed, but the heartbeat detour could not be installed");
        return false;
    }

    mOriginalCallable.store(
        reinterpret_cast<Callback>(mOriginalStorage),
        std::memory_order_release);

    const std::uintptr_t relativeAddress =
        target >= module->loadBase ? target - module->loadBase : 0;
    mMod.getLogger().info(
        "Compatibility profile accepted: mc={}, build_id={}, module_size={}, heartbeat_offset=0x{:x}",
        CompatibilityProfile::minecraftVersion,
        module->buildId.empty() ? "unavailable" : module->buildId,
        module->fileSize,
        relativeAddress);
    mMod.getLogger().info("Heartbeat target prefix: {}", prefix);

    writeStatusSnapshot("installed", 0, 0, 0);
    writeDiscoveryProfile("installed", 0);
    mMod.getLogger().info("Heartbeat status file: {}", mStatusPath);
    mMod.getLogger().info("Tick discovery profile: {}", mDiscoveryPath);

    try {
        mSampler = std::thread([this] {
            sample();
        });
    } catch (const std::system_error& error) {
        mMod.getLogger().error(
            "Could not start heartbeat sampler thread: {}",
            error.what());
        uninstall();
        return false;
    }

    mMod.getLogger().info(
        "Read-only heartbeat hook installed; ARM64 indirect dispatch discovery active; no world or render state is modified");
    return true;
}

void HeartbeatHook::uninstall() noexcept {
    mStopRequested.store(true, std::memory_order_release);
    if (mSampler.joinable()) {
        mSampler.join();
    }

    const bool wasInstalled = mHook.installed();
    mHook.reset();

    using namespace std::chrono_literals;
    constexpr int maxDrainAttempts = 1000;
    int drainAttempt = 0;
    while (mInFlightCallbacks.load(std::memory_order_acquire) != 0 &&
           drainAttempt < maxDrainAttempts) {
        std::this_thread::sleep_for(1ms);
        ++drainAttempt;
    }

    const std::uint32_t remainingCallbacks =
        mInFlightCallbacks.load(std::memory_order_acquire);
    if (remainingCallbacks != 0) {
        mMod.getLogger().warn(
            "Heartbeat hook removal timed out with {} callback(s) still active",
            remainingCallbacks);
    }

    const std::uint64_t finalCount =
        mCallCount.load(std::memory_order_relaxed);
    writeDiscoveryProfile("stopped", finalCount);
    writeStatusSnapshot("stopped", 0, finalCount, 0);

    mOriginalCallable.store(nullptr, std::memory_order_release);
    clearActiveRegistration();
    mOriginalStorage = nullptr;

    if (wasInstalled) {
        mMod.getLogger().info(
            "Heartbeat hook removed; final callback count={}",
            finalCount);
    }
}

void HeartbeatHook::clearActiveRegistration() noexcept {
    HeartbeatHook* expected = this;
    sActive.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

bool HeartbeatHook::detour(void* instance) {
    HeartbeatHook* const active = sActive.load(std::memory_order_acquire);
    if (active == nullptr) {
        return false;
    }

    active->mInFlightCallbacks.fetch_add(1, std::memory_order_acq_rel);
    const Callback original =
        active->mOriginalCallable.load(std::memory_order_acquire);
    if (original == nullptr) {
        active->mInFlightCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }

    const bool result = original(instance);
    const std::uint64_t total =
        active->mCallCount.fetch_add(1, std::memory_order_relaxed) + 1;

    bool expected = false;
    if (active->mFirstCallbackLogged.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        active->mMod.getLogger().info(
            "Heartbeat callback observed for the first time; total_callbacks={}",
            total);
    }

    active->mInFlightCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}

void HeartbeatHook::scanStaticReferences() noexcept {
    if (mDiscovery == nullptr) {
        return;
    }

    auto& discovery = *mDiscovery;
    discovery.scanStarted = true;
    const auto startedAt = std::chrono::steady_clock::now();

    for (const MemoryRegion& region : discovery.regions) {
        if (mStopRequested.load(std::memory_order_acquire)) {
            discovery.scanCancelled = true;
            break;
        }
        if (!region.readable || !region.executable ||
            region.end <= region.start ||
            region.end - region.start < sizeof(std::uint32_t)) {
            continue;
        }

        const std::uintptr_t first =
            alignUp(region.start, arm64InstructionAlignment);
        for (std::uintptr_t address = first;
             address <= region.end - sizeof(std::uint32_t);
             address += sizeof(std::uint32_t)) {
            if ((discovery.executableBytesScanned % cancellationCheckStride) == 0U &&
                mStopRequested.load(std::memory_order_acquire)) {
                discovery.scanCancelled = true;
                break;
            }

            std::uint32_t instruction = 0;
            std::memcpy(
                &instruction,
                reinterpret_cast<const void*>(address),
                sizeof(instruction));
            discovery.executableBytesScanned += sizeof(instruction);

            const std::uint32_t opcode =
                instruction & arm64BranchOpcodeMask;
            if (opcode != arm64BranchOpcode &&
                opcode != arm64BranchLinkOpcode) {
                continue;
            }
            if (decodeArm64BranchDestination(address, instruction) !=
                discovery.targetAddress) {
                continue;
            }

            ++discovery.directBranchReferenceCount;
            if (discovery.branchReferences.size() < maximumRecordedReferences) {
                discovery.branchReferences.push_back({
                    .address = address,
                    .link = opcode == arm64BranchLinkOpcode,
                });
            }
        }
        if (discovery.scanCancelled) {
            break;
        }
    }

    if (!discovery.scanCancelled) {
        for (const MemoryRegion& region : discovery.regions) {
            if (mStopRequested.load(std::memory_order_acquire)) {
                discovery.scanCancelled = true;
                break;
            }
            if (!region.readable || region.executable ||
                region.end <= region.start ||
                region.end - region.start < sizeof(std::uintptr_t)) {
                continue;
            }

            const std::uintptr_t first = alignUp(region.start, pointerAlignment);
            for (std::uintptr_t address = first;
                 address <= region.end - sizeof(std::uintptr_t);
                 address += sizeof(std::uintptr_t)) {
                if ((discovery.readableDataBytesScanned % cancellationCheckStride) == 0U &&
                    mStopRequested.load(std::memory_order_acquire)) {
                    discovery.scanCancelled = true;
                    break;
                }

                const std::uintptr_t value = readPointer(address);
                discovery.readableDataBytesScanned += sizeof(value);

                if (value != discovery.targetAddress) {
                    continue;
                }

                ++discovery.pointerReferenceCount;
                if (discovery.pointerReferences.size() < maximumRecordedReferences) {
                    discovery.pointerReferences.push_back(address);
                }
            }
            if (discovery.scanCancelled) {
                break;
            }
        }
    }

    if (!discovery.scanCancelled) {
        const std::size_t candidateCount = std::min(
            discovery.pointerReferences.size(),
            maximumDispatchCandidates);
        for (std::size_t candidateIndex = 0;
             candidateIndex < candidateCount;
             ++candidateIndex) {
            const std::uintptr_t pointerAddress =
                discovery.pointerReferences[candidateIndex];
            const MemoryRegion* const containingRegion =
                findContainingRegion(discovery.regions, pointerAddress);
            if (containingRegion == nullptr || !containingRegion->readable ||
                containingRegion->executable) {
                continue;
            }

            std::uintptr_t runStart = pointerAddress;
            std::size_t walked = 0;
            while (runStart >= containingRegion->start + sizeof(std::uintptr_t) &&
                   walked < maximumTableWalkEntries) {
                const std::uintptr_t previous = runStart - sizeof(std::uintptr_t);
                if (!isExecutableModuleAddress(
                        discovery.regions,
                        readPointer(previous))) {
                    break;
                }
                runStart = previous;
                ++walked;
            }

            std::uintptr_t runEnd = pointerAddress + sizeof(std::uintptr_t);
            walked = 0;
            while (runEnd <= containingRegion->end - sizeof(std::uintptr_t) &&
                   walked < maximumTableWalkEntries) {
                if (!isExecutableModuleAddress(
                        discovery.regions,
                        readPointer(runEnd))) {
                    break;
                }
                runEnd += sizeof(std::uintptr_t);
                ++walked;
            }

            DiscoveryState::DispatchTableCandidate candidate;
            candidate.pointerAddress = pointerAddress;
            candidate.runStart = runStart;
            candidate.runEnd = runEnd;
            candidate.entryCount = static_cast<std::uint32_t>(
                (runEnd - runStart) / sizeof(std::uintptr_t));
            candidate.slotIndex = static_cast<std::uint32_t>(
                (pointerAddress - runStart) / sizeof(std::uintptr_t));
            candidate.slotOffsetBytes = static_cast<std::uint32_t>(
                pointerAddress - runStart);
            candidate.vtableLike = candidate.entryCount >= 3U;
            candidate.neighbours.reserve(maximumNeighbourEntries);

            constexpr std::int32_t neighbourRadius = 8;
            for (std::int32_t relativeIndex = -neighbourRadius;
                 relativeIndex <= neighbourRadius;
                 ++relativeIndex) {
                const std::int64_t signedAddress =
                    static_cast<std::int64_t>(pointerAddress) +
                    static_cast<std::int64_t>(relativeIndex) *
                        static_cast<std::int64_t>(sizeof(std::uintptr_t));
                if (signedAddress < 0) {
                    continue;
                }
                const std::uintptr_t entryAddress =
                    static_cast<std::uintptr_t>(signedAddress);
                if (entryAddress < containingRegion->start ||
                    entryAddress > containingRegion->end - sizeof(std::uintptr_t)) {
                    continue;
                }

                const std::uintptr_t value = readPointer(entryAddress);
                candidate.neighbours.push_back({
                    .relativeIndex = relativeIndex,
                    .entryAddress = entryAddress,
                    .value = value,
                    .kind = classifyPointerValue(discovery.regions, value),
                });
            }

            discovery.dispatchTables.push_back(std::move(candidate));
        }
    }

    if (!discovery.scanCancelled) {
        for (const auto& candidate : discovery.dispatchTables) {
            if (!candidate.vtableLike) {
                continue;
            }

            for (const MemoryRegion& region : discovery.regions) {
                if (mStopRequested.load(std::memory_order_acquire)) {
                    discovery.scanCancelled = true;
                    break;
                }
                if (!region.readable || !region.executable ||
                    region.end <= region.start ||
                    region.end - region.start < sizeof(std::uint32_t)) {
                    continue;
                }

                const std::uintptr_t first =
                    alignUp(region.start, arm64InstructionAlignment);
                for (std::uintptr_t address = first;
                     address <= region.end - sizeof(std::uint32_t);
                     address += sizeof(std::uint32_t)) {
                    if ((discovery.indirectExecutableBytesScanned %
                         cancellationCheckStride) == 0U &&
                        mStopRequested.load(std::memory_order_acquire)) {
                        discovery.scanCancelled = true;
                        break;
                    }

                    std::uint32_t instruction = 0;
                    std::memcpy(
                        &instruction,
                        reinterpret_cast<const void*>(address),
                        sizeof(instruction));
                    discovery.indirectExecutableBytesScanned +=
                        sizeof(instruction);

                    std::uint8_t functionRegister = 0;
                    std::uint8_t vtableRegister = 0;
                    std::uint32_t loadOffset = 0;
                    if (!decodeArm64LdrUnsigned64(
                            instruction,
                            functionRegister,
                            vtableRegister,
                            loadOffset) ||
                        loadOffset != candidate.slotOffsetBytes) {
                        continue;
                    }

                    bool branchFound = false;
                    bool branchLink = false;
                    std::uint8_t branchDistance = 0;
                    for (std::size_t distance = 1;
                         distance <= indirectSearchInstructionWindow;
                         ++distance) {
                        const std::uintptr_t branchAddress =
                            address + distance * sizeof(std::uint32_t);
                        if (branchAddress >
                            region.end - sizeof(std::uint32_t)) {
                            break;
                        }

                        std::uint32_t branchInstruction = 0;
                        std::memcpy(
                            &branchInstruction,
                            reinterpret_cast<const void*>(branchAddress),
                            sizeof(branchInstruction));
                        std::uint8_t branchRegister = 0;
                        bool link = false;
                        if (decodeArm64RegisterBranch(
                                branchInstruction,
                                branchRegister,
                                link) &&
                            branchRegister == functionRegister) {
                            branchFound = true;
                            branchLink = link;
                            branchDistance =
                                static_cast<std::uint8_t>(distance);
                            break;
                        }
                    }
                    if (!branchFound) {
                        continue;
                    }

                    bool vtableLoadFound = false;
                    std::uint8_t objectRegister = 0;
                    std::uint8_t vtableLoadDistance = 0;
                    for (std::size_t distance = 1;
                         distance <= indirectSearchInstructionWindow;
                         ++distance) {
                        const std::uintptr_t byteDistance =
                            distance * sizeof(std::uint32_t);
                        if (address < region.start + byteDistance) {
                            break;
                        }
                        const std::uintptr_t loadAddress =
                            address - byteDistance;

                        std::uint32_t loadInstruction = 0;
                        std::memcpy(
                            &loadInstruction,
                            reinterpret_cast<const void*>(loadAddress),
                            sizeof(loadInstruction));
                        std::uint8_t loadedRegister = 0;
                        std::uint8_t baseRegister = 0;
                        std::uint32_t objectOffset = 0;
                        if (decodeArm64LdrUnsigned64(
                                loadInstruction,
                                loadedRegister,
                                baseRegister,
                                objectOffset) &&
                            loadedRegister == vtableRegister &&
                            objectOffset == 0U) {
                            vtableLoadFound = true;
                            objectRegister = baseRegister;
                            vtableLoadDistance =
                                static_cast<std::uint8_t>(distance);
                            break;
                        }
                    }
                    if (!vtableLoadFound) {
                        continue;
                    }

                    ++discovery.indirectCallReferenceCount;
                    if (discovery.indirectCalls.size() <
                        maximumRecordedReferences) {
                        const bool duplicate = std::any_of(
                            discovery.indirectCalls.begin(),
                            discovery.indirectCalls.end(),
                            [address, &candidate](const auto& existing) {
                                return existing.address == address &&
                                       existing.slotOffsetBytes ==
                                           candidate.slotOffsetBytes;
                            });
                        if (!duplicate) {
                            discovery.indirectCalls.push_back({
                                .address = address,
                                .slotOffsetBytes = candidate.slotOffsetBytes,
                                .objectRegister = objectRegister,
                                .vtableRegister = vtableRegister,
                                .functionRegister = functionRegister,
                                .vtableLoadDistance = vtableLoadDistance,
                                .branchDistance = branchDistance,
                                .link = branchLink,
                            });
                        }
                    }
                }
                if (discovery.scanCancelled) {
                    break;
                }
            }
            if (discovery.scanCancelled) {
                break;
            }
        }
    }

    discovery.scanComplete = !discovery.scanCancelled;
    discovery.scanDurationMilliseconds =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt)
                .count());

    mMod.getLogger().info(
        "ARM64 heartbeat indirect dispatch scan {}: direct_branches={}, pointer_references={}, dispatch_tables={}, indirect_calls={}, duration_ms={}",
        discovery.scanComplete ? "completed" : "cancelled",
        discovery.directBranchReferenceCount,
        discovery.pointerReferenceCount,
        discovery.dispatchTables.size(),
        discovery.indirectCallReferenceCount,
        discovery.scanDurationMilliseconds);
}

void HeartbeatHook::writeDiscoveryProfile(
    std::string_view state,
    std::uint64_t totalCallbacks) noexcept {
    if (mDiscovery == nullptr || mDiscoveryPath.empty()) {
        return;
    }

    std::ofstream output(mDiscoveryPath, std::ios::trunc);
    if (!output) {
        return;
    }

    const auto& discovery = *mDiscovery;
    const std::uintptr_t targetOffset =
        discovery.targetAddress >= discovery.moduleLoadBase
            ? discovery.targetAddress - discovery.moduleLoadBase
            : 0;

    output << "schema=3\n";
    output << "state=" << state << '\n';
    output << "minecraft_version=" << CompatibilityProfile::minecraftVersion << '\n';
    output << "module_build_id="
           << (discovery.moduleBuildId.empty() ? "unavailable" : discovery.moduleBuildId)
           << '\n';
    output << "module_file_size=" << discovery.moduleFileSize << '\n';
    output << "discovery_method=arm64_indirect_dispatch_scan\n";
    output << "previous_result=single_pointer_reference_no_direct_branch\n";
    output << "heartbeat_target_offset=0x"
           << std::hex << targetOffset << std::dec << '\n';
    output << "total_callbacks=" << totalCallbacks << '\n';
    output << "scan_state="
           << (discovery.scanComplete
                   ? "complete"
                   : discovery.scanCancelled
                         ? "cancelled"
                         : discovery.scanStarted ? "running" : "not_started")
           << '\n';
    output << "scan_duration_ms=" << discovery.scanDurationMilliseconds << '\n';
    output << "executable_bytes_scanned="
           << discovery.executableBytesScanned << '\n';
    output << "readable_data_bytes_scanned="
           << discovery.readableDataBytesScanned << '\n';
    output << "indirect_executable_bytes_scanned="
           << discovery.indirectExecutableBytesScanned << '\n';
    output << "direct_branch_reference_count="
           << discovery.directBranchReferenceCount << '\n';
    output << "direct_branch_reference_recorded="
           << discovery.branchReferences.size() << '\n';
    output << "pointer_reference_count="
           << discovery.pointerReferenceCount << '\n';
    output << "pointer_reference_recorded="
           << discovery.pointerReferences.size() << '\n';
    output << "dispatch_table_candidate_count="
           << discovery.dispatchTables.size() << '\n';
    output << "indirect_call_reference_count="
           << discovery.indirectCallReferenceCount << '\n';
    output << "indirect_call_reference_recorded="
           << discovery.indirectCalls.size() << '\n';

    for (std::size_t index = 0;
         index < discovery.branchReferences.size();
         ++index) {
        const auto& reference = discovery.branchReferences[index];
        const std::uintptr_t offset =
            reference.address >= discovery.moduleLoadBase
                ? reference.address - discovery.moduleLoadBase
                : 0;
        output << "direct_branch." << index << ".offset=0x"
               << std::hex << offset << std::dec << '\n';
        output << "direct_branch." << index << ".kind="
               << (reference.link ? "bl" : "b") << '\n';
    }

    for (std::size_t index = 0;
         index < discovery.pointerReferences.size();
         ++index) {
        const std::uintptr_t address = discovery.pointerReferences[index];
        const std::uintptr_t offset =
            address >= discovery.moduleLoadBase
                ? address - discovery.moduleLoadBase
                : 0;
        output << "pointer_reference." << index << ".offset=0x"
               << std::hex << offset << std::dec << '\n';
    }

    for (std::size_t index = 0;
         index < discovery.dispatchTables.size();
         ++index) {
        const auto& candidate = discovery.dispatchTables[index];
        output << "dispatch_table." << index << ".pointer_offset=0x"
               << std::hex
               << (candidate.pointerAddress - discovery.moduleLoadBase)
               << std::dec << '\n';
        output << "dispatch_table." << index << ".run_start_offset=0x"
               << std::hex
               << (candidate.runStart - discovery.moduleLoadBase)
               << std::dec << '\n';
        output << "dispatch_table." << index << ".run_end_offset=0x"
               << std::hex
               << (candidate.runEnd - discovery.moduleLoadBase)
               << std::dec << '\n';
        output << "dispatch_table." << index << ".entry_count="
               << candidate.entryCount << '\n';
        output << "dispatch_table." << index << ".slot_index="
               << candidate.slotIndex << '\n';
        output << "dispatch_table." << index << ".slot_offset_bytes="
               << candidate.slotOffsetBytes << '\n';
        output << "dispatch_table." << index << ".vtable_like="
               << (candidate.vtableLike ? "true" : "false") << '\n';
        output << "dispatch_table." << index << ".neighbour_count="
               << candidate.neighbours.size() << '\n';

        for (std::size_t neighbourIndex = 0;
             neighbourIndex < candidate.neighbours.size();
             ++neighbourIndex) {
            const auto& neighbour = candidate.neighbours[neighbourIndex];
            output << "dispatch_table." << index << ".neighbour."
                   << neighbourIndex << ".relative_index="
                   << neighbour.relativeIndex << '\n';
            output << "dispatch_table." << index << ".neighbour."
                   << neighbourIndex << ".kind="
                   << pointerValueKindName(neighbour.kind) << '\n';
            if (findContainingRegion(discovery.regions, neighbour.value) != nullptr) {
                output << "dispatch_table." << index << ".neighbour."
                       << neighbourIndex << ".value_offset=0x"
                       << std::hex
                       << (neighbour.value - discovery.moduleLoadBase)
                       << std::dec << '\n';
            } else if (neighbour.kind == PointerValueKind::SmallScalar) {
                output << "dispatch_table." << index << ".neighbour."
                       << neighbourIndex << ".scalar=0x"
                       << std::hex << neighbour.value << std::dec << '\n';
            }
        }
    }

    for (std::size_t index = 0;
         index < discovery.indirectCalls.size();
         ++index) {
        const auto& reference = discovery.indirectCalls[index];
        output << "indirect_call." << index << ".offset=0x"
               << std::hex
               << (reference.address - discovery.moduleLoadBase)
               << std::dec << '\n';
        output << "indirect_call." << index << ".slot_offset_bytes="
               << reference.slotOffsetBytes << '\n';
        output << "indirect_call." << index << ".object_register=x"
               << static_cast<unsigned int>(reference.objectRegister) << '\n';
        output << "indirect_call." << index << ".vtable_register=x"
               << static_cast<unsigned int>(reference.vtableRegister) << '\n';
        output << "indirect_call." << index << ".function_register=x"
               << static_cast<unsigned int>(reference.functionRegister) << '\n';
        output << "indirect_call." << index << ".vtable_load_distance="
               << static_cast<unsigned int>(reference.vtableLoadDistance) << '\n';
        output << "indirect_call." << index << ".branch_distance="
               << static_cast<unsigned int>(reference.branchDistance) << '\n';
        output << "indirect_call." << index << ".branch_kind="
               << (reference.link ? "blr" : "br") << '\n';
    }
}

void HeartbeatHook::sample() {
    using namespace std::chrono_literals;

    std::uint64_t sequence = 1;
    std::uint64_t previous = 0;
    writeStatusSnapshot("sampler_started", sequence, 0, 0);
    writeDiscoveryProfile("scanning", 0);
    scanStaticReferences();
    writeDiscoveryProfile(
        mDiscovery != nullptr && mDiscovery->scanComplete
            ? "scan_complete"
            : "scan_cancelled",
        mCallCount.load(std::memory_order_relaxed));

    while (!mStopRequested.load(std::memory_order_acquire)) {
        for (int slice = 0;
             slice < 20 && !mStopRequested.load(std::memory_order_acquire);
             ++slice) {
            std::this_thread::sleep_for(100ms);
        }
        if (mStopRequested.load(std::memory_order_acquire)) {
            break;
        }

        const std::uint64_t total = mCallCount.load(std::memory_order_relaxed);
        const std::uint64_t delta = total - previous;
        previous = total;
        ++sequence;
        writeStatusSnapshot("sampling", sequence, total, delta);
        writeDiscoveryProfile("sampling", total);
    }
}

void HeartbeatHook::writeStatusSnapshot(
    std::string_view state,
    std::uint64_t sequence,
    std::uint64_t totalCallbacks,
    std::uint64_t callbackDelta) noexcept {
    if (mStatusPath.empty()) {
        return;
    }

    std::ofstream output(mStatusPath, std::ios::trunc);
    if (!output) {
        return;
    }

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    output << "schema=1\n";
    output << "state=" << state << '\n';
    output << "sampler_sequence=" << sequence << '\n';
    output << "timestamp_unix_ms=" << now << '\n';
    output << "total_callbacks=" << totalCallbacks << '\n';
    output << "callbacks_since_previous=" << callbackDelta << '\n';
}

}  // namespace aeronautics::bedrock
