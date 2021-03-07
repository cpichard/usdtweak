#version 330 core
in vec4 color;
out vec4 FragColor;
uniform vec3 highlight;
void main()
{
    if (dot(highlight, color.xyz) >0.9) 
       { FragColor = vec4(1.0, 1.0, 0.2, 1.0);}
  else {  FragColor = color; }
};
