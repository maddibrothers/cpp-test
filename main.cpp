#include <SDL2/SDL.h>
#include <GLES3/gl3.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <cmath>
#include <vector>

// ============================================================================
// 1. PROCEDURAL PBR SHADER (CORRUGATED STEEL, PLANKS, SLABS & SHADOWS)
// ============================================================================
const char* vertexShaderSource = R"(#version 300 es
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;

out vec3 vNormal;
out vec3 vFragPos;
out vec3 vLocalPos;

void main() {
    vLocalPos = aPos;
    vFragPos = vec3(uModel * vec4(aPos, 1.0));
    vNormal = mat3(transpose(inverse(uModel))) * aNormal;
    gl_Position = uProj * uView * vec4(vFragPos, 1.0);
}
)";

const char* fragmentShaderSource = R"(#version 300 es
precision highp float;

in vec3 vNormal;
in vec3 vFragPos;
in vec3 vLocalPos;

uniform vec3 uColor;
uniform vec3 uLightPos;
uniform vec3 uViewPos;
uniform int  uMatType; // 0=Ground/Asphalt, 1=Container (Ribbed), 2=Crate (Planks), 3=Wall/Barrier

out vec4 FragColor;

float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// ACES Filmic Tone Mapping for deep contrast and filmic saturation
vec3 ACESFilm(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 N = normalize(vNormal);
    vec3 lightDir = normalize(uLightPos - vFragPos);
    vec3 viewDir = normalize(uViewPos - vFragPos);

    vec3 albedo = uColor;
    float roughness = 0.65;
    float metallic = 0.1;
    float ao = 1.0;

    // --- MATERIAL 0: CONCRETE FLOOR & ASPHALT ROAD ---
    if (uMatType == 0) {
        vec2 grid = abs(fract(vFragPos.xz * 0.166) - 0.5);
        float seam = smoothstep(0.46, 0.50, max(grid.x, grid.y));
        albedo = mix(albedo, vec3(0.18, 0.18, 0.20), seam * 0.85);

        float grain = hash(floor(vFragPos.xz * 35.0)) * 0.12;
        albedo += vec3(grain - 0.06);

        if (abs(vFragPos.x) < 0.28 && abs(vFragPos.z) < 26.0) {
            float dash = step(0.4, fract(vFragPos.z * 0.4));
            albedo = mix(albedo, vec3(0.95, 0.72, 0.10), dash * 0.9);
        }

        ao = clamp((vFragPos.y + 0.15) * 4.0, 0.45, 1.0);
        roughness = 0.88;
    }
    // --- MATERIAL 1: CORRUGATED SHIPPING CONTAINERS ---
    else if (uMatType == 1) {
        if (abs(N.y) < 0.4) {
            float ribCoord = (abs(N.x) > 0.5) ? vFragPos.z : vFragPos.x;
            float rib = sin(ribCoord * 14.0);

            vec3 ribNormalPerturb = vec3(0.0);
            if (abs(N.x) > 0.5) {
                ribNormalPerturb = vec3(0.0, 0.0, cos(ribCoord * 14.0) * 0.75);
            } else {
                ribNormalPerturb = vec3(cos(ribCoord * 14.0) * 0.75, 0.0, 0.0);
            }
            N = normalize(N + ribNormalPerturb);

            float ribShadow = smoothstep(-0.8, 0.4, rib);
            albedo *= mix(0.55, 1.15, ribShadow);

            float groundDirt = clamp(vFragPos.y / 1.2, 0.0, 1.0);
            albedo = mix(vec3(0.18, 0.14, 0.10), albedo, smoothstep(0.0, 0.85, groundDirt));
        } else {
            float roofRib = sin(vFragPos.z * 10.0);
            albedo *= (0.8 + 0.2 * step(0.0, roofRib));
        }

        ao = clamp(vFragPos.y * 1.5, 0.55, 1.0);
        metallic = 0.45;
        roughness = 0.40;
    }
    // --- MATERIAL 2: WOODEN CRATES WITH METAL BRACKETS ---
    else if (uMatType == 2) {
        vec3 p = abs(vLocalPos);
        bool isCorner = (p.x > 0.42 && p.y > 0.42) || (p.x > 0.42 && p.z > 0.42) || (p.y > 0.42 && p.z > 0.42);

        if (isCorner) {
            albedo = vec3(0.22, 0.23, 0.25);
            metallic = 0.6;
            roughness = 0.35;
        } else {
            float plank = fract(vLocalPos.y * 5.0);
            float seam = smoothstep(0.0, 0.08, plank) * smoothstep(1.0, 0.92, plank);
            albedo *= mix(0.65, 1.08, seam);
            roughness = 0.82;
        }
    }
    // --- MATERIAL 3: PERIMETER WALLS & BARRIERS ---
    else if (uMatType == 3) {
        float concreteGrain = hash(floor(vFragPos.xy * 8.0)) * 0.15;
        albedo += vec3(concreteGrain - 0.075);
        ao = clamp(vFragPos.y / 2.0, 0.6, 1.0);
        roughness = 0.92;
    }

    // --- PBR LIGHTING ENGINE ---
    vec3 skyLight = vec3(0.22, 0.30, 0.42);
    vec3 groundBounce = vec3(0.14, 0.09, 0.06);
    vec3 ambient = mix(groundBounce, skyLight, N.y * 0.5 + 0.5) * 0.70 * ao;

    float NdotL = max(dot(N, lightDir), 0.0);
    vec3 sunColor = vec3(1.0, 0.96, 0.90) * 2.8;
    vec3 diffuse = NdotL * sunColor;

    vec3 halfDir = normalize(lightDir + viewDir);
    float specFactor = pow(max(dot(N, halfDir), 0.0), mix(64.0, 8.0, roughness));
    vec3 specColor = mix(vec3(0.04), albedo, metallic);
    vec3 specular = specColor * specFactor * (1.0 - roughness);

    vec3 directLight = (diffuse + specular);
    vec3 linearColor = (ambient * albedo) + (directLight * albedo);

    vec3 finalColor = pow(ACESFilm(linearColor), vec3(1.0 / 2.2));
    FragColor = vec4(finalColor, 1.0);
}
)";

