// The two DS screens: a textured quad each. ScreenRenderer sets the quad's corners (in
// normalized device coordinates, top left then bottom right) per draw.
#include <metal_stdlib>
using namespace metal;

struct Quad { float4 corners; };
struct Out { float4 position [[position]]; float2 uv; };

// a triangle strip over the quad: vertex 0 top left, 1 top right, 2 bottom left, 3 bottom right
vertex Out screen_vertex(uint vid [[vertex_id]], constant Quad &q [[buffer(0)]]) {
    float2 t = float2(float(vid & 1), float(vid >> 1));
    Out o;
    o.position = float4(mix(q.corners.xy, q.corners.zw, t), 0, 1);
    o.uv = t;
    return o;
}

fragment float4 screen_fragment(Out in [[stage_in]], texture2d<float> screen [[texture(0)]],
                                sampler s [[sampler(0)]]) {
    return float4(screen.sample(s, in.uv).rgb, 1);
}
