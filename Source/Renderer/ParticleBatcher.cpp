/**
 * ParticleBatcher.cpp
 *
 * GPU-batched particle renderer using GL_POINTS with palette lookup.
 * Shaders are embedded as string literals (tiny, avoids file I/O).
 */

#include "ParticleBatcher.h"
#include "glad/gl.h"
#include "raylib/rlgl.h"

#include <cstring>
#include <algorithm>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using namespace RTE;

// ---------------------------------------------------------------------------
// Embedded shader sources
// ---------------------------------------------------------------------------

// GLSL ES 3.00 (WebGL 2) — used on Emscripten
static const char* s_VertSrcES = R"(#version 300 es
precision mediump float;
in vec2 aPosition;
in float aPaletteIndex;
uniform vec2 uScreenSize;
out float vPaletteIndex;
void main() {
    // Convert screen-space pixels to clip-space (-1..1)
    vec2 ndc = (aPosition / uScreenSize) * 2.0 - 1.0;
    // Flip Y: screen Y is top-down, GL clip Y is bottom-up
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
    gl_PointSize = 1.0;
    vPaletteIndex = aPaletteIndex;
}
)";

static const char* s_FragSrcES = R"(#version 300 es
precision mediump float;
in float vPaletteIndex;
uniform sampler2D uPalette;
out vec4 FragColor;
void main() {
    vec4 color = texture(uPalette, vec2((vPaletteIndex + 0.5) / 256.0, 0.5));
    if (color.a < 0.01) discard;
    FragColor = color;
}
)";

// GLSL 3.30 core — used on desktop
static const char* s_VertSrcGL = R"(#version 330 core
in vec2 aPosition;
in float aPaletteIndex;
uniform vec2 uScreenSize;
out float vPaletteIndex;
void main() {
    vec2 ndc = (aPosition / uScreenSize) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
    gl_PointSize = 1.0;
    vPaletteIndex = aPaletteIndex;
}
)";

static const char* s_FragSrcGL = R"(#version 330 core
in float vPaletteIndex;
uniform sampler2D uPalette;
out vec4 FragColor;
void main() {
    vec4 color = texture(uPalette, vec2((vPaletteIndex + 0.5) / 256.0, 0.5));
    if (color.a < 0.01) discard;
    FragColor = color;
}
)";

// ---------------------------------------------------------------------------
// Shader compilation helper
// ---------------------------------------------------------------------------

static GLuint CompileShader(GLenum type, const char* src) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, nullptr);
	glCompileShader(shader);
	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
#ifdef __EMSCRIPTEN__
		EM_ASM({ console.error('[ParticleBatcher] Shader compile error: ' + UTF8ToString($0)); }, log);
#endif
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static GLuint LinkProgram(GLuint vert, GLuint frag) {
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vert);
	glAttachShader(prog, frag);
	glLinkProgram(prog);
	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
#ifdef __EMSCRIPTEN__
		EM_ASM({ console.error('[ParticleBatcher] Program link error: ' + UTF8ToString($0)); }, log);
#endif
		glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

ParticleBatcher::~ParticleBatcher() {
	Destroy();
}