// ============================================================================
// 2. ARENA OBJECT LAYOUT
// ============================================================================
struct MapObject {
    float x, y, z;
    float sx, sy, sz;
    float r, g, b;
    int   matType;
};

const std::vector<MapObject> arenaObjects = {
    // Ground Floor
    { 0.0f, -0.1f, 0.0f,  48.0f, 0.2f, 70.0f,  0.45f, 0.46f, 0.48f, 0 },

    // Boundary Concrete Walls
    { -24.0f, 1.4f,   0.0f,   1.0f, 2.8f, 72.0f,  0.82f, 0.83f, 0.85f, 3 },
    {  24.0f, 1.4f,   0.0f,   1.0f, 2.8f, 72.0f,  0.82f, 0.83f, 0.85f, 3 },
    {   0.0f, 1.4f, -35.0f,  50.0f, 2.8f,  1.0f,  0.82f, 0.83f, 0.85f, 3 },
    {   0.0f, 1.4f,  35.0f,  50.0f, 2.8f,  1.0f,  0.82f, 0.83f, 0.85f, 3 },

    // Shipping Containers
    { -11.0f, 1.3f, -16.0f,   2.44f, 2.6f, 12.0f,  0.72f, 0.16f, 0.12f, 1 }, // Red 40ft
    { -11.0f, 3.9f, -16.0f,   2.44f, 2.6f, 12.0f,  0.10f, 0.32f, 0.58f, 1 }, // Navy Stacked
    { -12.0f, 1.3f,  -3.0f,   2.44f, 2.6f,  6.0f,  0.14f, 0.42f, 0.28f, 1 }, // Tactical Green
    {  -9.0f, 1.3f,   7.0f,   2.44f, 2.6f,  6.0f,  0.85f, 0.42f, 0.10f, 1 }, // Industrial Orange
    { -11.5f, 1.3f,  15.0f,   2.44f, 2.6f, 12.0f,  0.35f, 0.37f, 0.40f, 1 }, // Dark Grey 40ft
    {   0.0f, 1.3f,   0.0f,   2.44f, 2.6f, 12.0f,  0.10f, 0.32f, 0.58f, 1 }, // Center Blue
    {  11.0f, 1.3f, -11.0f,   2.44f, 2.6f,  6.0f,  0.85f, 0.42f, 0.10f, 1 }, // East Orange
    {  10.5f, 1.3f,  13.0f,   2.44f, 2.6f,  6.0f,  0.14f, 0.42f, 0.28f, 1 }, // East Green

    // Military Wooden Crates
    {  7.5f, 0.7f, -14.0f,   1.4f, 1.4f, 1.4f,    0.58f, 0.42f, 0.26f, 2 },
    {  8.9f, 0.7f, -14.4f,   1.4f, 1.4f, 1.4f,    0.58f, 0.42f, 0.26f, 2 },
    {  8.2f, 2.1f, -14.2f,   1.4f, 1.4f, 1.4f,    0.58f, 0.42f, 0.26f, 2 },
    { -5.0f, 0.7f,  12.0f,   1.4f, 1.4f, 1.4f,    0.58f, 0.42f, 0.26f, 2 },

    // Concrete Jersey Barriers
    { -5.5f, 0.57f,  18.0f,   0.7f, 1.15f, 3.6f,  0.68f, 0.67f, 0.65f, 3 },
    {  5.5f, 0.57f,  18.0f,   0.7f, 1.15f, 3.6f,  0.68f, 0.67f, 0.65f, 3 },
    { -5.5f, 0.57f, -18.0f,   0.7f, 1.15f, 3.6f,  0.68f, 0.67f, 0.65f, 3 },
    {  5.5f, 0.57f, -18.0f,   0.7f, 1.15f, 3.6f,  0.68f, 0.67f, 0.65f, 3 }
};

