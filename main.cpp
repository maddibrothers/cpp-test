#include <SDL2/SDL.h>
#include <GLES3/gl3.h>
#include <emscripten.h>
#include <cmath>
#include <vector>

// --- 1. EMBEDDED SHADERS (WEBGL 2 / GLES 3.0) ---
const char* vertexShaderSource = R"(#version 300 es
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;

out vec3 vNormal;
out vec3 vFragPos;

void main() {
    vFragPos = vec3(uModel * vec4(aPos, 1.0));
    vNormal = mat3(transpose(inverse(uModel))) * aNormal;
    gl_Position = uProj * uView * vec4(vFragPos, 1.0);
}
)";

const char* fragmentShaderSource = R"(#version 300 es
precision highp float;

in vec3 vNormal;
in vec3 vFragPos;

uniform vec3 uColor;
uniform vec3 uLightPos;
uniform vec3 uViewPos;

out vec4 FragColor;

// ACES Filmic Tone Mapping for deep, rich contrast
vec3 ACESFilm(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 norm = normalize(vNormal);
    vec3 lightDir = normalize(uLightPos - vFragPos);
    vec3 viewDir = normalize(uViewPos - vFragPos);

    // Dual-Tone Ambient (Cool Sky + Warm Earth)
    vec3 skyColor = vec3(0.25, 0.35, 0.45);
    vec3 groundColor = vec3(0.12, 0.08, 0.06);
    vec3 ambient = mix(groundColor, skyColor, norm.y * 0.5 + 0.5) * 0.5;

    // Direct Noon Sunlight
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 sunColor = vec3(1.0, 0.95, 0.88) * 2.5;
    vec3 diffuse = diff * sunColor;

    // Specular Highlight
    vec3 halfDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(norm, halfDir), 0.0), 32.0);
    vec3 specular = vec3(0.2) * spec;

    vec3 linearColor = (ambient + diffuse + specular) * uColor;
    vec3 finalColor = pow(ACESFilm(linearColor), vec3(1.0 / 2.2));

    FragColor = vec4(finalColor, 1.0);
}
)";

// --- 2. ARENA DATA (OBJECTS & BOUNDARIES) ---
struct BoxObject {
    float x, y, z;
    float sx, sy, sz;
    float r, g, b;
};

const std::vector<BoxObject> arenaObjects = {
    // Ground Floor
    { 0.0f, -0.1f, 0.0f,  48.0f, 0.2f, 70.0f,  0.78f, 0.79f, 0.81f },
    
    // Boundary Walls (48m x 70m)
    { -24.0f, 1.4f,   0.0f,   1.0f, 2.8f, 72.0f,  0.88f, 0.89f, 0.90f },
    {  24.0f, 1.4f,   0.0f,   1.0f, 2.8f, 72.0f,  0.88f, 0.89f, 0.90f },
    {   0.0f, 1.4f, -35.0f,  50.0f, 2.8f,  1.0f,  0.88f, 0.89f, 0.90f },
    {   0.0f, 1.4f,  35.0f,  50.0f, 2.8f,  1.0f,  0.88f, 0.89f, 0.90f },
    
    // Shipping Containers
    { -11.0f, 1.3f, -16.0f,   2.44f, 2.6f, 12.0f,  0.49f, 0.17f, 0.14f }, // Red 40ft
    { -11.0f, 3.9f, -16.0f,   2.44f, 2.6f, 12.0f,  0.12f, 0.26f, 0.39f }, // Blue Stacked
    { -12.0f, 1.3f,  -3.0f,   2.44f, 2.6f,  6.0f,  0.16f, 0.32f, 0.25f }, // Green
    {  -9.0f, 1.3f,   7.0f,   2.44f, 2.6f,  6.0f,  0.62f, 0.32f, 0.12f }, // Orange
    { -11.5f, 1.3f,  15.0f,   2.44f, 2.6f, 12.0f,  0.27f, 0.29f, 0.31f }, // Grey 40ft
    {   0.0f, 1.3f,   0.0f,   2.44f, 2.6f, 12.0f,  0.12f, 0.26f, 0.39f }, // Center Container
    {  11.0f, 1.3f, -11.0f,   2.44f, 2.6f,  6.0f,  0.62f, 0.32f, 0.12f }, // East Orange
    {  10.5f, 1.3f,  13.0f,   2.44f, 2.6f,  6.0f,  0.16f, 0.32f, 0.25f }  // East Green
};

// --- 3. 4X4 MATRIX MATH ---
void mat4Identity(float* m) {
    for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}

void mat4Perspective(float* m, float fovY, float aspect, float zNear, float zFar) {
    float f = 1.0f / tanf(fovY * 0.5f);
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zFar + zNear) / (zNear - zFar);
    m[11] = -1.0f;
    m[14] = (2.0f * zFar * zNear) / (zNear - zFar);
}

