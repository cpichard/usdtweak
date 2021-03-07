#version 330 core
// First approach was to draw circles on a quad using fragment shader, fun, but not the best solution
// TODO: change to drawing lines as this really doesn't look nice
in vec4 color;
in vec2 uv;
uniform vec3 highlight;
out vec4 FragColor;
void main()
{
    float alpha = abs(1.f-sqrt(uv.x*uv.x+uv.y*uv.y));
    float derivative = length(fwidth(uv));
    if (derivative > 0.1) discard;
    alpha = smoothstep(2.0*derivative, 0.0, alpha);
    if (alpha < 0.2) discard;
    if (dot(highlight,color.xyz) >0.9) { FragColor = vec4(1.0, 1.0, 0.2, alpha);}
    else {FragColor = vec4(color.xyz, alpha);}
}
