#include "benchmark_fixtures/provider.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <random>
#include <thread>
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
using namespace benchmark_test;
namespace {
struct Workspace {
    std::filesystem::path path;
    Workspace() {
        std::random_device random;
        for (unsigned n=0; n<32; ++n) {
            path=std::filesystem::temp_directory_path()/("rtfw-test-"+std::to_string(random()));
            if (std::filesystem::create_directory(path)) return;
        }
        throw std::runtime_error("test workspace unavailable");
    }
    ~Workspace() { std::error_code error; std::filesystem::remove_all(path,error); }
};
std::string read(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
std::size_t count(const std::filesystem::path& path) {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(path),
                                               std::filesystem::directory_iterator()));
}
}
TEST(BenchmarkArtifacts, ExactFilesAndNoOverwrite) {
    Workspace w; Fixture f; const auto r=result(f); const auto a=b::encode(r); const auto output=w.path/"result";
    ASSERT_EQ(b::publish(r,output),b::Status::ok);
    ASSERT_EQ(count(output),3U);
    EXPECT_EQ(read(output/"descriptor.json"),a.descriptor);
    EXPECT_EQ(read(output/"raw.json"),a.raw); EXPECT_EQ(read(output/"result.json"),a.summary);
    EXPECT_EQ(b::publish(r,output),b::Status::exists);
    EXPECT_EQ(read(output/"result.json"),a.summary); EXPECT_EQ(count(w.path),1U);
}
TEST(BenchmarkArtifacts, RejectsUnsafeMissingAndInvalidWithoutOutput) {
    Workspace w; Fixture f; auto r=result(f);
    EXPECT_EQ(b::publish(r,w.path/".."/"unsafe"),b::Status::invalid);
    EXPECT_EQ(b::publish(r,w.path/"missing"/"result"),b::Status::io_error);
    r.samples[0].index=23;
    EXPECT_EQ(b::publish(r,w.path/"bad"),b::Status::invalid); EXPECT_EQ(count(w.path),0U);
    r=result(f); r.start_utc="1970-02-31T00:00:00Z";
    EXPECT_THROW((void)b::encode(r),std::invalid_argument);
}
TEST(BenchmarkArtifacts, ConcurrentCreateNewHasOneWinner) {
    Workspace w; Fixture f; const auto r=result(f); b::Status one{},two{};
    std::thread a([&]{one=b::publish(r,w.path/"race");});
    std::thread c([&]{two=b::publish(r,w.path/"race");}); a.join(); c.join();
    EXPECT_TRUE((one==b::Status::ok && two==b::Status::exists) ||
                (two==b::Status::ok && one==b::Status::exists));
    EXPECT_EQ(count(w.path),1U); EXPECT_EQ(count(w.path/"race"),3U);
}
TEST(BenchmarkArtifacts, FailedRunKeepsFailureAndNoMeasuredStatistics) {
    Workspace w; Fixture f; f.throw_invoke=true; auto r=result(f);
    ASSERT_EQ(b::publish(r,w.path/"failed"),b::Status::ok);
    const auto contents=read(w.path/"failed"/"result.json");
    EXPECT_NE(contents.find("\"status\":\"provider_error\""),std::string::npos);
    EXPECT_NE(contents.find("\"statistics\":null"),std::string::npos);
    EXPECT_EQ(contents.find("secret"),std::string::npos);
    r.diagnostic="private-user-data"; EXPECT_THROW((void)b::encode(r),std::invalid_argument);
}
TEST(BenchmarkArtifacts, DoesNotFollowSymlinks) {
    Workspace w; Fixture f; const auto r=result(f);
    std::filesystem::create_directory(w.path/"real");
    std::error_code error;
    std::filesystem::create_directory_symlink(w.path/"real",w.path/"link",error);
    if (error) GTEST_SKIP() << "symlink privilege unavailable; Windows reparse CI remains required";
    EXPECT_EQ(b::publish(r,w.path/"link"/"result"),b::Status::io_error);
    EXPECT_EQ(b::publish(r,w.path/"link"),b::Status::exists);
    EXPECT_EQ(count(w.path/"real"),0U);
}
#ifndef _WIN32
TEST(BenchmarkArtifacts, ShortWriteRemovesPrivateStagingDirectory) {
    Workspace w; Fixture f; const auto r=result(f); const auto output=w.path/"limited";
    const pid_t child=fork(); ASSERT_GE(child,0);
    if (!child) {
        std::signal(SIGXFSZ,SIG_IGN);
        const struct rlimit limit{64,64};
        if (setrlimit(RLIMIT_FSIZE,&limit)!=0) _exit(2);
        _exit(b::publish(r,output)==b::Status::io_error ? 0 : 1);
    }
    int status=0; ASSERT_EQ(waitpid(child,&status,0),child);
    ASSERT_TRUE(WIFEXITED(status)); EXPECT_EQ(WEXITSTATUS(status),0);
    EXPECT_EQ(count(w.path),0U);
}
#endif

#ifdef _WIN32
TEST(BenchmarkArtifacts, RejectsWindowsNetworkDeviceAndAmbiguousPaths) {
    for (const auto* path:{"//server/share/out", "\\\\server\\share\\out", "C:relative", "CON", "aux.txt", "LPT1", "name.", "name:stream"})
        EXPECT_EQ(b::check_destination(path),b::Status::invalid);
}
#endif
