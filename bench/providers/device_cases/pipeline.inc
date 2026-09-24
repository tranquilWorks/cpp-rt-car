// Complete public Runtime pipeline. Runtime owns initialization and joined stop;
// fixtures retain both drivers and all disjoint host/device spans until then.
struct PipelineFixture {
    const Case& spec;
    detail::FakeCudaDriver cuda;
    detail::FakeXdmaDriver xdma;
    struct alignas(4096) Storage { std::array<std::int32_t,1024> values{}; };
    std::unique_ptr<Storage> cuda_host=std::make_unique<Storage>(),xdma_host=std::make_unique<Storage>(),device=std::make_unique<Storage>();
    std::array<rt::CudaStream,1> streams{};
    std::unique_ptr<rt::CudaDeviceBackend> cuda_backend;
    std::unique_ptr<rt::XdmaDeviceBackend> xdma_backend;
    rt::Runtime runtime;
    rt::HalV2BackendRegistration cuda_registration{},xdma_registration{};
    rt::DeviceBackendHandle cuda_handle{},xdma_handle{};
    rt::DeviceTimelineHandle cuda_timeline{},xdma_timeline{};
    rt::DeviceCommandBatch cuda_batch{},xdma_batch{};
    std::uint64_t prepares{},bridges{},validations{},checksum{};
    bool graph{}, fault{};
    explicit PipelineFixture(const Case& c):spec(c),graph(std::string_view(c.operation).find("graph")!=std::string_view::npos),fault(!c.allocation_free) {}
    ~PipelineFixture() { if(finish()!=Status::ok) std::terminate(); }
    static rt::CallbackResult prepare(void* user,const rt::CallbackContext& ctx) noexcept {
        auto& s=*static_cast<PipelineFixture*>(user);
        for(std::size_t i=0;i<s.spec.bytes/4;++i) {
            s.cuda_host->values[i]=static_cast<std::int32_t>(ctx.frame.frame_index*7+i*3);
            s.xdma_host->values[i]=-1;
        }
        ++s.prepares; return rt::CallbackResult::ok;
    }
    static rt::CallbackResult cuda_provider(void* user,const rt::DeviceCallbackContext& ctx,rt::DeviceCommandBatch& batch) noexcept {
        auto& s=*static_cast<PipelineFixture*>(user); batch=s.cuda_batch;
        batch.timeout_ns=5'000'000'000ULL; batch.signals[0].value=ctx.frame.frame_index;
        return rt::CallbackResult::ok;
    }
    static rt::CallbackResult bridge(void* user,const rt::CallbackContext& ctx) noexcept {
        auto& s=*static_cast<PipelineFixture*>(user);
        for(std::size_t i=0;i<s.spec.bytes/4;++i)
            if(s.cuda_host->values[i]!=static_cast<std::int32_t>(ctx.frame.frame_index*7+i*3+1)) return rt::CallbackResult::error;
        std::memcpy(s.xdma_host->values.data(),s.cuda_host->values.data(),s.spec.bytes);
        ++s.bridges; return rt::CallbackResult::ok;
    }
    static rt::CallbackResult xdma_provider(void* user,const rt::DeviceCallbackContext& ctx,rt::DeviceCommandBatch& batch) noexcept {
        auto& s=*static_cast<PipelineFixture*>(user); batch=s.xdma_batch;
        batch.timeout_ns=5'000'000'000ULL; batch.signals[0].value=ctx.frame.frame_index;
        return rt::CallbackResult::ok;
    }
    static rt::CallbackResult validate(void* user,const rt::CallbackContext& ctx) noexcept {
        auto& s=*static_cast<PipelineFixture*>(user);
        std::uint64_t checksum=0;
        for(std::size_t i=0;i<s.spec.bytes/4;++i) {
            const auto value=s.xdma_host->values[i];
            if(value!=static_cast<std::int32_t>(ctx.frame.frame_index*7+i*3+1)) return rt::CallbackResult::error;
            checksum+=static_cast<std::uint64_t>(value);
        }
        s.checksum+=checksum; ++s.validations; return rt::CallbackResult::ok;
    }
    static rt::HalV2BufferReference reference(rt::DeviceBufferHandle buffer,std::size_t bytes,std::uint32_t access) {
        rt::HalV2BufferReference r{}; r.buffer_token=buffer.value; r.bytes=bytes; r.access=access; return r;
    }
    Status setup(const CudaSession* cuda_session,const XdmaSession* xdma_session) {
        cuda.complete_on_record.store(true);
        cuda.graph_values=device->values.data(); cuda.graph_elements=spec.bytes/4;
        streams[0]=cuda_session ? cuda_session->stream : 0x51u;
        rt::CudaBackendConfig cc{}; cc.context=cuda_session ? cuda_session->context : cuda.context;
        cc.streams=streams; cc.queue_capacity=4; cc.buffer_capacity=1; cc.kernel_capacity=1;
        cuda_backend=std::make_unique<rt::CudaDeviceBackend>(cuda_session ? cuda_session->driver : cuda.api_v2(),cc);
        std::uint64_t kernel{};
        if(graph) {
            if(cuda_session) require(cuda_session->graph_bytes==spec.bytes);
            require(cuda_backend->bind_device_buffer("pipeline.cuda",cuda_session ? cuda_session->graph_buffer :
                reinterpret_cast<rt::CudaDeviceAddress>(device->values.data()),spec.bytes)==RTFW_DEVICE_STATUS_OK);
            const std::array bindings{rt::CudaGraphBufferBinding{"pipeline.cuda",RTFW_DEVICE_ACCESS_READ_WRITE}};
            require(cuda_backend->register_graph(7,cuda_session ? cuda_session->increment_graph : cuda.graph,bindings)==RTFW_DEVICE_STATUS_OK);
        } else require(cuda_backend->register_kernel(cuda_session ? cuda_session->increment_kernel : cuda.add_one_function,kernel)==RTFW_DEVICE_STATUS_OK);
        rt::XdmaBackendConfig xc{}; xc.queue_capacity=4; xc.buffer_capacity=1; xc.worker_count=spec.depth==4 ? 2 : 1;
        xc.max_transfer_bytes=4096; xc.max_buffer_bytes=4096;
        if(xdma_session) {
            require(xdma_session->confirmed_window_bytes>=spec.bytes && xdma_session->device_offset<=UINT64_MAX-spec.bytes);
            xc=xdma_session->config;
            require(xc.queue_capacity==4 && xc.buffer_capacity==1 && xc.worker_count>=1 && xc.worker_count<=2 &&
                xc.max_transfer_bytes>=spec.bytes && xc.max_transfer_bytes<=4096 && xc.max_buffer_bytes>=spec.bytes && xc.max_buffer_bytes<=4096);
        }
        xdma_backend=std::make_unique<rt::XdmaDeviceBackend>(xdma_session ? xdma_session->driver : xdma.api_v2(),xc);
        rt::RuntimeConfig config{};
        config.callback_capacity=8; config.worker_count=spec.depth; config.executor_queue_capacity=16;
        config.scratch_bytes=4096; config.task_scratch_bytes=256; config.task_scratch_slots=16;
        config.trace_capacity=256; config.device_backend_capacity=2; config.device_buffer_capacity=2;
        config.device_outstanding_capacity=4; config.device_completion_batch=4;
        config.memory_budget_bytes=240U*1024U*1024U;
        require(runtime.configure(config)==rt::Status::ok);
        cuda_registration=cuda_backend->hal_v2_registration("pipeline.cuda");
        xdma_registration=xdma_backend->hal_v2_registration("pipeline.xdma");
        require(runtime.register_device_backend(cuda_registration,cuda_handle)==rt::Status::ok);
        require(runtime.register_device_backend(xdma_registration,xdma_handle)==rt::Status::ok);
        rt::DeviceMemoryDomainHandle cd{},xd{}; rt::HalV2MemoryDomain cm{},xm{};
        require(runtime.device_memory_domain_at(cuda_handle,1,cd,cm) && runtime.device_memory_domain_at(xdma_handle,0,xd,xm));
        require(cm.coherency==static_cast<std::uint32_t>(rt::HalV2MemoryCoherency::staged_copy) &&
            xm.coherency==static_cast<std::uint32_t>(rt::HalV2MemoryCoherency::host_coherent));
        rt::DeviceBufferHandle cb{},xb{};
        auto ch=std::as_writable_bytes(std::span(cuda_host->values)).first(spec.bytes);
        auto xh=std::as_writable_bytes(std::span(xdma_host->values)).first(spec.bytes);
        const auto cp=reinterpret_cast<std::uintptr_t>(ch.data()),xp=reinterpret_cast<std::uintptr_t>(xh.data());
        require(cp+ch.size()<=xp || xp+xh.size()<=cp);
        constexpr auto access=RTFW_DEVICE_BUFFER_HOST_READ|RTFW_DEVICE_BUFFER_HOST_WRITE|RTFW_DEVICE_BUFFER_DEVICE_READ|RTFW_DEVICE_BUFFER_DEVICE_WRITE;
        require(runtime.register_device_buffer({"pipeline.cuda",cuda_handle,cd,ch,{},spec.bytes,rt::HalV2MemoryOwnership::borrowed_host,
            access,rt::HalV2MemoryCoherency::staged_copy,rt::hal_v2_memory_sync_copy_to_device|rt::hal_v2_memory_sync_copy_from_device},cb)==rt::Status::ok);
        require(runtime.register_device_buffer({"pipeline.xdma",xdma_handle,xd,xh,{},spec.bytes,rt::HalV2MemoryOwnership::borrowed_host,
            access,rt::HalV2MemoryCoherency::host_coherent,rt::hal_v2_memory_sync_none},xb)==rt::Status::ok);
        require(runtime.register_device_timeline({"pipeline.cuda.timeline",cuda_handle,0},cuda_timeline)==rt::Status::ok);
        require(runtime.register_device_timeline({"pipeline.xdma.timeline",xdma_handle,0},xdma_timeline)==rt::Status::ok);
        cuda_batch.command_count=3; cuda_batch.signal_count=1; cuda_batch.signals[0].timeline_handle=cuda_timeline.value;
        auto& upload=cuda_batch.commands[0]; upload.kind=static_cast<std::uint32_t>(rt::HalV2CommandKind::copy);
        upload.operation=static_cast<std::uint32_t>(rt::HalV2MemoryOperation::copy_to_device);
        upload.source=reference(cb,spec.bytes,RTFW_DEVICE_ACCESS_READ); upload.destination=reference(cb,spec.bytes,RTFW_DEVICE_ACCESS_WRITE);
        auto& dispatch=cuda_batch.commands[1]; dispatch.kind=static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
        dispatch.buffer_count=1; dispatch.buffers[0]=reference(cb,spec.bytes,RTFW_DEVICE_ACCESS_READ_WRITE);
        if(graph) dispatch.opcode=rt::cuda_device_opcode_graph(7);
        else {
            rt::CudaKernelLaunch launch{}; launch.kernel_token=kernel; launch.block_x=64; launch.grid_x=static_cast<std::uint32_t>((spec.bytes/4+63)/64);
            require(rt::cuda_kernel_add_buffer_argument(launch,0));
            require(rt::cuda_kernel_add_scalar_argument(launch,static_cast<std::uint32_t>(spec.bytes/4)));
            dispatch.opcode=rt::cuda_device_opcode_launch_kernel; dispatch.payload_size=sizeof(launch);
            std::memcpy(dispatch.payload.data(),&launch,sizeof(launch));
        }
        cuda_batch.commands[2]=upload;
        cuda_batch.commands[2].operation=static_cast<std::uint32_t>(rt::HalV2MemoryOperation::copy_from_device);
        xdma_batch.command_count=2; xdma_batch.signal_count=1; xdma_batch.signals[0].timeline_handle=xdma_timeline.value;
        for(std::size_t i=0;i<2;++i) {
            auto& command=xdma_batch.commands[i]; command.kind=static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
            command.opcode=i==0 ? rt::xdma_device_opcode_host_to_card : rt::xdma_device_opcode_card_to_host;
            rt::XdmaTransfer transfer{}; transfer.device_offset=xdma_session ? xdma_session->device_offset : 0;
            command.payload_size=sizeof(transfer); std::memcpy(command.payload.data(),&transfer,sizeof(transfer));
            command.buffer_count=1; command.buffers[0]=reference(xb,spec.bytes,i==0 ? RTFW_DEVICE_ACCESS_READ : RTFW_DEVICE_ACCESS_WRITE);
        }
        std::array<rt::PhaseHandle,5> phases{};
        require(runtime.register_callback({"pipeline.prepare",prepare,this},phases[0])==rt::Status::ok);
        require(runtime.register_device_batch_phase({"pipeline.cuda",cuda_handle,cuda_provider,this,cuda_batch},phases[1])==rt::Status::ok);
        require(runtime.register_callback({"pipeline.bridge",bridge,this},phases[2])==rt::Status::ok);
        require(runtime.register_device_batch_phase({"pipeline.xdma",xdma_handle,xdma_provider,this,xdma_batch},phases[3])==rt::Status::ok);
        require(runtime.register_callback({"pipeline.validate",validate,this},phases[4])==rt::Status::ok);
        for(std::size_t i=1;i<phases.size();++i) require(runtime.add_dependency(phases[i-1],phases[i])==rt::Status::ok);
        require(runtime.finalize()==rt::Status::ok);
        rt::MemoryPlan plan{}; require(runtime.memory_plan(plan) && plan.planned_bytes<=512U*1024U*1024U);
        require(runtime.start()==rt::Status::ok); return Status::ok;
    }
    Status finish() noexcept {
        return runtime.state()==rt::RuntimeState::configuring || runtime.state()==rt::RuntimeState::stopped ||
            runtime.stop()==rt::Status::ok ? Status::ok : Status::provider_error;
    }
    rt::HalV2Health health(const rt::HalV2BackendRegistration& r) {
        rt::HalV2Health h{}; require(r.api.get_health(r.api.instance,&h)==rt::HalV2Status::ok); return h;
    }
    std::uint64_t invoke(std::uint64_t ordinal,Measures& m) {
        const auto cb=health(cuda_registration),xb=health(xdma_registration);
        const auto pb=prepares,bb=bridges,vb=validations,checksum_before=checksum;
        const auto transforms_before=graph ? cuda.graph_launches.load() : cuda.launches.load();
        const auto transfers_before=xdma.transfers.load();
        if(fault) cuda.fail_next_graph_launch.store(true);
        for(std::size_t i=0;i<spec.depth;++i) {
            const auto frame=ordinal*spec.depth+i+1;
            const auto status=runtime.step({frame,std::chrono::milliseconds(1),std::nullopt});
            if(fault) {
                require(status==rt::Status::device_reset_required && validations==vb && bridges==bb && checksum==checksum_before && xdma.transfers.load()==transfers_before);
                ++m.failed;
                require(runtime.reset_device(cuda_handle)==rt::Status::ok); ++m.resets;
                return 0;
            }
            require(status==rt::Status::ok);
            rt::DeviceTimelineInfo ct{},xt{};
            require(runtime.device_timeline_at(cuda_handle,0,ct) && runtime.device_timeline_at(xdma_handle,0,xt));
            require(ct.timeline==cuda_timeline && xt.timeline==xdma_timeline && ct.timeline!=xt.timeline &&
                ct.backend==cuda_handle && xt.backend==xdma_handle && ct.completed_value==frame && xt.completed_value==frame &&
                ct.last_accepted_value==frame && xt.last_accepted_value==frame);
        }
        const auto ca=health(cuda_registration),xa=health(xdma_registration);
        m.submissions=ca.submissions-cb.submissions+xa.submissions-xb.submissions;
        m.completions=ca.completions-cb.completions+xa.completions-xb.completions;
        require(m.submissions==2*spec.depth && m.completions==m.submissions && ca.outstanding==0 && xa.outstanding==0);
        require(prepares-pb==spec.depth && bridges-bb==spec.depth && validations-vb==spec.depth);
        if(!spec.real) require((graph ? cuda.graph_launches.load() : cuda.launches.load())-transforms_before==spec.depth &&
            xdma.transfers.load()-transfers_before==2*spec.depth);
        m.kernels=validations-vb; m.checked_elements=(validations-vb)*spec.bytes/4;
        m.copied_bytes=spec.bytes*(m.completions*2+bridges-bb);
        return checksum-checksum_before;
    }
};
