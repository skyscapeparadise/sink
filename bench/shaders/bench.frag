#version 300 es
precision highp float;
// render_bench's shader background: four octaves of value noise, warped, the
// kind of thing people actually set (more than a gradient, well short of a
// raymarcher). It only has to be a fixed, representative cost; change it and
// earlier render.shader results stop being comparable.
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y);
}
float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) { v += a * noise(p); p *= 2.03; a *= 0.5; }
    return v;
}
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.y;
    vec2 q = vec2(fbm(uv * 3.0 + iTime * 0.1), fbm(uv * 3.0 + vec2(5.2, 1.3)));
    float v = fbm(uv * 3.0 + 2.0 * q);
    fragColor = vec4(mix(vec3(0.05, 0.02, 0.10), vec3(0.45, 0.25, 0.70), v), 1.0);
}
