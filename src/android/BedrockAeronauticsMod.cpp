#include "android/BedrockAeronauticsMod.hpp"

#include "aeronautics/module.hpp"
#include "bedrock/ClientLevelTickEvent.hpp"
#include "bedrock/ClientLevelTickHook.hpp"
#include "bedrock/HeartbeatHook.hpp"
#include "bedrock/LevelRenderEvent.hpp"
#include "bedrock/LevelRenderHook.hpp"
#include "physics/AnchoredFlightProxy.hpp"
#include "physics/PhysicsScheduler.hpp"
#include "render/DiagnosticCubeRenderProbe.hpp"

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
      mRenderBus(std::make_unique<aeronautics::bedrock::LevelRenderBus>()),
      mPhysicsScheduler(std::make_unique<aeronautics::physics::PhysicsScheduler>(
          mSelf,
          *mTickBus)),
      mRenderProbe(std::make_unique<aeronautics::render::DiagnosticCubeRenderProbe>(
          mSelf,
          *mRenderBus,
          *mPhysicsScheduler)),
      mClientLevelTickHook(
          std::make_unique<aeronautics::bedrock::ClientLevelTickHook>(
              mSelf,
              *mHeartbeat,
              *mTickBus)),
      mLevelRenderHook(std::make_unique<aeronautics::bedrock::LevelRenderHook>(
          mSelf,
          *mHeartbeat,
          *mRenderBus,
          *mPhysicsScheduler)) {}

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
    mSelf.getLogger().info(
        "Native motion core ready; {}",
        aeronautics::physics::AnchoredFlightProxy::architectureMarker().data());

    if (!mPhysicsScheduler->start()) {
        mSelf.getLogger().warn(
            "Bedrock Aeronautics enabled without physics scheduler telemetry");
    }

    if (!mRenderProbe->start()) {
        mSelf.getLogger().warn(
            "Render interpolation telemetry probe is inactive");
    }

    if (!mHeartbeat->install()) {
        mRenderProbe->stop();
        mPhysicsScheduler->stop();
        mSelf.getLogger().warn(
            "Bedrock Aeronautics enabled in compatibility-safe mode; heartbeat hook inactive");
        return true;
    }

    if (!mClientLevelTickHook->install()) {
        mRenderProbe->stop();
        mPhysicsScheduler->stop();
        mSelf.getLogger().warn(
            "ClientLevel tick event source is inactive; physics and render telemetry stopped");
        return true;
    }

    if (!mLevelRenderHook->install()) {
        mRenderProbe->stop();
        mSelf.getLogger().warn(
            "Terrain command census hooks are inactive; physics remains enabled");
    }

    mSelf.getLogger().info(
        "Bedrock Aeronautics enabled; tick physics and read-only terrain command descriptor census initialized");
    return true;
}

bool BedrockAeronauticsMod::disable() {
    mLevelRenderHook->uninstall();
    if (!mLevelRenderHook->safeToUnload()) {
        mSelf.getLogger().error(
            "Bedrock Aeronautics cannot be disabled safely because a render hook remains installed");
        return false;
    }
    mRenderProbe->stop();

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
    mLevelRenderHook->uninstall();
    if (!mLevelRenderHook->safeToUnload()) {
        mSelf.getLogger().error(
            "Bedrock Aeronautics unload refused because a render hook still references the module");
        return false;
    }
    mRenderProbe->stop();

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
