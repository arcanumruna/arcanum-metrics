#pragma once
// A per-test temporary directory removed with its contents on destruction.
#include <dirent.h>
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
        if (DIR* d = ::opendir(dir_.c_str())) {
            while (const dirent* e = ::readdir(d)) {
                if (e->d_name[0] != '.') ::unlink((dir_ + "/" + e->d_name).c_str());
            }
            ::closedir(d);
        }
        ::rmdir(dir_.c_str());
    }
    TestDir(const TestDir&) = delete;
    TestDir& operator=(const TestDir&) = delete;
    [[nodiscard]] std::string path(const char* name) const { return dir_ + "/" + name; }

private:
    std::string dir_;
};
}  // namespace metrics::testing
