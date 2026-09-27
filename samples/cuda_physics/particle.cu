// Model v1: each particle is x[3], v[3], a[3], all signed 32-bit integers.
// The host restricts the state and horizon so all intermediate values fit.
extern "C" __global__ void rtfw_particle_step(int* state, unsigned count) {
    static_assert(sizeof(int) == 4, "model requires 32-bit signed integers");
    const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    for (unsigned axis = 0; axis < 3; ++axis) {
        const unsigned offset = 9*i + axis;
        const int velocity = state[offset+3] + state[offset+6];
        state[offset+3] = velocity;
        state[offset] += velocity;
    }
}
