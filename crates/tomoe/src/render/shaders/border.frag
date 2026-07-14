precision highp float;

uniform float niri_alpha;
uniform float niri_scale;
uniform vec2 niri_size;
varying vec2 niri_v_coords;
uniform vec4 color;
uniform vec2 geo_size;
uniform vec4 outer_radius;
uniform float border_width;

float tomoe_rounding_alpha(vec2 coords, vec2 size, vec4 corner_radius);

void main() {
    vec2 coords = niri_v_coords * geo_size;
    float alpha = tomoe_rounding_alpha(coords, geo_size, outer_radius);
    vec2 inner_coords = coords - vec2(border_width);
    vec2 inner_size = geo_size - vec2(border_width * 2.0);
    if (0.0 <= inner_coords.x && inner_coords.x <= inner_size.x &&
        0.0 <= inner_coords.y && inner_coords.y <= inner_size.y) {
        vec4 inner_radius = max(outer_radius - vec4(border_width), 0.0);
        alpha *= 1.0 - tomoe_rounding_alpha(inner_coords, inner_size, inner_radius);
    }
    vec4 premultiplied = color;
    premultiplied.rgb *= premultiplied.a;
    gl_FragColor = premultiplied * alpha * niri_alpha;
}
