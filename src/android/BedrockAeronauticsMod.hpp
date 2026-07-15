#pragma once

#include <pl/Mod.hpp>

namespace aeronautics::android {

class BedrockAeronauticsMod final {
public:
    static BedrockAeronauticsMod& instance();

    BedrockAeronauticsMod();

    [[nodiscard]] ll::mod::NativeMod& self() const noexcept { return mSelf; }

    bool load();
    bool enable();
    bool disable();
    bool unload();

private:
    ll::mod::NativeMod& mSelf;
};

}  // namespace aeronautics::android
