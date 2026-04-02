/**
 * FluidManager.h
 *
 * Singleton manager bridging the FluidSolver SPH engine to
 * Cortex Command's terrain, Box2D rigid bodies, and rendering.
 */

#pragma once

#include "Singleton.h"
#include "FluidSolver.h"

#define g_FluidMan FluidManager::Instance()

struct BITMAP; // Forward declare (Allegro compat)

namespace RTE {

class Vector;

class FluidManager : public Singleton<FluidManager> {

public:
	FluidManager() { Clear(); }
	~FluidManager() { Destroy(); }

	int Create() { return 0; }
	void Initialize();
	void Destroy();
	void Reset();

	/// Main simulation step. Called from MovableMan::Travel().
	void Step(float deltaTime);

	/// Draw fluid particles to the visual backbuffer.
	void Draw(BITMAP* targetBitmap, const Vector& cameraOffset);

	/// Draw fluid as material pixels (for terrain settling layer).
	void DrawMatter(BITMAP* targetBitmap, const Vector& cameraOffset);

	/// Draw debug visualization (density heatmap, velocity vectors).
	void DrawDebug(BITMAP* targetBitmap, const Vector& cameraOffset);

	/// Spawn fluid particles at a point.
	/// @param px, py   World position in pixels.
	/// @param vx, vy   Initial velocity in px/s.
	/// @param materialIndex CC material index (e.g. 160 for Water).
	/// @param count     Number of particles to spawn.
	void SpawnFluid(float px, float py, float vx, float vy,
	                unsigned char materialIndex, int count = 1);

	/// Spawn a rectangular volume of evenly-spaced fluid particles.
	void SpawnFluidRect(float x, float y, float w, float h,
	                    unsigned char materialIndex);

	int GetParticleCount() const { return m_Solver.GetParticleCount(); }
	void SetEnabled(bool enabled) { m_Enabled = enabled; }
	bool IsEnabled() const { return m_Enabled; }
	void SetDebugDraw(bool enable) { m_DebugDraw = enable; }
	bool IsDebugDrawEnabled() const { return m_DebugDraw; }

private:
	FluidSolver m_Solver;
	bool m_Enabled = false;
	bool m_DebugDraw = false;

	/// CC material index per fluid particle (parallel to solver arrays).
	std::vector<unsigned char> m_MaterialIndex;

	/// Rest frame counter per particle for settling detection.
	std::vector<int> m_RestFrames;
	static constexpr int SETTLE_THRESHOLD = 60; // Frames at rest before settling
	static constexpr float REST_VEL_THRESHOLD = 0.5f; // px/s

	void Clear();

	/// Process settled particles (convert to terrain pixels).
	void ProcessSettling();

	/// Apply fluid→body impulses to Box2D.
	void ApplyBodyImpulses();

	// Callbacks wired to the FluidSolver
	static bool TerrainQueryCallback(int x, int y);
	static void BodyQueryCallback(float minX, float minY, float maxX, float maxY,
	                               std::vector<FluidBodyContact>& contacts);
};

} // namespace RTE
