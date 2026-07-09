precision highp float;

uniform float niri_alpha;
varying vec2 niri_v_coords;
uniform vec4 color;
uniform vec2 geo_size;
uniform float shadow_range;
uniform float corner_radius;
uniform float shadow_power;

// Signed distance to the window's rounded rectangle. This is the same
// rounded-distance falloff shape Hyprland uses, expressed as an SDF so all
// geometry remains in Tomoe's integer physical-pixel coordinate space.
float rounded_rect_distance(vec2 p, vec2 half_size, float radius) {
    vec2 q = abs(p) - (half_size - vec2(radius));
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

void main() {
    if (shadow_range <= 0.0) discard;
    vec2 coords = niri_v_coords * geo_size;
    vec2 window_size = geo_size - vec2(2.0 * shadow_range);
    vec2 center = geo_size * 0.5;
    float radius = min(corner_radius, min(window_size.x, window_size.y) * 0.5);
    float distance_to_window = rounded_rect_distance(coords - center, window_size * 0.5, radius);

    // The window interior is transparent; outside it, fade to zero over
    // `shadow_range`. Half-pixel smoothing keeps the inner edge stable.
    if (distance_to_window <= -0.5 || distance_to_window >= shadow_range) discard;
    float outside = max(distance_to_window, 0.0);
    float falloff = pow(clamp(1.0 - outside / shadow_range, 0.0, 1.0), shadow_power);
    falloff *= smoothstep(-0.5, 0.5, distance_to_window);

    vec4 premultiplied = color;
    premultiplied.rgb *= premultiplied.a;
    gl_FragColor = premultiplied * falloff * niri_alpha;
}