void mat4LookAt(float* m, float eyeX, float eyeY, float eyeZ, float targetX, float targetY, float targetZ) {
    float fX = targetX - eyeX, fY = targetY - eyeY, fZ = targetZ - eyeZ;
    float rF = 1.0f / sqrtf(fX * fX + fY * fY + fZ * fZ);
    fX *= rF; fY *= rF; fZ *= rF;

    float sX = fY * 0.0f - fZ * 1.0f;
    float sY = 0.0f;
    float sZ = fX * 1.0f - 0.0f;
    float rS = 1.0f / sqrtf(sX * sX + sZ * sZ);
    sX *= rS; sZ *= rS;

    float uX = sZ * fY;
    float uY = sX * fZ - sZ * fX;
    float uZ = -sX * fY;

    mat4Identity(m);
    m[0] = sX;  m[4] = sY;  m[8]  = sZ;  m[12] = -(sX * eyeX + sY * eyeY + sZ * eyeZ);
    m[1] = uX;  m[5] = uY;  m[9]  = uZ;  m[13] = -(uX * eyeX + uY * eyeY + uZ * eyeZ);
    m[2] = -fX; m[6] = -fY; m[10] = -fZ; m[14] =  (fX * eyeX + fY * eyeY + fZ * eyeZ);
}

void mat4Model(float* m, float x, float y, float z, float sx, float sy, float sz) {
    mat4Identity(m);
    m[0] = sx; m[5] = sy; m[10] = sz;
    m[12] = x; m[13] = y; m[14] = z;
}

// --- 4. ENGINE STATE & GLOBALS ---
SDL_Window* window = nullptr;
GLuint shaderProgram = 0;
GLuint cubeVAO = 0, cubeVBO = 0;
GLint uModelLoc, uViewLoc, uProjLoc, uColorLoc, uLightPosLoc, uViewPosLoc;
float cameraAngle = 0.0f;

const float cubeVertices[] = {
    // Front face
    -0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
     0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
     0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
     0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
    -0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
    -0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
    // Back face
    -0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
     0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
     0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
     0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
    -0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
    -0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
    // Left face
    -0.5f,  0.5f,  0.5f, -1.0f,  0.0f,  0.0f,
    -0.5f,  0.5f, -0.5f, -1.0f,  0.0f,  0.0f,
    -0.5f, -0.5f, -0.5f, -1.0f,  0.0f,  0.0f,
    -0.5f, -0.5f, -0.5f, -1.0f,  0.0f,  0.0f,
    -0.5f, -0.5f,  0.5f, -1.0f,  0.0f,  0.0f,
    -0.5f,  0.5f,  0.5f, -1.0f,  0.0f,  0.0f,
    // Right face
     0.5f,  0.5f,  0.5f,  1.0f,  0.0f,  0.0f,
     0.5f, -0.5f, -0.5f,  1.0f,  0.0f,  0.0f,
     0.5f,  0.5f, -0.5f,  1.0f,  0.0f,  0.0f,
     0.5f, -0.5f, -0.5f,  1.0f,  0.0f,  0.0f,
     0.5f,  0.5f,  0.5f,  1.0f,  0.0f,  0.0f,
     0.5f, -0.5f,  0.5f,  1.0f,  0.0f,  0.0f,
    // Top face
    -0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,
     0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,
     0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,
     0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,
    -0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,
    -0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,
    // Bottom face
    -0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,
     0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,
     0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,
     0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,
    -0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,
    -0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f
};

void main_loop() {
    cameraAngle += 0.005f;
    float camX = sinf(cameraAngle) * 55.0f;
    float camZ = cosf(cameraAngle) * 55.0f;
    float camY = 32.0f;

    int width, height;
    SDL_GetWindowSize(window, &width, &height);
    glViewport(0, 0, width, height);

    glClearColor(0.55f, 0.71f, 0.86f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(shaderProgram);

    float proj[16], view[16], model[16];
    mat4Perspective(proj, 55.0f * (M_PI / 180.0f), (float)width / (float)height, 0.1f, 300.0f);
    mat4LookAt(view, camX, camY, camZ, 0.0f, 1.5f, 0.0f);

    glUniformMatrix4fv(uProjLoc, 1, GL_FALSE, proj);
    glUniformMatrix4fv(uViewLoc, 1, GL_FALSE, view);
    glUniform3f(uViewPosLoc, camX, camY, camZ);
    glUniform3f(uLightPosLoc, -36.0f, 17.0f, 24.0f);

    glBindVertexArray(cubeVAO);

    for (const auto& obj : arenaObjects) {
        mat4Model(model, obj.x, obj.y, obj.z, obj.sx, obj.sy, obj.sz);
        glUniformMatrix4fv(uModelLoc, 1, GL_FALSE, model);
        glUniform3f(uColorLoc, obj.r, obj.g, obj.b);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }

    SDL_GL_SwapWindow(window);
}

int main() {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    window = SDL_CreateWindow("Arena", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GL_CreateContext(window);

    glEnable(GL_DEPTH_TEST);

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertexShaderSource, nullptr);
    glCompileShader(vs);

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragmentShaderSource, nullptr);
    glCompileShader(fs);

    shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vs);
    glAttachShader(shaderProgram, fs);
    glLinkProgram(shaderProgram);

    uModelLoc = glGetUniformLocation(shaderProgram, "uModel");
    uViewLoc = glGetUniformLocation(shaderProgram, "uView");
    uProjLoc = glGetUniformLocation(shaderProgram, "uProj");
    uColorLoc = glGetUniformLocation(shaderProgram, "uColor");
    uLightPosLoc = glGetUniformLocation(shaderProgram, "uLightPos");
    uViewPosLoc = glGetUniformLocation(shaderProgram, "uViewPos");

    glGenVertexArrays(1, &cubeVAO);
    glGenBuffers(1, &cubeVBO);
    glBindVertexArray(cubeVAO);
    glBindBuffer(GL_ARRAY_BUFFER, cubeVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cubeVertices), cubeVertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    emscripten_set_main_loop(main_loop, 0, 1);
    return 0;
}
