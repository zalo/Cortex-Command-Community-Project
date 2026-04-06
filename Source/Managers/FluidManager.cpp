/**
 * FluidManager.cpp
 *
 * Bridges the standalone FluidSolver to CC's terrain, Box2D, and rendering.
 */

#include "FluidManager.h"
#include "Box2DManager.h"
#include "SceneMan.h"
#include "FrameMan.h"
#include "CameraMan.h"
#include "SLTerrain.h"
#include "Material.h"
#include "PresetMan.h"
#include "Vector.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// Allegro compat for putpixel
#ifdef __EMSCRIPTEN__
#include "AllegroCompat.h"
#else
#include <allegro.h>
#endif

using namespace RTE;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void FluidManager::Clear() {
	m_Enabled = false;
	m_DebugDraw = false;
	m_MaterialIndex.clear();
	m_RestFrames.clear();
}

void FluidManager::Initialize() {
	FluidSolver::Config config;
	config.particleRadius = 3.0f;   // 3-pixel radius = 6px diameter particles
	config.particleStride = 0.75f;
	config.gravityX = 0.0f;
	config.gravityY = g_SceneMan.GetGlobalAcc().GetY() * 20.0f; // Convert m/s² to px/s²
	config.pressureStrength = 0.5f;    // Strong pressure to prevent collapse
	config.dampingStrength = 0.5f;
	config.viscousStrength = 0.25f;
	config.restitution = 0.1f;     // Water is inelastic
	config.friction = 0.02f;
	config.maxParticles = 4096;
	config.subSteps = 2;

	m_Solver.Initialize(config);

	// Wire up callbacks
	m_Solver.SetTerrainQuery([](int x, int y) -> bool {
		return FluidManager::TerrainQueryCallback(x, y);
	});
	m_Solver.SetBodyQuery([](float minX, float minY, float maxX, float maxY,
	                         std::vector<FluidBodyContact>& contacts) {
		FluidManager::BodyQueryCallback(minX, minY, maxX, maxY, contacts);
	});

	m_MaterialIndex.resize(config.maxParticles, 0);
	m_PaletteIndex.resize(config.maxParticles, 0);
	m_RestFrames.resize(config.maxParticles, 0);
	m_Enabled = true;

#ifdef __EMSCRIPTEN__
	EM_ASM({ console.log('[Fluid] Initialized, gravity=' + $0.toFixed(1) + ' px/s², maxParticles=' + $1); },
	       config.gravityY, config.maxParticles);
#endif
}

void FluidManager::Destroy() {
	m_Solver.Destroy();
	Clear();
}

void FluidManager::Reset() {
	Destroy();
	// Don't re-initialize — wait for scene load
}

// ---------------------------------------------------------------------------
// Simulation step
// ---------------------------------------------------------------------------

void FluidManager::Step(float deltaTime) {
	if (!m_Enabled || m_Solver.GetParticleCount() == 0) return;

	// Update gravity from current scene (may not be loaded at init time)
	float gravY = g_SceneMan.GetGlobalAcc().GetY() * Box2DManager::PPM;
	m_Solver.SetGravity(0.0f, gravY);

	m_Solver.Step(deltaTime);
	ApplyBodyImpulses();
	ProcessSettling();
}

// ---------------------------------------------------------------------------
// Terrain callback
// ---------------------------------------------------------------------------

bool FluidManager::TerrainQueryCallback(int x, int y) {
	// Material index 0 = air/empty
	return g_SceneMan.GetTerrMatter(x, y) != 0;
}

// ---------------------------------------------------------------------------
// Body collision callback (Box2D v3 bridge)
// ---------------------------------------------------------------------------

// Context struct for Box2D overlap query callback
struct FluidBodyQueryCtx {
	const FluidSolver* solver;
	std::vector<FluidBodyContact>* contacts;
	float radius;
	float ppm;
};

