/**
 * FluidSolver.cpp
 *
 * SPH fluid solver implementation.
 * Algorithm derived from LiquidFun (Google, Apache 2.0 license).
 *
 * Key design choices:
 * - SoA (Structure of Arrays) for cache-friendly iteration
 * - Tag-sort spatial hash for O(N) neighbor finding
 * - All coordinates in pixel space (CC convention)
 * - No external dependencies (terrain/body via callbacks)
 */

#include "FluidSolver.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace RTE;

// ---------------------------------------------------------------------------
// LiquidFun-derived constants
// ---------------------------------------------------------------------------
static constexpr float MIN_PARTICLE_WEIGHT = 1.0f;  // Density threshold for pressure
static constexpr float MAX_PARTICLE_PRESSURE = 0.25f;
static constexpr float QUADRATIC_DAMPING = 0.5f;

// ---------------------------------------------------------------------------
// Spatial hash helpers
// ---------------------------------------------------------------------------

// Encode a grid cell position into a 32-bit tag.
// Y occupies upper 16 bits, X the lower 16 bits.
// This gives a Z-order-like layout: sorted by Y first, then X.
static inline uint32_t ComputeTag(float x, float y, float invCellSize) {
	// Offset to make negative coordinates positive (support scenes up to ~32k px)
	constexpr float OFFSET = 32768.0f;
	uint32_t gx = static_cast<uint32_t>(x * invCellSize + OFFSET);
	uint32_t gy = static_cast<uint32_t>(y * invCellSize + OFFSET);
	return (gy << 16) | (gx & 0xFFFF);
}

// Compute the tag range for cells adjacent to a given tag.
static inline void ComputeTagRange(uint32_t tag, uint32_t& lower, uint32_t& upper) {
	// One row up/down = ±(1 << 16), one column left/right = ±1
	uint32_t row = tag & 0xFFFF0000;
	uint32_t col = tag & 0x0000FFFF;
	uint32_t rowLo = (row >= 0x00010000) ? row - 0x00010000 : 0;
	uint32_t rowHi = (row <= 0xFFFE0000) ? row + 0x00020000 : 0xFFFF0000;
	uint32_t colLo = (col >= 1) ? col - 1 : 0;
	uint32_t colHi = (col <= 0xFFFE) ? col + 2 : 0xFFFF;
	lower = rowLo | colLo;
	upper = rowHi | colHi;
}

// ---------------------------------------------------------------------------
// Initialize / Destroy
// ---------------------------------------------------------------------------

void FluidSolver::Initialize(const Config& config) {
	m_Config = config;
	m_Count = 0;

	int cap = config.maxParticles;
	m_PosX.resize(cap, 0.0f);
	m_PosY.resize(cap, 0.0f);
	m_VelX.resize(cap, 0.0f);
	m_VelY.resize(cap, 0.0f);
	m_ForceX.resize(cap, 0.0f);
	m_ForceY.resize(cap, 0.0f);
	m_Weight.resize(cap, 0.0f);
	m_Flags.resize(cap, 0);
	m_Color.resize(cap, 0);
	m_Proxies.resize(cap);

	m_Contacts.reserve(cap * 6);   // ~6 contacts per particle typical for SPH
	m_BodyContacts.reserve(cap);
	m_BodyImpulses.reserve(256);
}

void FluidSolver::Destroy() {
	m_Count = 0;
	m_PosX.clear(); m_PosY.clear();
	m_VelX.clear(); m_VelY.clear();
	m_ForceX.clear(); m_ForceY.clear();
	m_Weight.clear();
	m_Flags.clear(); m_Color.clear();
	m_Proxies.clear();
	m_Contacts.clear();
	m_BodyContacts.clear();
	m_BodyImpulses.clear();
}

// ---------------------------------------------------------------------------
// Particle management
// ---------------------------------------------------------------------------

int32_t FluidSolver::AddParticle(float px, float py, float vx, float vy,
                                  uint32_t flags, uint32_t packedColor) {
	if (m_Count >= m_Config.maxParticles) return -1;

	int32_t i = m_Count++;
	m_PosX[i] = px;
	m_PosY[i] = py;
	m_VelX[i] = vx;
	m_VelY[i] = vy;
	m_ForceX[i] = 0.0f;
	m_ForceY[i] = 0.0f;
	m_Weight[i] = 0.0f;
	m_Flags[i] = flags;
	m_Color[i] = packedColor;
	return i;
}