void ParticleBatcher::Initialize() {
	if (m_Initialized) return;

	// Select shader source based on platform
#ifdef __EMSCRIPTEN__
	const char* vertSrc = s_VertSrcES;
	const char* fragSrc = s_FragSrcES;
#else
	const char* vertSrc = s_VertSrcGL;
	const char* fragSrc = s_FragSrcGL;
#endif

	GLuint vert = CompileShader(GL_VERTEX_SHADER, vertSrc);
	GLuint frag = CompileShader(GL_FRAGMENT_SHADER, fragSrc);
	if (!vert || !frag) {
#ifdef __EMSCRIPTEN__
		EM_ASM({ console.error('[ParticleBatcher] Failed to compile shaders — falling back to CPU'); });
#endif
		if (vert) glDeleteShader(vert);
		if (frag) glDeleteShader(frag);
		return;
	}

	m_ShaderProgram = LinkProgram(vert, frag);
	glDeleteShader(vert);
	glDeleteShader(frag);
	if (!m_ShaderProgram) return;

	m_LocScreenSize = glGetUniformLocation(m_ShaderProgram, "uScreenSize");
	m_LocPalette = glGetUniformLocation(m_ShaderProgram, "uPalette");

	// Create VAO + VBO
	glGenVertexArrays(1, &m_VAO);
	glGenBuffers(1, &m_VBO);

	glBindVertexArray(m_VAO);
	glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
	// Pre-allocate buffer (will be orphaned each frame with glBufferData)
	glBufferData(GL_ARRAY_BUFFER, MAX_PARTICLES * sizeof(Vertex), nullptr, GL_DYNAMIC_DRAW);

	// Attribute 0: vec2 aPosition
	GLint locPos = glGetAttribLocation(m_ShaderProgram, "aPosition");
	glEnableVertexAttribArray(locPos);
	glVertexAttribPointer(locPos, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
	                      reinterpret_cast<void*>(offsetof(Vertex, x)));

	// Attribute 1: float aPaletteIndex
	GLint locPal = glGetAttribLocation(m_ShaderProgram, "aPaletteIndex");
	glEnableVertexAttribArray(locPal);
	glVertexAttribPointer(locPal, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex),
	                      reinterpret_cast<void*>(offsetof(Vertex, paletteIndex)));

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	m_Vertices.resize(MAX_PARTICLES);
	m_VertexCount = 0;
	m_Initialized = true;

#ifdef __EMSCRIPTEN__
	EM_ASM({ console.log('[ParticleBatcher] Initialized — max ' + $0 + ' particles per batch'); },
	       MAX_PARTICLES);
#endif
}

void ParticleBatcher::Destroy() {
	if (m_ShaderProgram) { glDeleteProgram(m_ShaderProgram); m_ShaderProgram = 0; }
	if (m_VBO) { glDeleteBuffers(1, &m_VBO); m_VBO = 0; }
	if (m_VAO) { glDeleteVertexArrays(1, &m_VAO); m_VAO = 0; }
	m_Initialized = false;
	m_VertexCount = 0;
}

// ---------------------------------------------------------------------------
// Particle collection
// ---------------------------------------------------------------------------

void ParticleBatcher::Add(float screenX, float screenY, float paletteIndex) {
	if (m_VertexCount >= MAX_PARTICLES) return;
	m_Vertices[m_VertexCount++] = {screenX, screenY, paletteIndex};
}

void ParticleBatcher::AddBulk(const float* worldX, const float* worldY,
                               const unsigned char* paletteIndices,
                               const uint32_t* flags, uint32_t zombieFlag,
                               int32_t count, float camX, float camY,
                               int screenW, int screenH) {
	for (int32_t i = 0; i < count && m_VertexCount < MAX_PARTICLES; ++i) {
		if (flags[i] & zombieFlag) continue;

		float sx = worldX[i] - camX;
		float sy = worldY[i] - camY;

		if (sx < 0 || sy < 0 || sx >= screenW || sy >= screenH) continue;

		m_Vertices[m_VertexCount++] = {sx, sy, static_cast<float>(paletteIndices[i])};
	}
}

// ---------------------------------------------------------------------------
// GPU draw
// ---------------------------------------------------------------------------

void ParticleBatcher::Flush(unsigned int paletteTexture, int screenW, int screenH) {
	if (!m_Initialized || m_VertexCount == 0) return;

	// Save rlgl state that we're about to override
	// (rlgl flushes its batch when we bind our own shader/VAO)
	rlDrawRenderBatchActive();

	// Upload vertex data
	glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
	glBufferSubData(GL_ARRAY_BUFFER, 0, m_VertexCount * sizeof(Vertex), m_Vertices.data());

	// Bind our shader + VAO
	glUseProgram(m_ShaderProgram);
	glUniform2f(m_LocScreenSize, static_cast<float>(screenW), static_cast<float>(screenH));

	// Bind palette texture to unit 0
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, paletteTexture);
	glUniform1i(m_LocPalette, 0);

	glBindVertexArray(m_VAO);
	glDrawArrays(GL_POINTS, 0, m_VertexCount);
	glBindVertexArray(0);

	// Restore rlgl's shader (important — rlgl tracks active shader internally)
	rlSetShader(rlGetShaderIdDefault(), rlGetShaderLocsDefault());

	glBindBuffer(GL_ARRAY_BUFFER, 0);
}
