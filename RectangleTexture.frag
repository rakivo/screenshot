#version 330

in vec2 fragTexCoord;
out vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 rectParams;
uniform vec2 renderSize;
uniform float smoothness;

void main()
{
    vec2 texPos = fragTexCoord * renderSize;

    if (texPos.x >= rectParams.x && texPos.x <= rectParams.x + rectParams.z &&
        texPos.y >= rectParams.y && texPos.y <= rectParams.y + rectParams.w)
    {
        fragColor = texture(texture0, fragTexCoord);
    }
    else
    {
        fragColor = vec4(0.0, 0.0, 0.0, 0.0);
    }
}
