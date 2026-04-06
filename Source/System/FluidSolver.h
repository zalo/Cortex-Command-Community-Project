/**
 * FluidSolver.h
 *
 * Standalone SPH (Smoothed Particle Hydrodynamics) fluid solver.
 * Extracted from LiquidFun's algorithm, adapted for Cortex Command.
 *
 * This module has ZERO dependencies on Box2D or CC engine code.
 * All external queries (terrain, rigid bodies) are done via callbacks.
 * Coordinates are in pixel space (matching CC's convention).
 */

#pragma once

#include <cstdint>
#include <vector>
#include <functional>

namespace RTE {

/// Behavior flags for fluid particles (can be combined).
enum FluidParticleFlag : uint32_t {
	FPF_Water         = 0,         // Default water behavior (pressure + damping)
	FPF_Viscous       = 1 << 0,   // Viscous drag between neighbors
	FPF_Tensile       = 1 << 1,   // Surface tension
	FPF_Powder        = 1 << 2,   // Granular/powder (repulsion, no pressure)
	FPF_ColorMixing   = 1 << 3,   // Blend colors at contacts
	FPF_Wall          = 1 << 4,   // Static wall particle (velocity zeroed)
	FPF_Zombie        = 1 << 5,   // Marked for removal
};

/// Contact between two fluid particles.
struct FluidContact {
	int32_t indexA;
	int32_t indexB;
	float weight;   // Overlap weight 0..1 (1 = fully overlapping)
	float nx, ny;   // Unit normal from A → B
};

/// Contact between a fluid particle and an external rigid body.
struct FluidBodyContact {
	int32_t particleIndex;
	int64_t bodyUserData;   // Opaque handle (Box2D bodyId bits)
	float weight;           // Contact weight 0..1
	float nx, ny;           // Normal pointing into the body surface
	float bodyMass;         // Body mass for impulse scaling
	float bodyVelX, bodyVelY; // Body velocity at contact point
};

/// Impulse to apply to an external rigid body after the solver step.
struct FluidBodyImpulse {
	int64_t bodyUserData;
	float impulseX, impulseY;
	float pointX, pointY;   // World point of application (pixels)
};

class FluidSolver {

public:
	/// Tunable solver configuration.
	struct Config {
		float particleRadius    = 3.0f;    // Pixel radius of each particle
		float particleStride    = 0.75f;   // Spacing = stride * diameter
		float gravityX          = 0.0f;
		float gravityY          = 0.0f;    // Set from CC's global acceleration
		float pressureStrength  = 0.05f;   // How strongly particles push apart
		float dampingStrength   = 1.0f;    // Contact velocity damping
		float viscousStrength   = 0.25f;   // Viscous velocity matching
		float restitution       = 0.3f;    // Terrain bounce factor
		float friction          = 0.05f;   // Terrain friction factor
		int   maxParticles      = 4096;
		int   subSteps          = 2;       // Sub-steps per Step() call
	};

	// Callback types for external queries
	using TerrainQueryFn = std::function<bool(int x, int y)>;
	using BodyQueryFn = std::function<void(float minX, float minY, float maxX, float maxY,
	                                       std::vector<FluidBodyContact>& contacts)>;

	FluidSolver() = default;
	~FluidSolver() = default;

	void Initialize(const Config& config);
	void Destroy();

	/// Add a particle. Returns its index, or -1 if at capacity.
	int32_t AddParticle(float px, float py, float vx, float vy,
	                    uint32_t flags, uint32_t packedColor);

	/// Mark a particle for removal (compacted at end of step).
	void RemoveParticle(int32_t index);

	/// Main simulation step.
	void Step(float dt);

	/// Set external query callbacks.
	void SetTerrainQuery(TerrainQueryFn fn) { m_TerrainQuery = std::move(fn); }
	void SetBodyQuery(BodyQueryFn fn) { m_BodyQuery = std::move(fn); }

	/// Update gravity (call when scene changes).
	void SetGravity(float gx, float gy) { m_Config.gravityX = gx; m_Config.gravityY = gy; }

	/// Read-only access to particle data (SoA layout).
	int32_t GetParticleCount() const { return m_Count; }
	const float* GetPositionsX() const { return m_PosX.data(); }
	const float* GetPositionsY() const { return m_PosY.data(); }
	const float* GetVelocitiesX() const { return m_VelX.data(); }
	const float* GetVelocitiesY() const { return m_VelY.data(); }
	const float* GetWeights() const { return m_Weight.data(); }
	const uint32_t* GetColors() const { return m_Color.data(); }
	const uint32_t* GetFlags() const { return m_Flags.data(); }

	/// Get body impulses accumulated during the last Step().
	const std::vector<FluidBodyImpulse>& GetBodyImpulses() const { return m_BodyImpulses; }

	/// Get config for reading.
	const Config& GetConfig() const { return m_Config; }

private:
	Config m_Config;
	int32_t m_Count = 0;

	// ---- SoA parallel arrays (indexed by particle index) ----
	std::vector<float> m_PosX, m_PosY;
	std::vector<float> m_VelX, m_VelY;
	std::vector<float> m_ForceX, m_ForceY;
	std::vector<float> m_Weight;          // Density estimate
	std::vector<uint32_t> m_Flags;
	std::vector<uint32_t> m_Color;

	// ---- Spatial hash ----
	struct Proxy {
		uint32_t tag;
		int32_t index;
	};
	std::vector<Proxy> m_Proxies;

	// ---- Contact buffers (rebuilt each sub-step) ----
	std::vector<FluidContact> m_Contacts;
	std::vector<FluidBodyContact> m_BodyContacts;

	// ---- Output impulses for rigid bodies ----
	std::vector<FluidBodyImpulse> m_BodyImpulses;

	// ---- External callbacks ----
	TerrainQueryFn m_TerrainQuery;
	BodyQueryFn m_BodyQuery;

	// ---- Internal methods (LiquidFun-derived algorithm) ----
	void ComputeTags();
	void SortProxies();
	void FindContacts();
	void FindBodyContacts();
	void ComputeWeight();
	void ApplyGravity(float dt);
	void ApplyPressure(float dt);
	void ApplyDamping(float dt);
	void ApplyViscous(float dt);
	void ResolveTerrainCollisions(float dt);
	void ResolveBodyCollisions(float dt);
	void Integrate(float dt);
	void CompactParticles();
};

} // namespace RTE