// ============================================================================
// 3. CORRECT COLUMN-MAJOR 3D MATRIX MATH
// ============================================================================
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

// Correct Right-Handed LookAt: Right = F x Up, Up = Right x F
void mat4LookAt(float* m, float eyeX, float eyeY, float eyeZ, float targetX, float targetY, float targetZ) {
    float fX = targetX - eyeX, fY = targetY - eyeY, fZ = targetZ - eyeZ;
    float rF = 1.0f / sqrtf(fX * fX + fY * fY + fZ * fZ);
    fX *= rF; fY *= rF; fZ *= rF;

    // Right = normalize(F x (0,1,0)) -> (-fZ, 0, fX)
    float sX = -fZ;
    float sY = 0.0f;
    float sZ = fX;
    float rS = 1.0f / sqrtf(sX * sX + sZ * sZ);
    sX *= rS; sZ *= rS;

    // Up = s x f
    float uX = -sZ * fY;
    float uY = sZ * fX - sX * fZ;
    float uZ = sX * fY;

    mat4Identity(m);
    m[0] = sX;  m[4] = sY;  m[8]  = sZ;  m[12] = -(sX * eyeX + sY * eyeY + sZ * eyeZ);
    m[1] = uX;  m[5] = uY;  m[9]  = uZ;  m[13] = -(uX * eyeX + uY * eyeY + uZ * eyeZ);
    m[2] = -fX; m[6] = -fY; m[10] = -fZ; m[14] =  (fX * eyeX + fY * eyeY + fZ * eyeZ);
    m[3] = 0.0f; m[7] = 0.0f; m[11] = 0.0f; m[15] = 1.0f;
}

void mat4Model(float* m, float x, float y, float z, float sx, float sy, float sz) {
    mat4Identity(m);
    m[0] = sx; m[5] = sy; m[10] = sz;
    m[12] = x; m[13] = y; m[14] = z;
}

// ============================================================================
// 4. INTERACTIVE ORBIT CAMERA CONTROLS (TOUCH & MOUSE)
// ============================================================================
float camYaw = 0.85f;
float camPitch = 0.52f;
float camDist = 58.0f;

bool isMouseDown = false;
int lastMouseX = 0, lastMouseY = 0;

static float touchStartDist = 0.0f;
static float lastTouchX = 0.0f;
static float lastTouchY = 0.0f;
static bool isTouching = false;

EM_BOOL on_touch_start(int eventType, const EmscriptenTouchEvent *e, void *userData) {
    if (e->numTouches == 1) {
        lastTouchX = e->touches[0].targetX;
        lastTouchY = e->touches[0].targetY;
        isTouching = true;
    } else if (e->numTouches >= 2) {
        float dx = e->touches[0].targetX - e->touches[1].targetX;
        float dy = e->touches[0].targetY - e->touches[1].targetY;
        touchStartDist = sqrtf(dx * dx + dy * dy);
    }
    return EM_TRUE;
}

EM_BOOL on_touch_move(int eventType, const EmscriptenTouchEvent *e, void *userData) {
    if (e->numTouches == 1 && isTouching) {
        float dx = e->touches[0].targetX - lastTouchX;
        float dy = e->touches[0].targetY - lastTouchY;
        camYaw   -= dx * 0.007f;
        camPitch += dy * 0.007f;

        // Clamp elevation so camera stays upright and never flips
        if (camPitch < 0.08f) camPitch = 0.08f;
        if (camPitch > 1.45f) camPitch = 1.45f;

        lastTouchX = e->touches[0].targetX;
        lastTouchY = e->touches[0].targetY;
    } else if (e->numTouches >= 2) {
        float dx = e->touches[0].targetX - e->touches[1].targetX;
        float dy = e->touches[0].targetY - e->touches[1].targetY;
        float dist = sqrtf(dx * dx + dy * dy);
        float diff = dist - touchStartDist;

        camDist -= diff * 0.15f;
        if (camDist < 12.0f) camDist = 12.0f;
        if (camDist > 120.0f) camDist = 120.0f;

        touchStartDist = dist;
    }
    return EM_TRUE;
}

