#include "android/BedrockAeronauticsMod.hpp"

#include "aeronautics/module.hpp"
#include "bedrock/ClientLevelTickEvent.hpp"
#include "bedrock/ClientLevelTickHook.hpp"
#include "bedrock/HeartbeatHook.hpp"
#include "physics/PhysicsScheduler.hpp"

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
      mTickBus(std::make_unique<aeronautics::bedrock::ClientLevelTickBus>()),
      mPhysicsScheduler(std::make_unique<aeronautics::physics::PhysicsScheduler>(
          mSelf,
          *mTickBus)),
      mClientLevelTickHook(
          std::make_unique<aeronautics::bedrock::ClientLevelTickHook>(
              mSelf,
              *mHeartbeat,
              *mTickBus)) {}

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
    if (!mPhysicsScheduler->start()) {
        mSelf.getLogger().warn(
            "Bedrock Aeronautics enabled without physics scheduler telemetry");
    }

    if (!mHeartbeat->install()) {
        mPhysicsScheduler->stop();
        mSelf.getLogger().warn(
            "Bedrock Aeronautics enabled in compatibility-safe mode; heartbeat hook inactive");
        return true;
    }

    if (!mClientLevelTickHook->install()) {
        mPhysicsScheduler->stop();
        mSelf.getLogger().warn(
            "ClientLevel tick event source is inactive; physics scheduler stopped");
        return true;
    }

    mSelf.getLogger().info(
        "Bedrock Aeronautics enabled; ClientLevelTickEvent bus and fixed-step physics scheduler initialized");
    return true;
}

bool BedrockAeronauticsMod::disable() {
    mClientLevelTickHook->uninstall();
    if (!mClientLevelTickHook->safeToUnload()) {
        mSelf.getLogger().error(
            "Bedrock Aeronautics cannot be disabled safely because the original ClientLevel vtable pointer was not restored");
        return false;
    }
    mHeartbeat->uninstall();
    mPhysicsScheduler->stop();
    mSelf.getLogger().info("Bedrock Aeronautics disabled");
    return true;
}

bool BedrockAeronauticsMod::unload() {
    mClientLevelTickHook->uninstall();
    if (!mClientLevelTickHook->safeToUnload()) {
        mSelf.getLogger().error(
            "Bedrock Aeronautics unload refused because ClientLevel still references the module trampoline");
        return false;
    }
    mHeartbeat->uninstall();
    mPhysicsScheduler->stop();
    mSelf.getLogger().info("Bedrock Aeronautics unloaded");
    return true;
}

}  // namespace aeronautics::android
