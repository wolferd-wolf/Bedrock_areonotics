#include "aeronautics/module.hpp"

#ifndef BEDROCK_AERONAUTICS_VERSION
#define BEDROCK_AERONAUTICS_VERSION "0.0.1-dev"
#endif

const char* bedrock_aeronautics_version() {
    return BEDROCK_AERONAUTICS_VERSION;
}
