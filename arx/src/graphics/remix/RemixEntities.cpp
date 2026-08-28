/*
 * Arx Remaster — M4: send nearby entities to Remix from engine data.
 *
 * Meshes are exported once per model, in object space (or bone space when the
 * model has a skeleton), and moved with a per-frame transform. The earlier
 * version baked world-space vertices behind an identity transform and mixed the
 * frame counter into the mesh hash, so Remix saw a brand new, never-moving
 * object every rebuild: no temporal history, hence the ghost trails.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/remix/RemixEntities.h"

#if ARX_HAVE_RTX_REMIX

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "animation/Skeleton.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "graphics/Color.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/Math.h"
#include "graphics/Vertex.h"
#include "graphics/data/TextureContainer.h"
#include "graphics/remix/RemixApi.h"
#include "graphics/remix/RemixConvert.h"
#include "graphics/remix/RemixTextures.h"
#include "io/log/Logger.h"
#include "math/Types.h"
#include "platform/Platform.h"

namespace remix {
namespace {

constexpr size_t kMaxEntities = 48;
constexpr size_t kMaxModelTriangles = 8000;
constexpr size_t kMaxTrianglesTotal = 45000;

struct ModelMesh {
	remixapi_MeshHandle mesh = nullptr;
	bool skinned = false;
	size_t boneCount = 0;
	size_t triangles = 0;
	//! Guards against a freed EERIE_3DOBJ being replaced at the same address.
	size_t vertexCount = 0;
	size_t faceCount = 0;
	bool forcedRigid = false;
	unsigned frame = 0;
};

std::map<const EERIE_3DOBJ *, ModelMesh> g_models;
unsigned g_loggedEntities = 0;
unsigned g_loggedLinks = 0;

bool skipFace(const EERIE_FACE & face, TextureContainer * tex) {
	if(face.facetype & (POLY_NODRAW | POLY_HIDE | POLY_IGNORE | POLY_WATER)) {
		return true;
	}
	if(face.facetype & POLY_LAVA) {
		return false;
	}
	if(face.facetype & POLY_TRANS) {
		return !isCutoutTexture(tex);
	}
	return false;
}

/*!
 * Arx is rigidly skinned: every vertex belongs to exactly one bone group, its
 * rest position lives in vertexlocal, and the pose is one affine transform per
 * bone. That maps straight onto Remix skinning with bonesPerVertex = 1.
 */
bool canSkin(const EERIE_3DOBJ & obj, bool forceRigid) {

	if(forceRigid || debugEnabled(DebugNoSkinning) || !obj.m_skeleton) {
		return false;
	}

	const Skeleton & rig = *obj.m_skeleton;
	if(rig.bones.size() == 0 || rig.bones.size() > REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT) {
		return false;
	}

	return obj.m_boneVertices.size() == rig.bones.size()
	       && obj.vertexlocal.size() == obj.vertexlist.size();
}

//! One bone index per vertex, from the bone-to-vertices lists Arx already keeps.
std::vector<uint32_t> boneIndices(const EERIE_3DOBJ & obj) {

	std::vector<uint32_t> out(obj.vertexlist.size(), 0);

	for(VertexGroupId group : obj.m_boneVertices.handles()) {
		for(VertexId vertex : obj.m_boneVertices[group]) {
			const size_t index = size_t(vertex.handleData());
			if(index < out.size()) {
				out[index] = uint32_t(group.handleData());
			}
		}
	}

	return out;
}

/*!
 * Arx linear map → remixapi_Transform.
 *
 * The Y flip is a reflection D = diag(1, -1, 1), so a rotation has to be
 * conjugated: L_remix = D * L_arx * D, which is just a sign flip on every entry
 * with exactly one index equal to 1. glm is column-major and remixapi_Transform
 * is row-major, hence the [c][r] read.
 */
