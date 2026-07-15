#include "aeronautics/module.hpp"

#if defined(__ANDROID__)
#include <android/log.h>
#else
#include <cstdio>
#endif

#ifndef BEDROCK_AERONAUTICS_VERSION
#define BEDROCK_AERONAUTICS_VERSION "0.0.1-dev"
#endif

namespace {
void log_info(const char* message) {
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "BedrockAeronautics", "%s", message);
#else
    std::fprintf(stderr, "[BedrockAeronautics] %s\n", message);
#endif
}
}  // namespace

void mod_init() {
    log_info("Bedrock Aeronautics native module loaded");
    log_info("Target: Minecraft Bedrock Android 1.21.0.03 ARM64");
}

const char* bedrock_aeronautics_version() {
    return BEDROCK_AERONAUTICS_VERSION;
}
