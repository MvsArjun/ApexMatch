#include "test_support.hpp"
#include <exception>
#include <iostream>

void run_core_tests();
void run_extension_tests();
int main() {
    // Preserve the last completed case when a platform runtime terminates abruptly.
    std::cout << std::unitbuf;
    try {
        run_core_tests();
        run_extension_tests();
        std::cout << "All tests passed (" << apexmatch::test::passed_cases << " named cases).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