remixapi_Transform toRemixTransform(const glm::mat4 & linear, const Vec3f & translation) {

	remixapi_Transform out {};

	for(int r = 0; r < 3; r++) {
		for(int c = 0; c < 3; c++) {
			const float sign = ((r == 1) != (c == 1)) ? -1.f : 1.f;
			out.matrix[r][c] = sign * linear[c][r];
		}
	}

	out.matrix[0][3] = translation.x;
	out.matrix[1][3] = translation.y;
	out.matrix[2][3] = translation.z;

	return out;
}

remixapi_Transform entityTransform(const Entity & entity) {
	// Non-skeletal entities carry no scale in the engine's own draw path either.
	return toRemixTransform(toRotationMatrix(entity.angle), toRemix(entity.pos));
}

/*!
 * One world-space affine per bone, matching Cedric_TransformVerts():
 *   world = (mat4_cast(quat) * diag(scale)) * vertexlocal + trans
 * Entity scale is already folded into bone.anim.scale by animateSkeleton().
 */
void fillBoneTransforms(const EERIE_3DOBJ & obj, std::vector<remixapi_Transform> & out) {

	const Skeleton & rig = *obj.m_skeleton;

	out.clear();
	out.reserve(rig.bones.size());

	for(VertexGroupId group : rig.bones.handles()) {
		const Bone & bone = rig.bones[group];

		glm::mat4 matrix = glm::mat4_cast(bone.anim.quat);
		for(int row = 0; row < 3; row++) {
			matrix[0][row] *= bone.anim.scale.x;
			matrix[1][row] *= bone.anim.scale.y;
			matrix[2][row] *= bone.anim.scale.z;
		}

		out.push_back(toRemixTransform(matrix, toRemix(bone.anim.trans)));
	}
}

struct Bucket {
	std::vector<remixapi_HardcodedVertex> vertices;
	std::vector<float> weights;
	std::vector<uint32_t> bones;
};

remixapi_HardcodedVertex toEntityVertex(const Vec3f & p, const Vec3f & n, float u, float v) {

	remixapi_HardcodedVertex out {};

	const Vec3f rp = toRemixObject(p);
	out.position[0] = rp.x;
	out.position[1] = rp.y;
	out.position[2] = rp.z;

	const Vec3f rn = toRemixDir(n);
	const float len = std::sqrt(rn.x * rn.x + rn.y * rn.y + rn.z * rn.z);
	if(len > 1e-6f) {
		out.normal[0] = rn.x / len;
		out.normal[1] = rn.y / len;
		out.normal[2] = rn.z / len;
	} else {
		out.normal[1] = 1.f;
	}

	out.texcoord[0] = u;
	out.texcoord[1] = v;
	out.color = 0xFFFFFFFFu;

	return out;
}

void appendFace(Bucket & bucket, const EERIE_3DOBJ & obj, const EERIE_FACE & face,
                bool skinned, const std::vector<uint32_t> & bones) {

	// Y-flip is a reflection, so reverse winding to keep front faces.
	const int corners[3] = { 0, 2, 1 };

	for(int i = 0; i < 3; i++) {
		const VertexId id = face.vid[corners[i]];
		const size_t index = size_t(id.handleData());

		// Skinned meshes are exported in bone space, everything else in object space.
		const Vec3f p = skinned ? obj.vertexlocal[id] : obj.vertexlist[id].v;

		Vec3f n = obj.vertexlist[id].norm;
		if(glm::dot(n, n) < 1e-8f) {
			n = face.norm;
		}

		bucket.vertices.push_back(toEntityVertex(p, n, face.u[corners[i]], face.v[corners[i]]));
		if(skinned) {
			bucket.weights.push_back(1.f);
			bucket.bones.push_back(index < bones.size() ? bones[index] : 0u);
		}
	}
}

void destroyModel(RemixApi & api, ModelMesh & model) {
	if(model.mesh && api.iface().DestroyMesh) {
		api.iface().DestroyMesh(model.mesh);
	}
	model.mesh = nullptr;
}

