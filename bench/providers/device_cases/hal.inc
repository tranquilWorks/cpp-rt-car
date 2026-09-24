struct HalFixture {
    const Case& spec;
    detail::FakeCudaDriver cuda;
    detail::FakeXdmaDriver xdma;
    std::array<rt::CudaStream,1> streams{0x51u};
    std::unique_ptr<rt::CudaDeviceBackend> cuda_backend;
    std::unique_ptr<rt::XdmaDeviceBackend> xdma_backend;
    rtfw_device_backend_api legacy{};
    rt::HalV2BackendRegistration native{};
    bool v2{},is_cuda{};
    explicit HalFixture(const Case& c):spec(c),v2(std::string_view(c.operation).ends_with("v2")),is_cuda(std::string_view(c.operation).starts_with("hal-cuda")) {
        if(is_cuda) {
            rt::CudaBackendConfig config{}; config.context=cuda.context; config.streams=streams;
            config.queue_capacity=c.depth; config.buffer_capacity=2;
            cuda_backend=std::make_unique<rt::CudaDeviceBackend>(cuda.api_v2(),config);
            if(v2) native=cuda_backend->hal_v2_registration("benchmark.hal"); else legacy=cuda_backend->api();
        } else {
            rt::XdmaBackendConfig config{}; config.queue_capacity=c.depth; config.buffer_capacity=2;
            config.worker_count=1; config.max_buffer_bytes=4096; config.max_transfer_bytes=4096;
            xdma_backend=std::make_unique<rt::XdmaDeviceBackend>(xdma.api_v2(),config);
            if(v2) native=xdma_backend->hal_v2_registration("benchmark.hal"); else legacy=xdma_backend->api();
        }
    }
    std::uint64_t invoke(Measures& m) {
        std::uint64_t checksum=0;
        if(v2) {
            rt::HalV2Capabilities c{};
            require(native.api.get_capabilities(native.api.instance,&c)==rt::HalV2Status::ok);
            require(c.api_version==rt::hal_v2_api_version && c.max_in_flight==spec.depth && c.max_registered_buffers==2 &&
                c.inline_payload_capacity==128 && c.buffer_ref_capacity==8 && c.supports_reset==1 && c.deterministic_mock==0 && c.control_storage_bytes>0);
            rt::HalV2MemoryTopologySnapshot topology{};
            require(native.memory_topology && native.memory_topology->discover(native.memory_topology->instance,&topology)==rt::HalV2Status::ok);
            require(topology.memory_domain_count>=1 && topology.timestamp_domain_count>=1);
            rt::HalV2CommandTimelineCapabilities command{};
            require(native.command_timeline && native.command_timeline->get_capabilities(native.command_timeline->instance,&command)==rt::HalV2Status::ok);
            require(command.max_in_flight_batches==spec.depth && command.max_commands_per_batch==rt::hal_v2_command_capacity);
            c.struct_size=0;
            require(native.api.get_capabilities(native.api.instance,&c)==rt::HalV2Status::invalid_argument); ++m.rejected;
            checksum=spec.depth+2+128+8;
        } else {
            rtfw_device_capabilities c{}; c.struct_size=sizeof(c);
            require(legacy.get_capabilities(legacy.instance,&c)==RTFW_DEVICE_STATUS_OK);
            require(c.abi_version==RTFW_DEVICE_ABI_VERSION && c.max_in_flight==spec.depth && c.max_registered_buffers==2 &&
                c.inline_payload_capacity==128 && c.buffer_ref_capacity==8 && c.supports_reset==1 && c.deterministic_mock==0 && c.control_storage_bytes>0);
            checksum=c.max_in_flight+c.max_registered_buffers+c.inline_payload_capacity+c.buffer_ref_capacity;
            c.struct_size=0;
            require(legacy.get_capabilities(legacy.instance,&c)==RTFW_DEVICE_STATUS_INVALID_ARGUMENT); ++m.rejected;
        }
        require(cuda.allocations.load()==0 && cuda.event_create_calls.load()==0 && !xdma.initialized.load() && xdma.transfers.load()==0);
        m.checked_elements=8; return checksum;
    }
};