void FluidSolver::RemoveParticle(int32_t index) {
	if (index >= 0 && index < m_Count) {
		m_Flags[index] |= FPF_Zombie;
	}
}

// ---------------------------------------------------------------------------
// Main simulation step
// ---------------------------------------------------------------------------

void FluidSolver::Step(float dt) {
	if (m_Count == 0 || dt <= 0.0f) return;

	m_BodyImpulses.clear();

	float subDt = dt / static_cast<float>(m_Config.subSteps);

	for (int sub = 0; sub < m_Config.subSteps; ++sub) {
		// Phase 1: Build spatial structure and contacts
		ComputeTags();
		SortProxies();
		FindContacts();
		FindBodyContacts();
		ComputeWeight();

		// Phase 2: Apply forces (all modify velocity)
		ApplyGravity(subDt);
		ApplyPressure(subDt);
		ApplyDamping(subDt);
		ApplyViscous(subDt);
		ResolveTerrainCollisions(subDt);
		ResolveBodyCollisions(subDt);

		// Phase 3: Integrate positions
		Integrate(subDt);
	}

	// Phase 4: Remove dead particles
	CompactParticles();
}

// ---------------------------------------------------------------------------
// Spatial hash
// ---------------------------------------------------------------------------

void FluidSolver::ComputeTags() {
	float diameter = m_Config.particleRadius * 2.0f;
	float invCellSize = 1.0f / diameter;

	for (int32_t i = 0; i < m_Count; ++i) {
		m_Proxies[i].tag = ComputeTag(m_PosX[i], m_PosY[i], invCellSize);
		m_Proxies[i].index = i;
	}
}

void FluidSolver::SortProxies() {
	std::sort(m_Proxies.begin(), m_Proxies.begin() + m_Count,
	          [](const Proxy& a, const Proxy& b) { return a.tag < b.tag; });
}

// ---------------------------------------------------------------------------
// Contact finding (LiquidFun's sorted-scan approach)
// ---------------------------------------------------------------------------

