#include "common.hpp"

namespace easy_uds::test {

void test_error_model() {
    using namespace easy_uds;

    const std::error_code timeout_system(ETIMEDOUT, std::generic_category());
    const Error timeout(ErrorCode::timeout, "receive timed out", timeout_system);
    expect(timeout.kind() == ErrorCode::timeout,
           "Error::kind should expose the stable semantic class");
    expect(timeout.code() == ErrorCode::timeout,
           "std::system_error::code should use the easy-uds category");
    expect(timeout.system_code() == timeout_system,
           "Error should preserve the original errno error_code");
    expect(std::string_view(timeout.what()).find("receive timed out") !=
               std::string_view::npos,
           "Error::what should retain operation context");
    expect(std::string_view(timeout.what()).find(timeout_system.message()) !=
               std::string_view::npos,
           "Error::what should include the operating-system detail");

    bool caught_as_system_error = false;
    try {
        throw timeout;
    } catch (const std::system_error&) {
        caught_as_system_error = true;
    }
    expect(caught_as_system_error,
           "Error should remain catch-compatible with std::system_error");

    ClientOptions options;
    options.connect_timeout = 100ms;
    options.request_timeout = 200ms;
    const std::string missing = socket_path("error-unavailable");
    cleanup_socket_artifacts(missing);
    try {
        (void)Client(missing, options).request("ping");
        throw std::runtime_error("test failed: missing server should fail");
    } catch (const Error& error) {
        expect(error.kind() == ErrorCode::unavailable,
               "missing server should report unavailable");
        expect(error.system_code() ==
                   std::error_code(ENOENT, std::generic_category()) ||
                   error.system_code() ==
                       std::error_code(ECONNREFUSED, std::generic_category()),
               "missing server should preserve connect errno");
    }
}

} // namespace easy_uds::test
