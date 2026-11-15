void mainImage(out vec4 color, in vec2 fragCoord) {
    vec2 p = floor(fragCoord);
    float band = mod(floor(p.x / 96.0) + floor(p.y / 96.0), 2.0);
    float stripe = mod(p.y, 8.0) < 1.0 ? 0.03 : 0.0;
    float shade = 0.10 * mod(p.x, 5.0) / 5.0 + 0.06 * mod(p.y, 3.0) / 3.0;
    vec3 base = vec3(0.05, 0.07, 0.13);
    vec3 glow = vec3(0.24, 0.10, 0.34);
    color = vec4(mix(base, glow, band) + vec3(shade + stripe), 1.0);
}