void FluidSolver::FindContacts() {
	m_Contacts.clear();

	float diameter = m_Config.particleRadius * 2.0f;
	float diameterSq = diameter * diameter;
	float invDiameter = 1.0f / diameter;

	// For each particle (in sorted order), check particles to the right
	// and in the row below. Because proxies are sorted by tag, nearby
	// particles in 2D space are adjacent in the array.
	for (int32_t a = 0; a < m_Count; ++a) {
		int32_t idxA = m_Proxies[a].index;
		uint32_t tagA = m_Proxies[a].tag;

		if (m_Flags[idxA] & FPF_Zombie) continue;

		float ax = m_PosX[idxA];
		float ay = m_PosY[idxA];

		// Scan forward — particles in same or adjacent cells
		// The tag encodes (gridY << 16 | gridX), so adjacent cells
		// differ by at most 0x10001 in tag value.
		uint32_t maxTag = tagA + 0x00020002; // 2 rows ahead, 2 cols ahead

		for (int32_t b = a + 1; b < m_Count; ++b) {
			uint32_t tagB = m_Proxies[b].tag;
			if (tagB > maxTag) break; // Too far in sorted order

			int32_t idxB = m_Proxies[b].index;
			if (m_Flags[idxB] & FPF_Zombie) continue;

			float dx = m_PosX[idxB] - ax;
			float dy = m_PosY[idxB] - ay;
			float distSq = dx * dx + dy * dy;

			if (distSq < diameterSq && distSq > 1e-8f) {
				float dist = std::sqrt(distSq);
				float weight = 1.0f - dist * invDiameter;
				float invDist = 1.0f / dist;

				m_Contacts.push_back({
					idxA, idxB,
					weight,
					dx * invDist, dy * invDist
				});
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Body contacts (via external callback)
// ---------------------------------------------------------------------------

void FluidSolver::FindBodyContacts() {
	m_BodyContacts.clear();
	if (!m_BodyQuery || m_Count == 0) return;

	// Compute bounding box of all particles
	float minX = m_PosX[0], maxX = m_PosX[0];
	float minY = m_PosY[0], maxY = m_PosY[0];
	for (int32_t i = 1; i < m_Count; ++i) {
		if (m_Flags[i] & FPF_Zombie) continue;
		if (m_PosX[i] < minX) minX = m_PosX[i];
		if (m_PosX[i] > maxX) maxX = m_PosX[i];
		if (m_PosY[i] < minY) minY = m_PosY[i];
		if (m_PosY[i] > maxY) maxY = m_PosY[i];
	}

	float r = m_Config.particleRadius;
	m_BodyQuery(minX - r, minY - r, maxX + r, maxY + r, m_BodyContacts);
}

// ---------------------------------------------------------------------------
// Weight (density) computation
// ---------------------------------------------------------------------------

void FluidSolver::ComputeWeight() {
	// Reset weights
	std::memset(m_Weight.data(), 0, m_Count * sizeof(float));

	// Sum contact weights — each contact contributes to both particles
	for (const auto& c : m_Contacts) {
		m_Weight[c.indexA] += c.weight;
		m_Weight[c.indexB] += c.weight;
	}

	// Body contacts also contribute to density
	for (const auto& bc : m_BodyContacts) {
		m_Weight[bc.particleIndex] += bc.weight;
	}
}

// ---------------------------------------------------------------------------
// Force computation
// ---------------------------------------------------------------------------

void FluidSolver::ApplyGravity(float dt) {
	float gx = m_Config.gravityX * dt;
	float gy = m_Config.gravityY * dt;
	for (int32_t i = 0; i < m_Count; ++i) {
		if (m_Flags[i] & (FPF_Wall | FPF_Zombie)) continue;
		m_VelX[i] += gx;
		m_VelY[i] += gy;
	}
}

void FluidSolver::ApplyPressure(float dt) {
	// Compute pressure per particle from density
	// pressure = strength * max(0, weight - minWeight)
	// Clamped to maxPressure * criticalPressure
	float diameter = m_Config.particleRadius * 2.0f;
	float invDt = (dt > 0.0f) ? 1.0f / dt : 0.0f;
	float criticalVel = diameter * invDt;
	float criticalPressure = criticalVel * criticalVel;
	float pressureMax = MAX_PARTICLE_PRESSURE * criticalPressure;

	// Velocity per unit pressure: dt / (density * diameter)
	// Using density=1 for simplicity (LiquidFun default)
	float velPerPressure = dt / diameter;

	// Particle-particle pressure
	for (const auto& c : m_Contacts) {
		float pressureA = m_Config.pressureStrength * std::max(0.0f, m_Weight[c.indexA] - MIN_PARTICLE_WEIGHT);
		float pressureB = m_Config.pressureStrength * std::max(0.0f, m_Weight[c.indexB] - MIN_PARTICLE_WEIGHT);
		pressureA = std::min(pressureA, pressureMax);
		pressureB = std::min(pressureB, pressureMax);

		float f = velPerPressure * c.weight * (pressureA + pressureB);
		float fx = f * c.nx;
		float fy = f * c.ny;

		if (!(m_Flags[c.indexA] & (FPF_Wall | FPF_Zombie))) {
			m_VelX[c.indexA] -= fx;
			m_VelY[c.indexA] -= fy;
		}
		if (!(m_Flags[c.indexB] & (FPF_Wall | FPF_Zombie))) {
			m_VelX[c.indexB] += fx;
			m_VelY[c.indexB] += fy;
		}
	}

	// Particle-body pressure: push particle away from body surface
	for (const auto& bc : m_BodyContacts) {
		int32_t i = bc.particleIndex;
		if (m_Flags[i] & (FPF_Wall | FPF_Zombie)) continue;

		float pressure = m_Config.pressureStrength * std::max(0.0f, m_Weight[i] - MIN_PARTICLE_WEIGHT);
		pressure = std::min(pressure, pressureMax);

		float f = velPerPressure * bc.weight * pressure;
		// Push particle away from body along contact normal
		m_VelX[i] -= f * bc.nx;
		m_VelY[i] -= f * bc.ny;
	}
}

void FluidSolver::ApplyDamping(float dt) {
	// Reduce relative normal velocity at contacts (energy dissipation)
	for (const auto& c : m_Contacts) {
		int32_t a = c.indexA, b = c.indexB;
		if ((m_Flags[a] | m_Flags[b]) & FPF_Zombie) continue;

		// Relative velocity along contact normal
		float dvx = m_VelX[b] - m_VelX[a];
		float dvy = m_VelY[b] - m_VelY[a];
		float vn = dvx * c.nx + dvy * c.ny;

		if (vn < 0.0f) { // Approaching
			float damping = std::max(m_Config.dampingStrength * c.weight,
			                         std::min(-QUADRATIC_DAMPING * vn, 0.5f));
			float fx = damping * vn * c.nx;
			float fy = damping * vn * c.ny;

			if (!(m_Flags[a] & FPF_Wall)) {
				m_VelX[a] += fx;
				m_VelY[a] += fy;
			}
			if (!(m_Flags[b] & FPF_Wall)) {
				m_VelX[b] -= fx;
				m_VelY[b] -= fy;
			}
		}
	}
}

void FluidSolver::ApplyViscous(float dt) {
	// Viscous: match velocities between neighbors (only for viscous particles)
	for (const auto& c : m_Contacts) {
		int32_t a = c.indexA, b = c.indexB;
		bool aViscous = (m_Flags[a] & FPF_Viscous) != 0;
		bool bViscous = (m_Flags[b] & FPF_Viscous) != 0;
		if (!aViscous && !bViscous) continue;
		if ((m_Flags[a] | m_Flags[b]) & FPF_Zombie) continue;

		float dvx = m_VelX[b] - m_VelX[a];
		float dvy = m_VelY[b] - m_VelY[a];

		float f = m_Config.viscousStrength * c.weight;
		float fx = f * dvx;
		float fy = f * dvy;

		if (!(m_Flags[a] & FPF_Wall)) {
			m_VelX[a] += fx;
			m_VelY[a] += fy;
		}
		if (!(m_Flags[b] & FPF_Wall)) {
			m_VelX[b] -= fx;
			m_VelY[b] -= fy;
		}
	}
}

// ---------------------------------------------------------------------------
// Terrain collision
// ---------------------------------------------------------------------------

void FluidSolver::ResolveTerrainCollisions(float /*dt*/) {
	if (!m_TerrainQuery) return;

	float radius = m_Config.particleRadius;

	for (int32_t i = 0; i < m_Count; ++i) {
		if (m_Flags[i] & (FPF_Wall | FPF_Zombie)) continue;

		int px = static_cast<int>(m_PosX[i]);
		int py = static_cast<int>(m_PosY[i]);

		// Check if particle center is inside terrain
		if (!m_TerrainQuery(px, py)) continue;

		// Compute terrain normal by sampling a small grid around the particle.
		// Normal points from solid → air (the escape direction).
		float nx = 0.0f, ny = 0.0f;
		int sampleDist = static_cast<int>(radius) + 1;
		for (int dy = -sampleDist; dy <= sampleDist; ++dy) {
			for (int dx = -sampleDist; dx <= sampleDist; ++dx) {
				if (dx == 0 && dy == 0) continue;
				bool solid = m_TerrainQuery(px + dx, py + dy);
				if (!solid) {
					// This neighbor is air — contribute to normal
					float len = std::sqrt(static_cast<float>(dx * dx + dy * dy));
					nx += static_cast<float>(dx) / len;
					ny += static_cast<float>(dy) / len;
				}
			}
		}

		float nLen = std::sqrt(nx * nx + ny * ny);
		if (nLen < 0.01f) {
			// Completely surrounded by terrain — push straight up
			nx = 0.0f;
			ny = -1.0f;
		} else {
			nx /= nLen;
			ny /= nLen;
		}

		// Push particle out of terrain along normal
		m_PosX[i] += nx * radius;
		m_PosY[i] += ny * radius;

		// Reflect velocity component along normal
		float vn = m_VelX[i] * nx + m_VelY[i] * ny;
		if (vn < 0.0f) { // Moving into terrain
			// Remove normal component and apply restitution
			m_VelX[i] -= (1.0f + m_Config.restitution) * vn * nx;
			m_VelY[i] -= (1.0f + m_Config.restitution) * vn * ny;

			// Apply friction to tangential component
			float tx = m_VelX[i] - vn * nx;
			float ty = m_VelY[i] - vn * ny;
			m_VelX[i] -= m_Config.friction * tx;
			m_VelY[i] -= m_Config.friction * ty;
		}
	}
}

// ---------------------------------------------------------------------------
// Body collision
// ---------------------------------------------------------------------------

void FluidSolver::ResolveBodyCollisions(float dt) {
	for (const auto& bc : m_BodyContacts) {
		int32_t i = bc.particleIndex;
		if (m_Flags[i] & (FPF_Wall | FPF_Zombie)) continue;

		// Relative velocity (particle relative to body)
		float dvx = m_VelX[i] - bc.bodyVelX;
		float dvy = m_VelY[i] - bc.bodyVelY;
		float vn = dvx * bc.nx + dvy * bc.ny;

		if (vn > 0.0f) continue; // Moving away from body

		// Push particle out along normal
		float pushDist = m_Config.particleRadius * bc.weight;
		m_PosX[i] -= bc.nx * pushDist;
		m_PosY[i] -= bc.ny * pushDist;

		// Reflect velocity
		float restitution = m_Config.restitution;
		m_VelX[i] -= (1.0f + restitution) * vn * bc.nx;
		m_VelY[i] -= (1.0f + restitution) * vn * bc.ny;

		// Apply equal and opposite impulse to the body
		if (bc.bodyMass > 0.0f) {
			// Impulse = change_in_momentum of fluid particle
			// Approximate particle mass from density * area
			float diameter = m_Config.particleRadius * 2.0f;
			float pMass = diameter * diameter; // density=1, area=d^2
			float impulseMag = pMass * std::abs(vn) * (1.0f + restitution);

			m_BodyImpulses.push_back({
				bc.bodyUserData,
				bc.nx * impulseMag,
				bc.ny * impulseMag,
				m_PosX[i], m_PosY[i]
			});
		}
	}
}

// ---------------------------------------------------------------------------
// Integration
// ---------------------------------------------------------------------------

void FluidSolver::Integrate(float dt) {
	// Clamp velocity to critical velocity (prevents tunneling)
	float diameter = m_Config.particleRadius * 2.0f;
	float maxVel = diameter / dt;
	float maxVelSq = maxVel * maxVel;

	for (int32_t i = 0; i < m_Count; ++i) {
		if (m_Flags[i] & FPF_Zombie) continue;

		// Wall particles don't move
		if (m_Flags[i] & FPF_Wall) {
			m_VelX[i] = 0.0f;
			m_VelY[i] = 0.0f;
			continue;
		}

		// Clamp velocity
		float vSq = m_VelX[i] * m_VelX[i] + m_VelY[i] * m_VelY[i];
		if (vSq > maxVelSq) {
			float scale = maxVel / std::sqrt(vSq);
			m_VelX[i] *= scale;
			m_VelY[i] *= scale;
		}

		// Euler integration
		m_PosX[i] += m_VelX[i] * dt;
		m_PosY[i] += m_VelY[i] * dt;
	}
}

// ---------------------------------------------------------------------------
// Compact (remove zombie particles)
// ---------------------------------------------------------------------------

void FluidSolver::CompactParticles() {
	int32_t dst = 0;
	for (int32_t src = 0; src < m_Count; ++src) {
		if (m_Flags[src] & FPF_Zombie) continue;

		if (dst != src) {
			m_PosX[dst]  = m_PosX[src];
			m_PosY[dst]  = m_PosY[src];
			m_VelX[dst]  = m_VelX[src];
			m_VelY[dst]  = m_VelY[src];
			m_Weight[dst] = m_Weight[src];
			m_Flags[dst] = m_Flags[src];
			m_Color[dst] = m_Color[src];
		}
		++dst;
	}
	m_Count = dst;
}