ModelMesh buildModel(RemixApi & api, const EERIE_3DOBJ & obj, bool forceRigid) {

	ModelMesh model;
	model.vertexCount = obj.vertexlist.size();
	model.faceCount = obj.facelist.size();
	model.forcedRigid = forceRigid;
	model.skinned = canSkin(obj, forceRigid);

	const std::vector<uint32_t> bones = model.skinned ? boneIndices(obj) : std::vector<uint32_t>();
	if(model.skinned) {
		model.boneCount = obj.m_skeleton->bones.size();
	}

	std::map<TextureContainer *, Bucket> groups;
	for(const EERIE_FACE & face : obj.facelist) {
		TextureContainer * tex = nullptr;
		if(face.material && size_t(face.material) < obj.materials.size()) {
			tex = obj.materials[face.material];
		}
		if(skipFace(face, tex) || model.triangles >= kMaxModelTriangles) {
			continue;
		}
		appendFace(groups[tex], obj, face, model.skinned, bones);
		model.triangles++;
	}

	if(model.triangles == 0) {
		return model;
	}

	std::vector<std::vector<uint32_t>> indexLists;
	indexLists.reserve(groups.size());
	std::vector<remixapi_MeshInfoSurfaceTriangles> surfaces;
	surfaces.reserve(groups.size());

	for(auto & group : groups) {
		Bucket & bucket = group.second;
		if(bucket.vertices.size() < 3) {
			continue;
		}

		indexLists.emplace_back(bucket.vertices.size());
		std::vector<uint32_t> & indices = indexLists.back();
		for(uint32_t i = 0; i < uint32_t(indices.size()); i++) {
			indices[i] = i;
		}

		remixapi_MeshInfoSurfaceTriangles surface {};
		surface.vertices_values = bucket.vertices.data();
		surface.vertices_count = bucket.vertices.size();
		surface.indices_values = indices.data();
		surface.indices_count = indices.size();
		if(model.skinned) {
			surface.skinning_hasvalue = TRUE;
			surface.skinning_value.bonesPerVertex = 1;
			surface.skinning_value.blendWeights_values = bucket.weights.data();
			surface.skinning_value.blendWeights_count = uint32_t(bucket.weights.size());
			surface.skinning_value.blendIndices_values = bucket.bones.data();
			surface.skinning_value.blendIndices_count = uint32_t(bucket.bones.size());
		} else {
			surface.skinning_hasvalue = FALSE;
		}
		surface.material = debugEnabled(DebugNoMaterials) ? nullptr : materialFor(group.first, api);
		surfaces.push_back(surface);
	}

	if(surfaces.empty()) {
		return model;
	}

	remixapi_MeshInfo meshInfo {};
	meshInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
	// Stable for the life of the model: Remix matches meshes between frames by
	// hash to build motion vectors.
	meshInfo.hash = 0xA2000000ull ^ uint64_t(reinterpret_cast<uintptr_t>(&obj));
	meshInfo.surfaces_values = surfaces.data();
	meshInfo.surfaces_count = uint32_t(surfaces.size());

	const remixapi_ErrorCode status = api.iface().CreateMesh(&meshInfo, &model.mesh);
	if(status != REMIXAPI_ERROR_CODE_SUCCESS) {
		LogWarning << "Remix entities: CreateMesh failed for " << obj.file.string()
		           << " (" << RemixApi::errorString(status) << ')';
		model.mesh = nullptr;
	}

	return model;
}

ModelMesh & modelFor(RemixApi & api, const EERIE_3DOBJ & obj, unsigned frame, bool forceRigid) {

	auto it = g_models.find(&obj);
	if(it != g_models.end()) {
		ModelMesh & cached = it->second;
		const bool stale = cached.vertexCount != obj.vertexlist.size()
		                   || cached.faceCount != obj.facelist.size()
		                   || cached.forcedRigid != forceRigid;
		if(!stale) {
			cached.frame = frame;
			return cached;
		}
		destroyModel(api, cached);
		g_models.erase(it);
	}

	ModelMesh model = buildModel(api, obj, forceRigid);
	model.frame = frame;
	return g_models.emplace(&obj, model).first->second;
}

