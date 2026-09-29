// Frozen M26-01 int32 SoA: x[3][256], v[3][256], effort[3][256].
// Input staging supplies exact bounded effort; inactive lanes remain zero.
extern "C" __global__ void golden_physics_step(int *state, unsigned count) {
  static_assert(sizeof(int)==4, "32-bit integer scenario required");
  const unsigned i=blockIdx.x*blockDim.x+threadIdx.x;
  if (i>=count) return;
  for (unsigned a=0;a<3;++a) {
    const unsigned x=a*256+i,v=x+3*256,effort=x+6*256;
    const int speed=state[v]+state[effort];
    state[v]=speed; state[x]+=speed;
  }
}
