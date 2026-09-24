#pragma once
#include <string_view>
namespace rtfw::benchmark::device::native {
inline constexpr std::string_view kPtx = R"ptx(
.version 6.0
.target sm_50
.address_size 64

.visible .entry rtfw_add_one(
    .param .u64 rtfw_add_one_param_0,
    .param .u32 rtfw_add_one_param_1
)
{
    .reg .pred %p;
    .reg .b32 %r<5>;
    .reg .b64 %rd<4>;

    ld.param.u64 %rd1, [rtfw_add_one_param_0];
    ld.param.u32 %r1, [rtfw_add_one_param_1];
    mov.u32 %r2, %tid.x;
    mov.u32 %r3, %ctaid.x;
    mov.u32 %r4, %ntid.x;
    mad.lo.s32 %r2, %r3, %r4, %r2;
    setp.ge.u32 %p, %r2, %r1;
    @%p bra DONE;
    mul.wide.u32 %rd2, %r2, 4;
    add.s64 %rd3, %rd1, %rd2;
    ld.global.u32 %r3, [%rd3];
    add.u32 %r3, %r3, 1;
    st.global.u32 [%rd3], %r3;
DONE:
    ret;
}
)ptx";
}
