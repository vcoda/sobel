#version 450
#define SCREEN_SIZE vec2(1280., 720.)

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D mask;

// https://www.shadertoy.com/view/MdGGWh
float Sobel(sampler2D s, vec2 uv)
{
    mat3 Gx = mat3(-1., 0., 1.,
                   -2., 0., 2.,
                   -1., 0., 1.);

    mat3 Gy = mat3(-1., -2., -1.,
                    0.,  0.,  0.,
                    1.,  2.,  1.);

    vec2 invScreenSize = vec2(1.) / SCREEN_SIZE;
    vec2 grad = vec2(0.);

    for (int i = 0; i < 3; ++i) 
    {
        for (int j = 0; j < 3; ++j)
        {
            vec2 offset = vec2(i, j) * invScreenSize;
            float lum = texture(s, uv + offset).r;
            grad += vec2(Gx[i][j], Gy[i][j]) * lum;
        }
    }

    return length(grad);
}

float Scharr(sampler2D s, vec2 uv)
{
    mat3 Gx = mat3( 3., 0.,  -3.,
                   10., 0., -10.,
                    3., 0.,  -3.);

    mat3 Gy = mat3( 3.,  10.,  3.,
                    0.,   0.,  0.,
                   -3., -10., -3.);

    vec2 invScreenSize = vec2(1.) / SCREEN_SIZE;
    vec2 grad = vec2(0.);
    
    for (int i = 0; i < 3; ++i) 
    {
        for (int j = 0; j < 3; ++j)
        {
            vec2 offset = vec2(i, j) * invScreenSize;
            float lum = texture(s, uv + offset).r;
            grad += vec2(Gx[i][j], Gy[i][j]) * lum;
        }
    }

    return length(grad);
}

void main()
{
    float grad = Sobel(mask, texCoord);
    oColor = vec4(vec3(grad), 1.);
}
