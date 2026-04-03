/**
 * ParticleBatcher.h
 *
 * GPU-batched particle renderer. Collects particle positions and palette
 * indices into a VBO, draws them all as GL_POINTS in a single draw call.
 *
 * Replaces thousands of CPU putpixel() calls with one GPU draw call.
 * Uses the same palette texture as the Blit8 shader for color lookup.
 */

#pragma once

#include <vector>
#include <cstdint>

namespace RTE {

class ParticleBatcher {

public:
	ParticleBatcher() = default;
	~ParticleBatcher();

	/// Initialize GL resources (VAO, VBO, shader). Call after GL context is ready.
	void Initialize();

	/// Destroy GL resources.
	void Destroy();

	/// Reset for a new frame.
	void Clear() { m_VertexCount = 0; }

	/// Add a single particle to the batch.
	/// @param screenX, screenY  Screen-space position (pixels, after camera offset).
	/// @param paletteIndex      8bpp palette color index (0-255).
	void Add(float screenX, float screenY, float paletteIndex);

	/// Add particles in bulk from SoA arrays (for FluidManager).
	/// Applies camera offset and screen bounds clipping.
	void AddBulk(const float* worldX, const float* worldY,
	             const unsigned char* paletteIndices,
	             const uint32_t* flags, uint32_t zombieFlag,
	             int32_t count, float camX, float camY, int screenW, int screenH);

	/// Upload VBO and draw all collected particles as GL_POINTS.
	/// Must be called while the target FBO is bound.
	/// @param paletteTexture  GL texture ID of the 256x1 palette LUT.
	/// @param screenW, screenH  Backbuffer dimensions for orthographic projection.
	void Flush(unsigned int paletteTexture, int screenW, int screenH);

	/// How many particles are in the current batch.
	int GetVertexCount() const { return m_VertexCount; }

	/// Is the batcher initialized and ready?
	bool IsReady() const { return m_Initialized; }

private:
	struct Vertex {
		float x, y;
		float paletteIndex;
	};

	bool m_Initialized = false;
	unsigned int m_VAO = 0;
	unsigned int m_VBO = 0;
	unsigned int m_ShaderProgram = 0;
	int m_LocScreenSize = -1;
	int m_LocPalette = -1;

	std::vector<Vertex> m_Vertices;
	int m_VertexCount = 0;
	static constexpr int MAX_PARTICLES = 30000;
};

} // namespace RTE
