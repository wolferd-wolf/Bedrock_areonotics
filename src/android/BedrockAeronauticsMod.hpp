#pragma once

#include <memory>

#include <pl/Mod.hpp>

namespace aeronautics::bedrock {
class HeartbeatHook;
class VtableNeighbourProbe;
}

namespace aeronautics::android {

class BedrockAeronauticsMod final {
public:
    static BedrockAeronauticsMod& instance();

    BedrockAeronauticsMod();
    ~BedrockAeronauticsMod();

    [[nodiscard]] ll::mod::NativeMod& self() const noexcept { return mSelf; }

    bool load();
    bool enable();
    bool disable();
    bool unload();

private:
    ll::mod::NativeMod& mSelf;
    std::unique_ptr<aeronautics::bedrock::HeartbeatHook> mHeartbeat;
    std::unique_ptr<aeronautics::bedrock::VtableNeighbourProbe> mVtableProbe;
};

}  // namespace aeronautics::android