// Static C-compatible callback for b2World_OverlapAABB
static bool FluidBodyOverlapCallback(b2ShapeId shapeId, void* context) {
	auto* ctx = static_cast<FluidBodyQueryCtx*>(context);
	b2BodyId bodyId = b2Shape_GetBody(shapeId);
	if (!b2Body_IsValid(bodyId)) return true;

	b2BodyType bodyType = b2Body_GetType(bodyId);
	if (bodyType == b2_staticBody) return true;

	float bodyMass = b2Body_GetMass(bodyId);
	b2Vec2 bodyCenter = b2Body_GetWorldCenterOfMass(bodyId);
	float bodyCenterPx = bodyCenter.x * ctx->ppm;
	float bodyCenterPy = bodyCenter.y * ctx->ppm;

	b2AABB shapeAABB = b2Shape_GetAABB(shapeId);
	float shapeMinX = shapeAABB.lowerBound.x * ctx->ppm;
	float shapeMinY = shapeAABB.lowerBound.y * ctx->ppm;
	float shapeMaxX = shapeAABB.upperBound.x * ctx->ppm;
	float shapeMaxY = shapeAABB.upperBound.y * ctx->ppm;

	int32_t count = ctx->solver->GetParticleCount();
	const float* posX = ctx->solver->GetPositionsX();
	const float* posY = ctx->solver->GetPositionsY();
	const uint32_t* flags = ctx->solver->GetFlags();
	float r = ctx->radius;

	for (int32_t i = 0; i < count; ++i) {
		if (flags[i] & (FPF_Wall | FPF_Zombie)) continue;

		float px = posX[i], py = posY[i];

		// Quick AABB rejection
		if (px + r < shapeMinX || px - r > shapeMaxX ||
		    py + r < shapeMinY || py - r > shapeMaxY) continue;

		// Test if particle point is inside shape
		b2Vec2 pointM = {px / ctx->ppm, py / ctx->ppm};
		if (!b2Shape_TestPoint(shapeId, pointM)) {
			float ddx = px - bodyCenterPx;
			float ddy = py - bodyCenterPy;
			float dist2 = std::sqrt(ddx * ddx + ddy * ddy);
			float shapeRadius = (shapeMaxX - shapeMinX + shapeMaxY - shapeMinY) * 0.25f;
			if (dist2 > shapeRadius + r) continue;
		}

		// Contact normal: particle → body center
		float dx = bodyCenterPx - px;
		float dy = bodyCenterPy - py;
		float dist = std::sqrt(dx * dx + dy * dy);
		if (dist < 0.01f) continue;

		float nx = dx / dist;
		float ny = dy / dist;
		float weight = std::max(0.0f, 1.0f - dist / (r * 4.0f));

		b2Vec2 bodyVel = b2Body_GetWorldPointVelocity(bodyId, pointM);

		FluidBodyContact contact;
		contact.particleIndex = i;
		contact.bodyUserData = static_cast<int64_t>(b2StoreBodyId(bodyId));
		contact.weight = weight;
		contact.nx = nx;
		contact.ny = ny;
		contact.bodyMass = bodyMass;
		contact.bodyVelX = bodyVel.x;
		contact.bodyVelY = bodyVel.y;

		ctx->contacts->push_back(contact);
	}
	return true;
}

void FluidManager::BodyQueryCallback(float minX, float minY, float maxX, float maxY,
                                      std::vector<FluidBodyContact>& contacts) {
	if (!g_Box2DMan.IsActive()) return;

	b2WorldId worldId = g_Box2DMan.GetWorldId();
	float ppm = Box2DManager::PPM;
	float radius = g_FluidMan.m_Solver.GetConfig().particleRadius;

	b2AABB queryAABB;
	queryAABB.lowerBound = {minX / ppm, minY / ppm};
	queryAABB.upperBound = {maxX / ppm, maxY / ppm};

	b2QueryFilter filter = b2DefaultQueryFilter();

	FluidBodyQueryCtx ctx = {&g_FluidMan.m_Solver, &contacts, radius, ppm};
	b2World_OverlapAABB(worldId, queryAABB, filter, FluidBodyOverlapCallback, &ctx);
}

// ---------------------------------------------------------------------------
// Apply impulses from fluid → Box2D bodies
// ---------------------------------------------------------------------------

void FluidManager::ApplyBodyImpulses() {
	if (!g_Box2DMan.IsActive()) return;

	const auto& impulses = m_Solver.GetBodyImpulses();
	float ppm = Box2DManager::PPM;

	for (const auto& imp : impulses) {
		b2BodyId bodyId = b2LoadBodyId(static_cast<uint64_t>(imp.bodyUserData));

		if (!b2Body_IsValid(bodyId)) continue;

		b2Vec2 impulse = {imp.impulseX / ppm, imp.impulseY / ppm};
		b2Vec2 point = {imp.pointX / ppm, imp.pointY / ppm};
		b2Body_ApplyLinearImpulse(bodyId, impulse, point, true);
	}
}

// ---------------------------------------------------------------------------
// Settling — convert resting particles to terrain pixels
// ---------------------------------------------------------------------------

