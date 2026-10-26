#pragma once
// A per-test temporary directory removed with its contents on destruction.
#include <filesystem>
#include <system_error>
#include <string>
#include <unistd.h>
#include <cstdlib>

namespace metrics::testing {
class TestDir {
public:
    TestDir() {
        char tmpl[] = "/tmp/metrics_test_XXXXXX";
        dir_ = ::mkdtemp(tmpl) ? tmpl : "";
    }
    ~TestDir() {
        if (dir_.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    TestDir(const TestDir&) = delete;
    TestDir& operator=(const TestDir&) = delete;
    [[nodiscard]] std::string path(const char* name) const { return dir_ + "/" + name; }

private:
    std::string dir_;
};
}  // namespace metrics::testing
