#include "backends/gpu/gpu_bvh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <mutex>

#include "backends/backend.h"
#include "tiny_bvh.h"

namespace pen {

namespace {

constexpr uint32_t kListMaxTriangles = 3;  // tinybvh cannot make a CWBVH whose root is a leaf
constexpr uint32_t kTlasLeafSize = 2;

// tinybvh reports misuse through a handler that must not return; ours turns it into a backend error, so a bad mesh
// fails the device instead of ending the process.
[[noreturn]] void throwTinyBvhError(const char* message) {
  throw BackendError(fmt::format("tinybvh: {}", message));
}

struct Box {
  Vec3f lo{std::numeric_limits<float>::max()}, hi{-std::numeric_limits<float>::max()};
  void grow(Vec3f p) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  void grow(const Box& b) {
    grow(b.lo);
    grow(b.hi);
  }
  bool empty() const { return lo.x > hi.x; }
};

// float4 blocks per triangle in tinybvh's CWBVH triangle array at the pinned version: its header turns on
// CWBVH_COMPRESSED_TRIS (4 blocks, a precomputed transform, triangle index in block 3's w); without it, 3 blocks
// (v2 - v0, v1 - v0, v0 with the index in w).
#if defined(CWBVH_COMPRESSED_TRIS)
constexpr uint32_t kTinyTriBlocks = 4, kTinyIdBlock = 3;
#else
constexpr uint32_t kTinyTriBlocks = 3, kTinyIdBlock = 2;
#endif

// Appends mesh triangle t in our format (shaders/bvh.slang): v2 - v0, v1 - v0, v0 with the triangle id in w.
void appendTriangle(const std::vector<tinybvh::bvhvec4>& verts, const std::vector<uint32_t>& idx, uint32_t t,
                    uint32_t id, std::vector<Vec4f>& out) {
  const tinybvh::bvhvec4 v0 = verts[idx[t * 3]], v1 = verts[idx[t * 3 + 1]], v2 = verts[idx[t * 3 + 2]];
  float w;
  std::memcpy(&w, &id, 4);
  out.push_back({v2.x - v0.x, v2.y - v0.y, v2.z - v0.z, 0});
  out.push_back({v1.x - v0.x, v1.y - v0.y, v1.z - v0.z, 0});
  out.push_back({v0.x, v0.y, v0.z, w});
}

}  // namespace

GpuBvh buildGpuBvh(const Scene& scene) {
  static std::once_flag handlerOnce;
  std::call_once(handlerOnce, [] { tinybvh::tinybvh_set_fatal_error_handler(throwTinyBvhError); });

  GpuBvh out;
  std::vector<Box> meshBox(scene.meshes.size());

  // ---- One BLAS per mesh: all its primitives' triangles, ids into tri_map ----
  for (size_t m = 0; m < scene.meshes.size(); ++m) {
    const Mesh& mesh = scene.meshes[m];
    std::vector<tinybvh::bvhvec4> verts;
    std::vector<uint32_t> idx;
    const uint32_t idBase = static_cast<uint32_t>(out.tri_map.size() / 2);
    bool alpha = false;
    for (uint32_t p = 0; p < mesh.primitives.size(); ++p) {
      const MeshPrimitive& prim = mesh.primitives[p];
      const uint32_t vbase = static_cast<uint32_t>(verts.size());
      for (const Vec3f& v : prim.positions) {
        verts.emplace_back(v.x, v.y, v.z, 0.0f);
        meshBox[m].grow(v);
      }
      for (uint32_t i : prim.indices) idx.push_back(vbase + i);
      for (uint32_t t = 0; t < prim.triangleCount(); ++t) {
        out.tri_map.push_back(p);
        out.tri_map.push_back(t);
      }
      alpha = alpha || (prim.triangleCount() > 0 && scene.materials[prim.material].usesAlpha());
    }
    const uint32_t triCount = static_cast<uint32_t>(idx.size() / 3);
    gpu::Blas b{};
    b.tri_count = triCount;
    b.tri_base = static_cast<uint32_t>(out.tris.size());
    b.node_base = static_cast<uint32_t>(out.nodes.size());
    if (triCount == 0) {
      b.kind = 0;
    } else if (triCount <= kListMaxTriangles) {
      b.kind = 2;
      for (uint32_t t = 0; t < triCount; ++t) appendTriangle(verts, idx, t, idBase + t, out.tris);
    } else {
      tinybvh::BVH8_CWBVH bvh;
      bvh.Build(tinybvh::bvhvec4slice(verts.data(), static_cast<uint32_t>(verts.size())), idx.data(), triCount);
      b.kind = 1;
      // Nodes as they are, except each node's leaf triangle base (n1.y), which becomes a triangle number: our
      // triangles are 3 float4 each whatever tinybvh's own triangle format is.
      const size_t firstNode = out.nodes.size();
      out.nodes.insert(out.nodes.end(), reinterpret_cast<const Vec4f*>(bvh.bvh8Data),
                       reinterpret_cast<const Vec4f*>(bvh.bvh8Data) + bvh.usedBlocks);
      for (size_t n = firstNode + 1; n < out.nodes.size(); n += 5) {
        uint32_t base;
        std::memcpy(&base, &out.nodes[n].y, 4);
        base /= kTinyTriBlocks;
        std::memcpy(&out.nodes[n].y, &base, 4);
      }
      // Triangles in tinybvh's order, rebuilt from the mesh (tinybvh keeps the mesh triangle index).
      for (uint32_t j = 0; j * kTinyTriBlocks < bvh.usedTriBlocks; ++j) {
        uint32_t t;
        std::memcpy(&t, &bvh.bvh8Tris[j * kTinyTriBlocks + kTinyIdBlock].w, 4);
        if (t >= triCount) throw BackendError("tinybvh: CWBVH triangle index out of range");
        appendTriangle(verts, idx, t, idBase + t, out.tris);
      }
    }
    if (alpha) b.kind |= 0x100;
    out.blas.push_back(b);
    if (out.tris.size() > std::numeric_limits<uint32_t>::max() || out.nodes.size() > std::numeric_limits<uint32_t>::max())
      throw BackendError("the scene's BVH is larger than 64 GB; not supported");
  }

  // ---- TLAS: instances with triangles, world boxes, median splits on the longest axis ----
  struct Item {
    Box box;
    Vec3f centroid;
    uint32_t instance;
  };
  std::vector<Item> items;
  for (uint32_t i = 0; i < scene.instances.size(); ++i) {
    const Instance& inst = scene.instances[i];
    if ((out.blas[inst.mesh].kind & 0xff) == 0) continue;
    const Box& mb = meshBox[inst.mesh];
    Box wb;
    for (int c = 0; c < 8; ++c)
      wb.grow(inst.to_world.point({(c & 1) ? mb.hi.x : mb.lo.x, (c & 2) ? mb.hi.y : mb.lo.y, (c & 4) ? mb.hi.z : mb.lo.z}));
    // A little slack: the transformed corners round, and a box that misses by an ulp loses the hit.
    const Vec3f pad = (wb.hi - wb.lo) * 1e-5f + Vec3f(1e-6f);
    wb.lo = wb.lo - pad;
    wb.hi = wb.hi + pad;
    items.push_back({wb, (wb.lo + wb.hi) * 0.5f, i});
  }
  if (items.empty()) {
    // An empty leaf: traversal checks tlas_index's size first (bvh.slang), so nothing is read.
    out.tlas.push_back({{0, 0, 0}, 0, {0, 0, 0}, 1});
    out.notes.push_back("no instance has triangles; every ray misses");
    return out;
  }
  struct Task {
    uint32_t node, begin, end;
  };
  out.tlas.emplace_back();
  std::vector<Task> tasks{{0, 0, static_cast<uint32_t>(items.size())}};
  while (!tasks.empty()) {
    const Task t = tasks.back();
    tasks.pop_back();
    Box box, cbox;
    for (uint32_t i = t.begin; i < t.end; ++i) {
      box.grow(items[i].box);
      cbox.grow(items[i].centroid);
    }
    gpu::TlasNode& n = out.tlas[t.node];
    n = {{box.lo.x, box.lo.y, box.lo.z}, 0, {box.hi.x, box.hi.y, box.hi.z}, 0};
    if (t.end - t.begin <= kTlasLeafSize) {
      n.left_first = static_cast<uint32_t>(out.tlas_index.size());
      n.count = t.end - t.begin;
      for (uint32_t i = t.begin; i < t.end; ++i) out.tlas_index.push_back(items[i].instance);
      continue;
    }
    const Vec3f ext = cbox.hi - cbox.lo;
    const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : (ext.y >= ext.z ? 1 : 2);
    const uint32_t mid = (t.begin + t.end) / 2;
    std::nth_element(items.begin() + t.begin, items.begin() + mid, items.begin() + t.end,
                     [axis](const Item& a, const Item& b) { return a.centroid[axis] < b.centroid[axis]; });
    const uint32_t left = static_cast<uint32_t>(out.tlas.size());
    out.tlas[t.node].left_first = left;  // n may dangle after the emplace_backs
    out.tlas.emplace_back();
    out.tlas.emplace_back();
    tasks.push_back({left + 1, mid, t.end});
    tasks.push_back({left, t.begin, mid});
  }
  return out;
}

}  // namespace pen
