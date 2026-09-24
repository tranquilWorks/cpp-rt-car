// Explicit AXI-MM scratch-window host. No guessed endpoint or register map.
#include "device_provider.hpp"
#include <rt/xdma_linux.hpp>
#include <array>
#include <charconv>
#include <filesystem>
#include <iostream>
namespace b=rtfw::benchmark;
int main(int argc,char** argv) {
    if(argc!=7) {
        std::cerr<<"usage: rtfw-bench-xdma REAL_XDMA_CASE NEW_OUTPUT_DIRECTORY H2C_PATH C2H_PATH CONFIRMED_OFFSET CONFIRMED_BYTES\n";
        return 2;
    }
    try {
        const std::string_view selected=argv[1];
        const b::device::Case* chosen=nullptr;
        for(const auto& c:b::device::cases()) if(c.real && selected==c.id && selected.starts_with("real-xdma-")) chosen=&c;
        if(!chosen || b::check_destination(argv[2])!=b::Status::ok) return 2;
        const auto number=[](std::string_view text,std::uint64_t& value) {
            const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
            return parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size();
        };
        b::device::XdmaSession session;
        if(!number(argv[5],session.device_offset) || !number(argv[6],session.confirmed_window_bytes) ||
            session.confirmed_window_bytes<chosen->bytes || session.device_offset>UINT64_MAX-chosen->bytes) return 2;
        const std::array<std::string_view,1> h2c{argv[3]},c2h{argv[4]};
        // Absence is NOT RUN. An existing but invalid/unopenable supplied endpoint
        // is an initialization error, never a silent fallback to fake storage.
        const bool h2c_exists=std::filesystem::exists(argv[3]),c2h_exists=std::filesystem::exists(argv[4]);
        if((h2c_exists && !std::filesystem::is_character_file(argv[3])) ||
            (c2h_exists && !std::filesystem::is_character_file(argv[4]))) return 2;
        const bool available=h2c_exists && c2h_exists;
        rt::LinuxXdmaConfig driver_config{}; driver_config.h2c_paths=h2c; driver_config.c2h_paths=c2h;
        rt::LinuxXdmaDriver driver(driver_config);
        session.driver=driver.api();
        session.config.queue_capacity=1; session.config.buffer_capacity=1; session.config.worker_count=1;
        session.config.max_transfer_bytes=4096; session.config.max_buffer_bytes=4096;
        b::device::Provider provider(nullptr,available ? &session : nullptr);
        b::Runner runner; b::ProviderHandle handle;
        if(runner.register_provider(provider.table(),handle)!=b::Status::ok) return 1;
        const auto prepared=provider.prepare(selected);
        if(prepared!=b::Status::ok && prepared!=b::Status::not_run) return 1;
        auto identity=b::capture_identity();
        identity.backend=available ? "xdma-linux-axi-mm" : "not_available";
        identity.driver=available ? "linux-xdma-character-device" : "not_available";
        const auto result=runner.run("rtfw.device",selected,b::steady_clock(),identity);
        if(provider.finish()!=b::Status::ok || b::publish(result,argv[2])!=b::Status::ok) return 1;
        std::cout<<"status="<<b::status_name(result.status)<<" measured="<<result.samples.size()<<'\n';
        return result.status==b::Status::ok ? 0 : result.status==b::Status::not_run ? 3 : 1;
    } catch(...) { std::cerr<<"XDMA benchmark failed\n"; return 1; }
}
