#include "AllocCounter.h"

#include "metrics/MetricsFile.h"

#include <gtest/gtest.h>

#include <atomic>
#include <climits>
#include <dirent.h>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace metrics;

namespace {

class MetricsFileRegression : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/metrics_file_test_XXXXXX";
        ASSERT_NE(::mkdtemp(tmpl), nullptr);
        dir_ = tmpl;
        path_ = dir_ + "/m.dat";
    }
    void TearDown() override {
        if (DIR* d = ::opendir(dir_.c_str())) {
            while (const dirent* e = ::readdir(d)) {
                if (e->d_name[0] != '.') ::unlink((dir_ + "/" + e->d_name).c_str());
            }
            ::closedir(d);
        }
        ::rmdir(dir_.c_str());
    }
    CreateParams params(int32_t maxSlots, int64_t startEpochNs = 1000) const {
        return CreateParams{path_.c_str(), maxSlots, maxSlots, 4242, startEpochNs, {}};
    }
    int countEntries() const {
        int n = 0;
        if (DIR* d = ::opendir(dir_.c_str())) {
            while (const dirent* e = ::readdir(d)) {
                if (e->d_name[0] != '.') ++n;
            }
            ::closedir(d);
        }
        return n;
    }
    void writeRaw(const std::string& path, int32_t version, int32_t maxSlots, int64_t length) const {
        int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0640);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(::ftruncate(fd, length), 0);
        const int32_t metaLen = kMetadataRecordLength, valLen = kValueRecordLength;
        ASSERT_EQ(::pwrite(fd, &maxSlots, 4, kHdrMaxSlotsOffset), 4);
        ASSERT_EQ(::pwrite(fd, &metaLen, 4, kHdrMetadataRecordLengthOffset), 4);
        ASSERT_EQ(::pwrite(fd, &valLen, 4, kHdrValueRecordLengthOffset), 4);
        if (version != 0) ASSERT_EQ(::pwrite(fd, &version, 4, kHdrLayoutVersionOffset), 4);
        ::close(fd);
    }
    std::string dir_, path_;
};

template <class T>
T rd(const MetricsFile& f, int32_t off) {
    T v;
    std::memcpy(&v, f.base() + off, sizeof v);
    return v;
}

}  // namespace