/*!
 * Weapons, torches and shields are not entities in the scene: they hang off
 * EERIE_3DOBJ::linked and the engine draws them in its own pass, which is why
 * the jail guard's axe was missing from the Remix window while the OpenGL one
 * held it.
 *
 * Mirrors the placement in AnimationRender.cpp:1091 exactly:
 *   rotation = the parent bone's quaternion
 *   scale    = the linked entity's scale
 *   pos      = attachVertex + rotation * (origin - lidx2) * scale
 * and then each vertex goes to pos + rotation * v * scale, with no origin
 * subtraction - odd, but it is what the engine does and fidelity wins.
 */
void drawLinked(RemixApi & api, const Entity & entity, unsigned frame,
                size_t & drawn, size_t & triangles) {

	const EERIE_3DOBJ & parent = *entity.obj;
	if(!parent.m_skeleton || parent.vertexWorldPositions.size() != parent.vertexlist.size()) {
		return;
	}

	const bool playerParent = (&entity == entities.player());

	if(!parent.linked.empty() && g_loggedLinks < 8) {
		LogInfo << "Remix entities: " << entity.idString() << " carries "
		        << parent.linked.size() << " linked object(s)";
		g_loggedLinks++;
	}

	for(const EERIE_LINKED & link : parent.linked) {

		if(drawn >= kMaxEntities || triangles >= kMaxTrianglesTotal) {
			return;
		}
		if(playerParent && link.io && !(link.io->ioflags & IO_ITEM)) {
			continue;
		}
		if(!link.lgroup || !link.obj || link.obj->vertexlist.empty()) {
			continue;
		}
		if(size_t(link.lgroup.handleData()) >= parent.m_skeleton->bones.size()) {
			continue;
		}
		if(size_t(link.lidx.handleData()) >= parent.vertexWorldPositions.size()
		   || size_t(link.lidx2.handleData()) >= link.obj->vertexlist.size()
		   || size_t(link.obj->origin.handleData()) >= link.obj->vertexlist.size()) {
			continue;
		}

		// Linked objects go through the engine's non-animated path, so keep the
		// mesh rigid even when the model happens to carry a skeleton.
		ModelMesh & model = modelFor(api, *link.obj, frame, true);
		if(!model.mesh) {
			continue;
		}

		const glm::quat rotation = parent.m_skeleton->bones[link.lgroup].anim.quat;
		const float scale = link.io ? link.io->scale : 1.f;
		const Vec3f attach = parent.vertexWorldPositions[link.lidx].v;
		const Vec3f offset = link.obj->vertexlist[link.obj->origin].v
		                     - link.obj->vertexlist[link.lidx2].v;
		const Vec3f pos = attach + (rotation * offset) * scale;

		glm::mat4 linear = glm::mat4_cast(rotation);
		for(int col = 0; col < 3; col++) {
			for(int row = 0; row < 3; row++) {
				linear[col][row] *= scale;
			}
		}

		remixapi_InstanceInfoBlendEXT blend {};
		remixapi_InstanceInfo instance {};
		fillOpaqueInstance(instance, blend, model.mesh);
		instance.transform = toRemixTransform(linear, toRemix(pos));
		api.iface().DrawInstance(&instance);

		drawn++;
		triangles += model.triangles;
	}
}

bool shouldExport(const Entity & entity, const Vec3f & playerPos) {

	if(&entity == entities.player()) {
		return false;
	}
	if(!entity.obj || entity.show != SHOW_FLAG_IN_SCENE) {
		return false;
	}
	if(entity.ioflags & (IO_CAMERA | IO_MARKER)) {
		return false;
	}
	if(entity.gameFlags & (GFLAG_INVISIBILITY | GFLAG_MEGAHIDE)) {
		return false;
	}
	if(glm::distance(entity.pos, playerPos) > entityExportRadius()) {
		return false;
	}

	const EERIE_3DOBJ * obj = entity.obj;
	if(obj->facelist.empty() || obj->vertexlist.empty()) {
		return false;
	}
	// An NPC without a skeleton has no pose to send; skip it rather than draw it
	// frozen in its bind pose.
	if((entity.ioflags & IO_NPC) && !obj->m_skeleton) {
		return false;
	}

	return true;
}

} // namespace

