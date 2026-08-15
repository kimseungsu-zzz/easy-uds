#include "common.hpp"

int main() {
    try {
        easy_uds::test::test_release_candidate_adversarial();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Adversarial audit passed.\n";
    return 0;
}