EM_BOOL on_touch_end(int eventType, const EmscriptenTouchEvent *e, void *userData) {
    if (e->numTouches == 0) isTouching = false;
    return EM_TRUE;
}

// ============================================================================
// 5. ENGINE STATE & MAIN LOOP
// ============================================================================
SDL_Window* window = nullptr;
GLuint shaderProgram = 0;
GLuint cubeVAO = 0, cubeVBO = 0;
GLint uModelLoc, uViewLoc, uProjLoc, uColorLoc, uLightPosLoc, uViewPosLoc, uMatTypeLoc;

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
    // Process Mouse events for desktop browsers
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
            isMouseDown = true;
            lastMouseX = ev.button.x; lastMouseY = ev.button.y;
        } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
            isMouseDown = false;
        } else if (ev.type == SDL_MOUSEMOTION && isMouseDown && !isTouching) {
            camYaw   -= (ev.motion.x - lastMouseX) * 0.007f;
            camPitch += (ev.motion.y - lastMouseY) * 0.007f;
            if (camPitch < 0.08f) camPitch = 0.08f;
            if (camPitch > 1.45f) camPitch = 1.45f;
            lastMouseX = ev.motion.x; lastMouseY = ev.motion.y;
        } else if (ev.type == SDL_MOUSEWHEEL) {
            camDist -= ev.wheel.y * 3.5f;
            if (camDist < 12.0f) camDist = 12.0f;
            if (camDist > 120.0f) camDist = 120.0f;
        }
    }

    // Auto-sync canvas resolution to mobile viewport
    double cssWidth, cssHeight;
    emscripten_get_element_css_size("#canvas", &cssWidth, &cssHeight);
    int width = (int)cssWidth;
    int height = (int)cssHeight;
    if (width <= 0) width = 1280;
    if (height <= 0) height = 720;
    emscripten_set_canvas_element_size("#canvas", width, height);
    glViewport(0, 0, width, height);

    // Deep tactical atmosphere clear
    glClearColor(0.08f, 0.11f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(shaderProgram);

    // Spherical Orbit calculation
    float eyeX = sinf(camYaw) * cosf(camPitch) * camDist;
    float eyeY = 1.2f + sinf(camPitch) * camDist;
    float eyeZ = cosf(camYaw) * cosf(camPitch) * camDist;

    float proj[16], view[16], model[16];
    float aspect = (float)width / (float)height;
    mat4Perspective(proj, 50.0f * (M_PI / 180.0f), aspect, 0.2f, 350.0f);
    mat4LookAt(view, eyeX, eyeY, eyeZ, 0.0f, 1.2f, 0.0f);

    glUniformMatrix4fv(uProjLoc, 1, GL_FALSE, proj);
    glUniformMatrix4fv(uViewLoc, 1, GL_FALSE, view);
    glUniform3f(uViewPosLoc, eyeX, eyeY, eyeZ);
    glUniform3f(uLightPosLoc, -34.0f, 24.0f, 26.0f);

    glBindVertexArray(cubeVAO);

    for (const auto& obj : arenaObjects) {
        mat4Model(model, obj.x, obj.y, obj.z, obj.sx, obj.sy, obj.sz);
        glUniformMatrix4fv(uModelLoc, 1, GL_FALSE, model);
        glUniform3f(uColorLoc, obj.r, obj.g, obj.b);
        glUniform1i(uMatTypeLoc, obj.matType);
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

    // Register mobile touch listeners
    emscripten_set_touchstart_callback("#canvas", nullptr, EM_TRUE, on_touch_start);
    emscripten_set_touchmove_callback("#canvas", nullptr, EM_TRUE, on_touch_move);
    emscripten_set_touchend_callback("#canvas", nullptr, EM_TRUE, on_touch_end);
    emscripten_set_touchcancel_callback("#canvas", nullptr, EM_TRUE, on_touch_end);

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

    uModelLoc   = glGetUniformLocation(shaderProgram, "uModel");
    uViewLoc    = glGetUniformLocation(shaderProgram, "uView");
    uProjLoc    = glGetUniformLocation(shaderProgram, "uProj");
    uColorLoc   = glGetUniformLocation(shaderProgram, "uColor");
    uLightPosLoc= glGetUniformLocation(shaderProgram, "uLightPos");
    uViewPosLoc = glGetUniformLocation(shaderProgram, "uViewPos");
    uMatTypeLoc = glGetUniformLocation(shaderProgram, "uMatType");

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
