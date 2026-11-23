#ifndef TOMOE_SHADER_VK_H
#define TOMOE_SHADER_VK_H

#ifdef TOMOE_VK_SOURCES
static const char tomoe_vk_prelude[] =
    "#version 450\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "layout(set = 0, binding = 0, std140) uniform TomoeUniforms {\n"
    "    vec4 iMouse;\n"
    "    vec4 iMarksBox;\n"
    "    vec4 tomoe_beneath_box;\n"
    "    vec4 tomoe_frost_box;\n"
    "    vec3 iResolution;\n"
    "    float iTime;\n"
    "    int iFrame;\n"
    "    int iTrailCount;\n"
    "    int iWindowCount;\n"
    "    int tomoe_reserved;\n"
    "    vec4 iTrail[64];\n"
    "    vec4 iData[48];\n"
    "    vec4 iWindow[8];\n"
    "    vec4 iSelection[4];\n"
    "};\n"
    "layout(set = 1, binding = 0) uniform sampler tomoe_sampler;\n"
    "layout(set = 2, binding = 0) uniform texture2D tomoe_beneath_image;\n"
    "layout(set = 3, binding = 0) uniform texture2D tomoe_marks_image;\n"
    "layout(set = 4, binding = 0) uniform texture2D tomoe_frost_image;\n"
    "#define tomoe_beneath sampler2D(tomoe_beneath_image, tomoe_sampler)\n"
    "#define tomoe_marks sampler2D(tomoe_marks_image, tomoe_sampler)\n"
    "#define tomoe_frost sampler2D(tomoe_frost_image, tomoe_sampler)\n"
    "vec4 beneath(vec2 at) {\n"
    "    return texture(tomoe_beneath, (at - tomoe_beneath_box.xy) * tomoe_beneath_box.zw);\n"
    "}\n"
    "vec4 frost(vec2 at) {\n"
    "    return texture(tomoe_frost, (at - tomoe_frost_box.xy) * tomoe_frost_box.zw);\n"
    "}\n"
    "vec4 markvec(vec2 at) {\n"
    "    if (iMarksBox.z <= 0.0 || iMarksBox.w <= 0.0) return vec4(0.0);\n"
    "    vec2 uv = vec2((at.x - iMarksBox.x) / iMarksBox.z,\n"
    "        (iMarksBox.y + iMarksBox.w - at.y) / iMarksBox.w);\n"
    "    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return vec4(0.0);\n"
    "    return texture(tomoe_marks, uv);\n"
    "}\n"
    "float marked(vec2 at) { return markvec(at).a; }\n"
    "layout(location = 5) in vec2 v_local;\n"
    "layout(location = 0) out vec4 tomoe_color;\n"
    "#line 1\n";

static const char tomoe_vk_main[] =
    "\nvoid main() {\n"
    "    mainImage(tomoe_color, vec2(v_local.x, iResolution.y - v_local.y));\n"
    "}\n";
#endif

enum tomoe_vk_member {
    TOMOE_VK_MOUSE = 0,
    TOMOE_VK_MARKS_BOX = 1,
    TOMOE_VK_BENEATH_BOX = 2,
    TOMOE_VK_FROST_BOX = 3,
    TOMOE_VK_RESOLUTION = 4,
    TOMOE_VK_TIME = 5,
    TOMOE_VK_FRAME = 6,
    TOMOE_VK_TRAIL_COUNT = 7,
    TOMOE_VK_WINDOW_COUNT = 8,
    TOMOE_VK_TRAIL = 10,
    TOMOE_VK_DATA = 11,
    TOMOE_VK_WINDOWS = 12,
    TOMOE_VK_SELECTION = 13,
};

#endif
