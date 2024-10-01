// Stolen from: <https://github.com/NSinecode/Raylib-Drawing-texture-in-circle/blob/master/CircleTexture.frag>

#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

uniform vec2 center;
uniform float radius;
uniform float smoothness;
uniform vec2 renderSize;

out vec4 finalColor;

void main()
{
    vec2 NNfragTexCoord = fragTexCoord * renderSize;
    float L = length(center - NNfragTexCoord);

    float edgeThreshold = radius; 
    float alpha = smoothstep(edgeThreshold - smoothness, edgeThreshold, L);

    if (L <= radius) {
        finalColor = texture(texture0, fragTexCoord) * fragColor;
        finalColor.a *= (1.0 - alpha);
    } else {
        finalColor = vec4(0.0);
    }
}