void drawEntities(RemixApi & api, const Vec3f & playerPos, unsigned frame) {

	if(!api.iface().DrawInstance || !entities.player()) {
		return;
	}

	struct Candidate {
		Entity * entity = nullptr;
		float dist = 0.f;
	};
	std::vector<Candidate> candidates;
	for(Entity & entity : entities) {
		if(shouldExport(entity, playerPos)) {
			candidates.push_back({ &entity, glm::distance(entity.pos, playerPos) });
		}
	}
	std::sort(candidates.begin(), candidates.end(), [](const Candidate & a, const Candidate & b) {
		return a.dist < b.dist;
	});

	std::vector<remixapi_Transform> boneMatrices;
	size_t drawn = 0;
	size_t clipped = 0;
	size_t triangles = 0;

	if(entities.player() && entities.player()->obj) {
		drawLinked(api, *entities.player(), frame, drawn, triangles);
	}

	for(const Candidate & cand : candidates) {

		if(drawn >= kMaxEntities || triangles >= kMaxTrianglesTotal) {
			clipped++;
			continue;
		}

		Entity & entity = *cand.entity;
		ModelMesh & model = modelFor(api, *entity.obj, frame, false);
		if(!model.mesh) {
			continue;
		}

		remixapi_InstanceInfoBlendEXT blend {};
		remixapi_InstanceInfo instance {};
		fillOpaqueInstance(instance, blend, model.mesh);

		remixapi_InstanceInfoBoneTransformsEXT bones {};
		if(model.skinned) {
			fillBoneTransforms(*entity.obj, boneMatrices);
			if(boneMatrices.size() != model.boneCount) {
				// The skeleton changed under us; drop the mesh and rebuild next frame.
				destroyModel(api, model);
				continue;
			}
			bones.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
			bones.pNext = instance.pNext;
			bones.boneTransforms_values = boneMatrices.data();
			bones.boneTransforms_count = uint32_t(boneMatrices.size());
			instance.pNext = &bones;
			// Bone transforms are already world space.
			instance.transform = identityTransform();
		} else {
			instance.transform = entityTransform(entity);
		}

		api.iface().DrawInstance(&instance);
		drawn++;
		triangles += model.triangles;

		// Held weapons, torches and shields hang off the model, not the entity list.
		drawLinked(api, entity, frame, drawn, triangles);

		if(g_loggedEntities < 24) {
			LogInfo << "Remix entities: " << entity.idString()
			        << (model.skinned ? " skinned " : " rigid ") << model.triangles << " tris at "
			        << entity.pos.x << ',' << entity.pos.y << ',' << entity.pos.z;
			g_loggedEntities++;
		}
	}

	// Models nobody drew this frame: free them, so a freed EERIE_3DOBJ cannot be
	// replaced at the same address behind a stale mesh.
	for(auto it = g_models.begin(); it != g_models.end(); ) {
		if(it->second.frame != frame) {
			destroyModel(api, it->second);
			it = g_models.erase(it);
		} else {
			++it;
		}
	}

	if(frame == 0 || (frame % 120) == 0) {
		LogInfo << "Remix entities: drew " << drawn << " objects, " << triangles << " triangles, "
		        << g_models.size() << " meshes cached, clipped " << clipped
		        << " over the " << kMaxEntities << " object / " << kMaxTrianglesTotal
		        << " triangle budget";
	}
}

void destroyEntityMeshes(RemixApi & api) {
	for(auto & entry : g_models) {
		destroyModel(api, entry.second);
	}
	g_models.clear();
}

} // namespace remix

#endif // ARX_HAVE_RTX_REMIX