void FluidManager::ProcessSettling() {
	int32_t count = m_Solver.GetParticleCount();
	const float* velX = m_Solver.GetVelocitiesX();
	const float* velY = m_Solver.GetVelocitiesY();
	const float* posX = m_Solver.GetPositionsX();
	const float* posY = m_Solver.GetPositionsY();

	SLTerrain* terrain = g_SceneMan.GetTerrain();
	if (!terrain) return;

	for (int32_t i = 0; i < count; ++i) {
		float speed = std::sqrt(velX[i] * velX[i] + velY[i] * velY[i]);

		if (speed < REST_VEL_THRESHOLD) {
			m_RestFrames[i]++;
		} else {
			m_RestFrames[i] = 0;
		}

		if (m_RestFrames[i] >= SETTLE_THRESHOLD) {
			// Settle: paint this particle to terrain as a material pixel
			int px = static_cast<int>(posX[i]);
			int py = static_cast<int>(posY[i]);

			unsigned char matIndex = m_MaterialIndex[i];
			if (matIndex == 0) matIndex = 160; // Default to water

			// Look up settle material
			const Material* mat = g_SceneMan.GetMaterialFromID(matIndex);
			if (mat) {
				unsigned char settleMat = mat->GetSettleMaterial();

				// Piling: try to find an empty spot (move up or jitter)
				unsigned char existingMat = g_SceneMan.GetTerrMatter(px, py);
				int piling = mat->GetPiling();
				for (int s = 0; s < piling && existingMat != 0; ++s) {
					if ((piling - s) % 2 == 0) {
						py -= 1; // Move up
					} else {
						px += (rand() % 2 == 0) ? 1 : -1; // Random jitter
					}
					existingMat = g_SceneMan.GetTerrMatter(px, py);
				}

				// Paint the pixel
				terrain->SetMaterialPixel(px, py, settleMat);
				// Also set the foreground color pixel
				uint32_t color = m_Solver.GetColors()[i];
				terrain->SetFGColorPixel(px, py,
					((color >> 16) & 0xFF) | (color & 0xFF00) | ((color & 0xFF) << 16));
			}

			m_Solver.RemoveParticle(i);
			m_RestFrames[i] = 0;
		}
	}
}

// ---------------------------------------------------------------------------
// Spawning
// ---------------------------------------------------------------------------

void FluidManager::SpawnFluid(float px, float py, float vx, float vy,
                               unsigned char materialIndex, int count) {
	if (!m_Enabled) return;

	const Material* mat = g_SceneMan.GetMaterialFromID(materialIndex);
	uint32_t color = 0xFF4488CC;
	unsigned char palIdx = 133; // Default blue-ish palette index
	if (mat) {
		Color matColor = mat->GetColor();
		color = (0xFF << 24) | (matColor.GetR() << 16) | (matColor.GetG() << 8) | matColor.GetB();
		palIdx = static_cast<unsigned char>(matColor.GetIndex());
	}

	float radius = m_Solver.GetConfig().particleRadius;
	float spacing = radius * 2.0f * m_Solver.GetConfig().particleStride;

	for (int i = 0; i < count; ++i) {
		float ox = (static_cast<float>(rand()) / RAND_MAX - 0.5f) * spacing;
		float oy = (static_cast<float>(rand()) / RAND_MAX - 0.5f) * spacing;

		int32_t idx = m_Solver.AddParticle(px + ox, py + oy, vx, vy, FPF_Water, color);
		if (idx >= 0) {
			m_MaterialIndex[idx] = materialIndex;
			m_PaletteIndex[idx] = palIdx;
			m_RestFrames[idx] = 0;
		}
	}

#ifdef __EMSCRIPTEN__
	if (count > 10) {
		EM_ASM({ console.log('[Fluid] Spawned ' + $0 + ' particles, total=' + $1); },
		       count, m_Solver.GetParticleCount());
	}
#endif
}

