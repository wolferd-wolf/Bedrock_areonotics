#include "aeronautics/module.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

int main() {
    const char* version = bedrock_aeronautics_version();
    if (version == nullptr) {
        std::cerr << "Version export returned null\n";
        return EXIT_FAILURE;
    }

    const std::string_view value{version};
    if (value.empty()) {
        std::cerr << "Version export returned an empty string\n";
        return EXIT_FAILURE;
    }

    std::cout << "Module metadata contract valid: " << value << '\n';
    return EXIT_SUCCESS;
}
