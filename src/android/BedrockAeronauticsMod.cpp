#include "android/BedrockAeronauticsMod.hpp"

#include "aeronautics/module.hpp"

#include <filesystem>
#include <system_error>

namespace aeronautics::android {

BedrockAeronauticsMod& BedrockAeronauticsMod::instance() {
    static BedrockAeronauticsMod instance;
    return instance;
}

BedrockAeronauticsMod::BedrockAeronauticsMod()
    : mSelf(*ll::mod::NativeMod::current()) {}

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
    mSelf.getLogger().info("Bedrock Aeronautics enabled");
    return true;
}

bool BedrockAeronauticsMod::disable() {
    mSelf.getLogger().info("Bedrock Aeronautics disabled");
    return true;
}

bool BedrockAeronauticsMod::unload() {
    mSelf.getLogger().info("Bedrock Aeronautics unloaded");
    return true;
}

}  // namespace aeronautics::android
