// Included inside the provider's private namespace; public backend APIs only.
struct GraphFixture {
    const Case& spec;
    detail::FakeCudaDriver fake;
    std::array<std::int32_t,1024> host{}, device{};
    std::array<rt::CudaStream,1> streams{};
    std::unique_ptr<rt::CudaDeviceBackend> backend;
    rt::HalV2BackendRegistration registration{};
    rt::HalV2MemoryToken token{};
    bool initialized{}, initialization_succeeded{};
    explicit GraphFixture(const Case& c):spec(c) {}
    ~GraphFixture() { if(finish()!=Status::ok) std::terminate(); }
    Status setup(const CudaSession* session) {
        rt::CudaBackendConfig config{};
        config.context=session ? session->context : fake.context;
        streams[0]=session ? session->stream : 0x51u; config.streams=streams;
        config.queue_capacity=spec.depth; config.buffer_capacity=1;
        fake.complete_on_record.store(true);
        fake.graph_values=device.data(); fake.graph_elements=spec.bytes/sizeof(std::int32_t);
        backend=std::make_unique<rt::CudaDeviceBackend>(session ? session->driver : fake.api_v2(),config);
        if(session) require(session->graph_bytes==spec.bytes);
        require(backend->bind_device_buffer("benchmark.graph",session ? session->graph_buffer :
            reinterpret_cast<rt::CudaDeviceAddress>(device.data()),spec.bytes)==RTFW_DEVICE_STATUS_OK);
        const std::array bindings{rt::CudaGraphBufferBinding{"benchmark.graph",RTFW_DEVICE_ACCESS_READ_WRITE}};
        require(backend->register_graph(7,session ? session->increment_graph : fake.graph,bindings)==RTFW_DEVICE_STATUS_OK);
        registration=backend->hal_v2_registration("benchmark.graph");
        rt::HalV2InitializeConfig init{}; init.requested_in_flight=spec.depth; init.requested_registered_buffers=1;
        initialized=true;
        initialization_succeeded=registration.api.initialize(registration.api.instance,&init)==rt::HalV2Status::ok;
        require(initialization_succeeded);
        rt::HalV2MemoryRegistration memory{};
        memory.domain_identity=2; memory.bytes=spec.bytes;
        memory.ownership=static_cast<std::uint32_t>(rt::HalV2MemoryOwnership::borrowed_host);
        memory.access=RTFW_DEVICE_BUFFER_HOST_READ|RTFW_DEVICE_BUFFER_HOST_WRITE|
            RTFW_DEVICE_BUFFER_DEVICE_READ|RTFW_DEVICE_BUFFER_DEVICE_WRITE;
        memory.coherency=static_cast<std::uint32_t>(rt::HalV2MemoryCoherency::staged_copy);
        memory.synchronization=rt::hal_v2_memory_sync_copy_to_device|rt::hal_v2_memory_sync_copy_from_device;
        memory.host_data=host.data();
        std::memcpy(memory.name.data(),"benchmark.graph",sizeof("benchmark.graph"));
        require(registration.memory_topology->register_memory(registration.memory_topology->instance,&memory,&token)==rt::HalV2Status::ok);
        return Status::ok;
    }
    Status finish() noexcept {
        if(!initialized) return Status::ok;
        const auto status=registration.api.shutdown(registration.api.instance);
        if(status!=rt::HalV2Status::ok && !(status==rt::HalV2Status::invalid_state && !initialization_succeeded))
            return Status::provider_error;
        initialized=false; return Status::ok;
    }
    std::uint64_t invoke(std::uint64_t ordinal,Measures& m) {
        for(std::size_t i=0;i<spec.bytes/4;++i) host[i]=static_cast<std::int32_t>(ordinal*7+i*3);
        rt::DeviceCommandBatch batch{};
        batch.batch_id=ordinal+1; batch.frame_index=ordinal; batch.timeout_ns=5'000'000'000ULL;
        batch.command_count=3; batch.signal_count=1;
        batch.signals[0].timeline_handle=11; batch.signals[0].value=ordinal+1;
        rt::HalV2BufferReference reference{};
        reference.buffer_token=token.submission_token; reference.bytes=spec.bytes;
        auto& upload=batch.commands[0];
        upload.kind=static_cast<std::uint32_t>(rt::HalV2CommandKind::copy);
        upload.operation=static_cast<std::uint32_t>(rt::HalV2MemoryOperation::copy_to_device);
        upload.source=reference; upload.source.access=RTFW_DEVICE_ACCESS_READ;
        upload.destination=reference; upload.destination.access=RTFW_DEVICE_ACCESS_WRITE;
        auto& graph=batch.commands[1]; graph.kind=static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
        graph.opcode=rt::cuda_device_opcode_graph(7); graph.buffer_count=1;
        graph.buffers[0]=reference; graph.buffers[0].access=RTFW_DEVICE_ACCESS_READ_WRITE;
        batch.commands[2]=upload;
        batch.commands[2].operation=static_cast<std::uint32_t>(rt::HalV2MemoryOperation::copy_from_device);
        auto& command=*registration.command_timeline;
        const auto graph_before=fake.graph_launches.load();
        fake.call_order_count.store(0);
        require(command.submit(command.instance,&batch)==rt::HalV2Status::ok); ++m.submissions;
        rt::HalV2Health health{};
        require(registration.api.get_health(registration.api.instance,&health)==rt::HalV2Status::ok);
        m.peak_outstanding=health.outstanding;
        rt::HalV2BatchCompletion completion{}; std::uint64_t count=0;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        do {
            require(command.poll(command.instance,&completion,1,&count)==rt::HalV2Status::ok); ++m.polls;
            if(count==1) break;
            require(count==0); std::this_thread::yield();
        } while(std::chrono::steady_clock::now()<deadline);
        require(count==1 && completion.batch_id==batch.batch_id &&
            completion.status==static_cast<std::int32_t>(rt::HalV2Status::ok) && completion.signal_count==1 &&
            completion.signals[0].timeline_handle==11 && completion.signals[0].value==ordinal+1);
        ++m.completions; m.copied_bytes=spec.bytes*2; m.kernels=1;
        if(!spec.real) {
            require(fake.graph_launches.load()==graph_before+1 && fake.call_order_count.load()>=4);
            require(fake.call_order[0]=='H' && fake.call_order[1]=='G' && fake.call_order[2]=='D' && fake.call_order[3]=='E');
        }
        require(registration.api.get_health(registration.api.instance,&health)==rt::HalV2Status::ok && health.outstanding==0);
        count=0;
        require(command.poll(command.instance,&completion,1,&count)==rt::HalV2Status::ok && count==0); ++m.polls;
        std::uint64_t checksum=0;
        for(std::size_t i=0;i<spec.bytes/4;++i) {
            require(host[i]==static_cast<std::int32_t>(ordinal*7+i*3+1));
            checksum+=static_cast<std::uint64_t>(host[i]); ++m.checked_elements;
        }
        return checksum;
    }
};
