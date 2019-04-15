#version 450

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D s0;

void main()
{
    oColor = texture(s0, texCoord);
    if (length(oColor.rgb) > 0.)
        oColor.rgb = vec3(1.0, 0.0, 0.0);
}
