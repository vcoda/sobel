#version 450

layout(location = 0) in vec3 normal;
layout(location = 0) out vec4 oColor;

void main()
{
    oColor = vec4(1., 0., 0., 0.);
}
