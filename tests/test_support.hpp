#pragma once
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace apexmatch::test {
inline std::size_t passed_cases = 0;
inline void require(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                                 ": requirement failed: " + expression);
    }
}
inline void run(const char* name, const std::function<void()>& body) {
    try { body(); }
    catch (const std::exception& error) {
        throw std::runtime_error(std::string(name) + ": " + error.what());
    }
    ++passed_cases;
    std::cout << "[PASS] " << name << '\n';
}
} // namespace apexmatch::test
#define REQUIRE(...) ::apexmatch::test::require(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __FILE__, __LINE__)
