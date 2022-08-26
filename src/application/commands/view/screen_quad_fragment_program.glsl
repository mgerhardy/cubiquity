#version 330 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D screenTexture;

void main()
{
    vec4 col = texture(screenTexture, TexCoords);
    FragColor = vec4(col.rgb / col.a, 1.0);
	//FragColor = vec4(col.rgb, 0.0);
} 