void FluidManager::SpawnFluidRect(float x, float y, float w, float h,
                                   unsigned char materialIndex) {
	if (!m_Enabled) return;

	const Material* mat = g_SceneMan.GetMaterialFromID(materialIndex);
	uint32_t color = 0xFF4488CC;
	unsigned char palIdx = 133;
	if (mat) {
		Color matColor = mat->GetColor();
		color = (0xFF << 24) | (matColor.GetR() << 16) | (matColor.GetG() << 8) | matColor.GetB();
		palIdx = static_cast<unsigned char>(matColor.GetIndex());
	}

	float spacing = m_Solver.GetConfig().particleRadius * 2.0f * m_Solver.GetConfig().particleStride;
	int spawned = 0;

	for (float py = y; py < y + h; py += spacing) {
		for (float px = x; px < x + w; px += spacing) {
			int32_t idx = m_Solver.AddParticle(px, py, 0.0f, 0.0f, FPF_Water, color);
			if (idx >= 0) {
				m_MaterialIndex[idx] = materialIndex;
				m_PaletteIndex[idx] = palIdx;
				m_RestFrames[idx] = 0;
				spawned++;
			}
		}
	}

#ifdef __EMSCRIPTEN__
	EM_ASM({ console.log('[Fluid] SpawnRect ' + $0 + 'x' + $1 + ' → ' + $2 + ' particles, total=' + $3); },
	       (int)w, (int)h, spawned, m_Solver.GetParticleCount());
#endif
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void FluidManager::Draw(BITMAP* targetBitmap, const Vector& cameraOffset) {
	if (!m_Enabled || !targetBitmap) return;

	int32_t count = m_Solver.GetParticleCount();
	const float* posX = m_Solver.GetPositionsX();
	const float* posY = m_Solver.GetPositionsY();
	const uint32_t* flags = m_Solver.GetFlags();
	const uint32_t* colors = m_Solver.GetColors();

	float camX = cameraOffset.GetX();
	float camY = cameraOffset.GetY();
	int bw = targetBitmap->w;
	int bh = targetBitmap->h;
	bool is8bpp = (targetBitmap->bpp() == 1);

	for (int32_t i = 0; i < count; ++i) {
		if (flags[i] & FPF_Zombie) continue;

		int sx = static_cast<int>(posX[i] - camX);
		int sy = static_cast<int>(posY[i] - camY);

		if (sx < 0 || sy < 0 || sx >= bw || sy >= bh) continue;

		if (is8bpp) {
			putpixel(targetBitmap, sx, sy, m_PaletteIndex[i]);
		} else {
			putpixel(targetBitmap, sx, sy, static_cast<int>(colors[i]));
		}
	}
}

void FluidManager::DrawMatter(BITMAP* targetBitmap, const Vector& cameraOffset) {
	if (!m_Enabled || !targetBitmap) return;

	int32_t count = m_Solver.GetParticleCount();
	const float* posX = m_Solver.GetPositionsX();
	const float* posY = m_Solver.GetPositionsY();
	const uint32_t* flags = m_Solver.GetFlags();

	float camX = cameraOffset.GetX();
	float camY = cameraOffset.GetY();

	for (int32_t i = 0; i < count; ++i) {
		if (flags[i] & FPF_Zombie) continue;

		int sx = static_cast<int>(posX[i] - camX);
		int sy = static_cast<int>(posY[i] - camY);

		if (sx < 0 || sy < 0 || sx >= targetBitmap->w || sy >= targetBitmap->h) continue;

		putpixel(targetBitmap, sx, sy, m_MaterialIndex[i]);
	}
}

void FluidManager::DrawDebug(BITMAP* targetBitmap, const Vector& cameraOffset) {
	if (!m_Enabled || !targetBitmap) return;

	int32_t count = m_Solver.GetParticleCount();
	const float* posX = m_Solver.GetPositionsX();
	const float* posY = m_Solver.GetPositionsY();
	const float* weights = m_Solver.GetWeights();
	const uint32_t* flags = m_Solver.GetFlags();

	float camX = cameraOffset.GetX();
	float camY = cameraOffset.GetY();

	for (int32_t i = 0; i < count; ++i) {
		if (flags[i] & FPF_Zombie) continue;

		int sx = static_cast<int>(posX[i] - camX);
		int sy = static_cast<int>(posY[i] - camY);

		if (sx < 1 || sy < 1 || sx >= targetBitmap->w - 1 || sy >= targetBitmap->h - 1) continue;

		// Color by density: blue (low) → green (normal) → red (high)
		float w = weights[i];
		int r = static_cast<int>(std::min(255.0f, w * 128.0f));
		int g = static_cast<int>(std::min(255.0f, (2.0f - std::abs(w - 2.0f)) * 128.0f));
		int b = static_cast<int>(std::min(255.0f, (3.0f - w) * 128.0f));
		uint32_t debugColor = (0xFF << 24) | (r << 16) | (g << 8) | b;

		// Draw a 3x3 cross for visibility
		putpixel(targetBitmap, sx, sy, static_cast<int>(debugColor));
		putpixel(targetBitmap, sx - 1, sy, static_cast<int>(debugColor));
		putpixel(targetBitmap, sx + 1, sy, static_cast<int>(debugColor));
		putpixel(targetBitmap, sx, sy - 1, static_cast<int>(debugColor));
		putpixel(targetBitmap, sx, sy + 1, static_cast<int>(debugColor));
	}
}