TEST_F(MetricsFileRegression, CreateWritesHeaderRegression) {
    const std::byte tag[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
    CreateParams p{path_.c_str(), 16, 10, 4242, 123456789012LL, tag};
    MetricsFile f;
    ASSERT_EQ(MetricsFile::create(p, f), FileStatus::Ok);
    EXPECT_EQ(rd<int32_t>(f, kHdrLayoutVersionOffset), 2);
    EXPECT_EQ(rd<int32_t>(f, kHdrPadOffset), 0);
    EXPECT_EQ(rd<int64_t>(f, kHdrPidOffset), 4242);
    EXPECT_EQ(rd<int64_t>(f, kHdrStartEpochNsOffset), 123456789012LL);
    EXPECT_EQ(rd<int32_t>(f, kHdrMaxSlotsOffset), 16);
    EXPECT_EQ(rd<int32_t>(f, kHdrMetadataRecordLengthOffset), 512);
    EXPECT_EQ(rd<int32_t>(f, kHdrValueRecordLengthOffset), 128);
    EXPECT_EQ(rd<unsigned char>(f, kHdrUserTagOffset + 0), 1);
    EXPECT_EQ(rd<unsigned char>(f, kHdrUserTagOffset + 2), 3);
    EXPECT_EQ(rd<unsigned char>(f, kHdrUserTagOffset + 3), 0);
    EXPECT_EQ(f.layout().maxSlots, 16);
    struct stat st;
    ASSERT_EQ(::stat(path_.c_str(), &st), 0);
    EXPECT_EQ(st.st_size, FileLayout{16}.fileLength());
    EXPECT_EQ(countEntries(), 1);  // no tmp file left behind

    MetricsFile r;
    ASSERT_EQ(MetricsFile::open(path_.c_str(), 1, r), FileStatus::Ok);
    EXPECT_EQ(r.layout().maxSlots, 16);
    EXPECT_TRUE(r.sameInstanceAs(path_.c_str()));
    EXPECT_TRUE(f.sameInstanceAs(path_.c_str()));
}

TEST_F(MetricsFileRegression, CapacityTooSmallRefusedRegression) {
    {
        MetricsFile f;
        CreateParams p{path_.c_str(), 9, 10, 1, 1, {}};
        EXPECT_EQ(MetricsFile::create(p, f), FileStatus::CapacityTooSmall);
        EXPECT_EQ(countEntries(), 0);  // no file, no tmp
    }
    {
        MetricsFile f;
        CreateParams p{path_.c_str(), 10, 10, 1, 1, {}};
        EXPECT_EQ(MetricsFile::create(p, f), FileStatus::Ok);
    }
    {
        ::unlink(path_.c_str());
        MetricsFile f;
        CreateParams p{path_.c_str(), INT32_MAX, int64_t{INT32_MAX} + 1, 1, 1, {}};
        EXPECT_EQ(MetricsFile::create(p, f), FileStatus::CapacityTooLarge);
        EXPECT_EQ(countEntries(), 0);
    }
}

TEST_F(MetricsFileRegression, RecreateWhileReaderMappedRegression) {
    MetricsFile w1;
    ASSERT_EQ(MetricsFile::create(params(64, 111), w1), FileStatus::Ok);
    const int64_t marker = 0x1122334455667788LL;
    std::memcpy(w1.base() + w1.layout().valuesOffset() + 63 * kValueRecordLength, &marker, 8);

    MetricsFile reader;
    ASSERT_EQ(MetricsFile::open(path_.c_str(), 1, reader), FileStatus::Ok);

    MetricsFile w2;
    ASSERT_EQ(MetricsFile::create(params(1, 222), w2), FileStatus::Ok);  // smaller file, same path

    // Every byte of the old mapping is still readable (no SIGBUS) and shows file 1.
    const auto* b = reinterpret_cast<const volatile unsigned char*>(reader.base());
    const int64_t len = reader.layout().fileLength();
    unsigned sum = 0;
    for (int64_t i = 0; i < len; ++i) sum += b[i];
    (void)sum;
    EXPECT_EQ(rd<int64_t>(reader, static_cast<int32_t>(reader.layout().valuesOffset() + 63 * kValueRecordLength)),
              marker);
    EXPECT_EQ(rd<int64_t>(reader, kHdrStartEpochNsOffset), 111);
    EXPECT_FALSE(reader.sameInstanceAs(path_.c_str()));
    EXPECT_TRUE(w2.sameInstanceAs(path_.c_str()));

    MetricsFile again;
    ASSERT_EQ(MetricsFile::open(path_.c_str(), 1, again), FileStatus::Ok);
    EXPECT_EQ(again.layout().maxSlots, 1);
    EXPECT_EQ(rd<int64_t>(again, kHdrStartEpochNsOffset), 222);
    EXPECT_TRUE(again.sameInstanceAs(path_.c_str()));
}

TEST_F(MetricsFileRegression, ReaderWaitsForVersionWordRegression) {
    writeRaw(path_, 0, 4, FileLayout{4}.fileLength());
    MetricsFile f;
    EXPECT_EQ(MetricsFile::open(path_.c_str(), 7, f), FileStatus::NotReady);
    EXPECT_EQ(f.openPolls(), 7);
    EXPECT_EQ(MetricsFile::open(path_.c_str(), 0, f), FileStatus::NotReady);
    EXPECT_EQ(f.openPolls(), 0);
    writeRaw(path_, 2, 4, FileLayout{4}.fileLength());
    EXPECT_EQ(MetricsFile::open(path_.c_str(), 7, f), FileStatus::Ok);
    EXPECT_EQ(f.openPolls(), 1);
}

TEST_F(MetricsFileRegression, V1FileRefusedRegression) {
    writeRaw(path_, 1, 4, FileLayout{4}.fileLength());
    MetricsFile f;
    EXPECT_EQ(MetricsFile::open(path_.c_str(), 3, f), FileStatus::VersionMismatch);
    writeRaw(path_, 3, 4, FileLayout{4}.fileLength());
    EXPECT_EQ(MetricsFile::open(path_.c_str(), 3, f), FileStatus::VersionMismatch);
}

TEST_F(MetricsFileRegression, ShortFileRefusedRegression) {
    writeRaw(path_, 2, 4, FileLayout{4}.fileLength() - 1);
    MetricsFile f;
    EXPECT_EQ(MetricsFile::open(path_.c_str(), 3, f), FileStatus::BadLength);
    writeRaw(path_, 2, 4, kHeaderLength - 1);
    EXPECT_EQ(MetricsFile::open(path_.c_str(), 3, f), FileStatus::BadLength);
    writeRaw(path_, 2, -1, FileLayout{4}.fileLength());
    EXPECT_EQ(MetricsFile::open(path_.c_str(), 3, f), FileStatus::BadLength);
    writeRaw(path_, 2, 4, FileLayout{4}.fileLength());
    EXPECT_EQ(MetricsFile::open(path_.c_str(), 3, f), FileStatus::Ok);
}

TEST_F(MetricsFileRegression, PathBoundsTable) {
    struct Row { size_t len; FileStatus create; };
    const Row rows[] = {
        {0, FileStatus::PathTooLong},
        {static_cast<size_t>(PATH_MAX) - 6, FileStatus::PathTooLong},
        {static_cast<size_t>(PATH_MAX), FileStatus::PathTooLong},
        {static_cast<size_t>(PATH_MAX) + 100, FileStatus::PathTooLong},
    };
    for (const Row& r : rows) {
        std::string p(r.len, 'a');
        if (!p.empty()) p[0] = '/';
        CreateParams cp{p.c_str(), 1, 1, 1, 1, {}};
        MetricsFile f, g;
        FileStatus c, o;
        {
            metrics::testing::AllocScope scope;
            c = MetricsFile::create(cp, f);
            o = MetricsFile::open(p.c_str(), 1, g);
            EXPECT_EQ(scope.count(), 0) << "len " << r.len;
        }
        EXPECT_EQ(c, r.create) << "len " << r.len;
        EXPECT_EQ(o, FileStatus::PathTooLong) << "len " << r.len;
    }
    // A usable path creates and opens with zero allocations.
    MetricsFile f, g;
    FileStatus c, o;
    {
        metrics::testing::AllocScope scope;
        c = MetricsFile::create(params(2), f);
        o = MetricsFile::open(path_.c_str(), 1, g);
        EXPECT_EQ(scope.count(), 0);
    }
    EXPECT_EQ(c, FileStatus::Ok);
    EXPECT_EQ(o, FileStatus::Ok);
    // Largest accepted path length (longer than the directory, so the OS says IoError, not PathTooLong).
    std::string longest(MetricsFile::kMaxPathLength, 'a');
    longest[0] = '/';
    MetricsFile h;
    EXPECT_EQ(MetricsFile::create(CreateParams{longest.c_str(), 1, 1, 1, 1, {}}, h), FileStatus::IoError);
}

TEST_F(MetricsFileRegression, NoFlockRegression) {
    {
        MetricsFile f;
        ASSERT_EQ(MetricsFile::create(params(4, 1), f), FileStatus::Ok);
    }
    int toChild[2], fromChild[2];
    ASSERT_EQ(::pipe(toChild), 0);
    ASSERT_EQ(::pipe(fromChild), 0);
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        ::close(toChild[1]);
        ::close(fromChild[0]);
        int fd = ::open(path_.c_str(), O_RDWR);
        if (fd < 0 || ::flock(fd, LOCK_EX) != 0) ::_exit(2);
        char c = 'x';
        if (::write(fromChild[1], &c, 1) != 1) ::_exit(3);
        (void)!::read(toChild[0], &c, 1);  // parent closes its end when done
        ::_exit(0);
    }
    ::close(toChild[0]);
    ::close(fromChild[1]);
    char c;
    ASSERT_EQ(::read(fromChild[0], &c, 1), 1);  // child holds the exclusive lock now

    MetricsFile r, w;
    EXPECT_EQ(MetricsFile::open(path_.c_str(), 1, r), FileStatus::Ok);
    EXPECT_EQ(MetricsFile::create(params(4, 2), w), FileStatus::Ok);
    EXPECT_TRUE(w.sameInstanceAs(path_.c_str()));

    ::close(toChild[1]);
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
