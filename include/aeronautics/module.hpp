#pragma once

#if defined(_WIN32)
#define AERO_EXPORT extern "C" __declspec(dllexport)
#else
#define AERO_EXPORT extern "C" __attribute__((visibility("default")))
#endif

AERO_EXPORT void mod_init();
AERO_EXPORT const char* bedrock_aeronautics_version();
