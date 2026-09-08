#include "support.hpp"

namespace rtfw::benchmark::runtime::detail {
namespace {
struct Checkpoint final:Fixture {
    Case c;
    std::vector<std::byte> state,output,corrupt,foreign;
    RuntimeOwner owner;
    explicit Checkpoint(const Case& value):c(value),state(c.bytes) {
        auto cfg=config();cfg.state_capacity=c.width;
        okay(owner.rt.configure(cfg));
        for(std::size_t i=0;i<c.width;++i)
            okay(owner.rt.register_state({"canonical-state-"+std::to_string(i),1,
                std::span<std::byte>(state).subspan(i*(c.bytes/c.width),c.bytes/c.width)}));
        finalized(owner);output=checkpoint(owner.rt);corrupt.resize(output.size());
        // A well-formed checkpoint with a different state schema is rejected by
        // restore, independently of the malformed-byte checks below.
        std::vector<std::byte> other_state(c.bytes);
        RuntimeOwner other;okay(other.rt.configure(cfg));
        for(std::size_t i=0;i<c.width;++i)
            okay(other.rt.register_state({"foreign-state-"+std::to_string(i),1,
                std::span<std::byte>(other_state).subspan(i*(c.bytes/c.width),c.bytes/c.width)}));
        finalized(other);foreign=checkpoint(other.rt);okay(other.stop());
    }
    Measures run(std::uint64_t ordinal) override {
        Measures m;put(state,ordinal+1);const auto expected=digest(state);
        rt::ArtifactWriteResult write;
        require(owner.rt.write_checkpoint(ordinal,std::span<std::byte>(output).first(output.size()-1),write)==rt::Status::capacity_exceeded);
        require(write.bytes_written==0 && write.required_bytes==output.size());++m.rejected;
        okay(owner.rt.write_checkpoint(ordinal,output,write));++m.operations;m.bytes=write.bytes_written;
        rt::CheckpointMetadata metadata;
        okay(rt::inspect_checkpoint_artifact(output,metadata));++m.operations;
        require(metadata.state_count==c.width && metadata.state_payload_bytes==c.bytes && metadata.checkpoint_frame_index==ordinal);
        put(state,ordinal+99);const auto sentinel=digest(state);
        corrupt=output;corrupt.back()^=std::byte{1};
        for(auto bytes:{std::span<const std::byte>(corrupt),std::span<const std::byte>(output).first(output.size()-1),std::span<const std::byte>(foreign)}) {
            require(owner.rt.restore_checkpoint(bytes)!=rt::Status::ok);++m.rejected;
            require(digest(state)==sentinel);
        }
        okay(owner.rt.restore_checkpoint(output));++m.operations;
        m.records=metadata.state_count;m.correct=digest(state)==expected && matches(state,ordinal+1);
        m.checksum=digest(state);return m;
    }
    rt::Status finish() noexcept override{return owner.stop();}
};
}
std::unique_ptr<Fixture> make_checkpoint(const Case& c){return std::make_unique<Checkpoint>(c);}
} // namespace rtfw::benchmark::runtime::detail
