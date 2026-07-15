#include "android/BedrockAeronauticsMod.hpp"

#include "aeronautics/module.hpp"
#include "bedrock/HeartbeatHook.hpp"
#include "bedrock/VtableSlotProbe.hpp"

#include <filesystem>
#include <memory>
#include <system_error>

namespace aeronautics::android {

BedrockAeronauticsMod& BedrockAeronauticsMod::instance() {
    static BedrockAeronauticsMod instance;
    return instance;
}

BedrockAeronauticsMod::BedrockAeronauticsMod()
    : mSelf(*ll::mod::NativeMod::current()),
      mHeartbeat(std::make_unique<aeronautics::bedrock::HeartbeatHook>(mSelf)),
      mVtableProbe(
          std::make_unique<aeronautics::bedrock::VtableSlotProbe>(mSelf)) {}

BedrockAeronauticsMod::~BedrockAeronauticsMod() = default;

bool BedrockAeronauticsMod::load() {
    std::error_code error;
    std::filesystem::create_directories(mSelf.getDataDir(), error);
    if (error) {
        mSelf.getLogger().error(
            "Failed to create data directory {}: {}",
            mSelf.getDataDir(),
            error.message());
        return false;
    }

    mSelf.getLogger().info(
        "Bedrock Aeronautics native module loaded; version={}",
        bedrock_aeronautics_version());
    mSelf.getLogger().info(
        "Target: Minecraft Bedrock Android 1.26.33.1 ARM64");
    mSelf.getLogger().info("Module directory: {}", mSelf.getModDir());
    return true;
}

bool BedrockAeronauticsMod::enable() {
    if (!mHeartbeat->install()) {
        mSelf.getLogger().warn(
            "Bedrock Aeronautics enabled in compatibility-safe mode; heartbeat hook inactive");
        return true;
    }

    if (!mVtableProbe->install()) {
        mSelf.getLogger().warn(
            "Milestone 2C single-slot pointer probe is inactive; the proven heartbeat remains enabled");
    }

    mSelf.getLogger().info(
        "Bedrock Aeronautics enabled; proven heartbeat and delayed single-slot validation initialized");
    return true;
}

bool BedrockAeronauticsMod::disable() {
    mVtableProbe->uninstall();
    if (!mVtableProbe->safeToUnload()) {
        mSelf.getLogger().error(
            "Bedrock Aeronautics cannot be disabled safely because the original vtable pointer was not restored");
        return false;
    }
    mHeartbeat->uninstall();
    mSelf.getLogger().info("Bedrock Aeronautics disabled");
    return true;
}

bool BedrockAeronauticsMod::unload() {
    mVtableProbe->uninstall();
    if (!mVtableProbe->safeToUnload()) {
        mSelf.getLogger().error(
            "Bedrock Aeronautics unload refused because Minecraft still references the module trampoline");
        return false;
    }
    mHeartbeat->uninstall();
    mSelf.getLogger().info("Bedrock Aeronautics unloaded");
    return true;
}

}  // namespace aeronautics::android
