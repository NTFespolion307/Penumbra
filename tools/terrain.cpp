// penumbra-terrain: real terrain from swisstopo tiles into a glTF scene (M14, DECISIONS D-122).
//
//   penumbra-terrain --tiles <dir> --out <dir> --name <base> --bbox E0,N0,E1,N1 [--grid 4] [--texture 2]
//                    [--camera E,N,H --target E,N,H --yfov 30]
//   penumbra-terrain --stac-next <page.json>              the next page's URL of a STAC item listing, if any
//   penumbra-terrain --stac-list --bbox E0,N0,E1,N1 <page.json>...
//                    "sha256 url" of the newest 2 m elevation and photo tile per km inside the bbox
//
// scripts/get-scenes fetches the STAC pages and the tiles with curl and checks them; this tool reads the JSON, so
// the scripts need no JSON parser.
//
// Reads swissALTI3D 2 m elevation tiles (swissalti3d_*_E-N_2_2056_5728.tif) and SWISSIMAGE 2 m photo tiles
// (swissimage-dop10_*_E-N_2_2056.tif) for every 1 km tile E0..E1-1, N0..N1-1 (LV95 kilometres), and writes
// <base>.gltf + <base>.bin (one grid mesh, a vertex every --grid metres, smooth normals) + <base>.jpg (the
// photos, a pixel every --texture metres). Axes: x east, y metres above sea level, -z north (so a sky azimuth is a
// compass bearing), centred on the bbox. --camera/--target place a perspective camera in LV95 metres.
// Data: (c) swisstopo, open data (https://www.swisstopo.admin.ch/en/terms-of-use-free-geodata-and-geoservices).

#include <tiffio.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr int kTile = 500;  // samples per 1 km tile at 2 m

[[noreturn]] void die(const std::string& msg) {
  std::fprintf(stderr, "penumbra-terrain: %s\n", msg.c_str());
  std::exit(1);
}

// The file in `dir` whose name has "_E-N_" and ends with `suffix`.
fs::path findTile(const fs::path& dir, int e, int n, const std::string& suffix) {
  const std::string key = "_" + std::to_string(e) + "-" + std::to_string(n) + "_";
  for (const auto& f : fs::directory_iterator(dir)) {
    const std::string name = f.path().filename().string();
    if (name.find(key) != std::string::npos && name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
      return f.path();
  }
  die("missing tile " + std::to_string(e) + "-" + std::to_string(n) + " (*" + suffix + ") in " + dir.string());
}

// A tiled or stripped TIFF of 500 x 500 samples into dst (row stride `stride`): float elevation (1 channel) or
// RGB bytes (3 channels; JPEG/YCbCr is decoded to RGB by libtiff).
template <class T>
void readTile(const fs::path& path, int channels, T* dst, size_t stride) {
  TIFF* tif = TIFFOpen(path.string().c_str(), "r");
  if (!tif) die("cannot open " + path.string());
  uint32_t w = 0, h = 0;
  TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
  TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
  if (w != kTile || h != kTile) die(path.string() + ": expected 500 x 500, found " + std::to_string(w) + " x " + std::to_string(h));
  uint16_t compression = 0;
  TIFFGetField(tif, TIFFTAG_COMPRESSION, &compression);
  if (compression == COMPRESSION_JPEG) TIFFSetField(tif, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB);
  if (!TIFFIsTiled(tif)) die(path.string() + ": expected a tiled (cloud-optimized) GeoTIFF");
  uint32_t tw = 0, th = 0;
  TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tw);
  TIFFGetField(tif, TIFFTAG_TILELENGTH, &th);
  std::vector<T> buf(size_t(tw) * th * channels);
  for (uint32_t y0 = 0; y0 < h; y0 += th)
    for (uint32_t x0 = 0; x0 < w; x0 += tw) {
      if (TIFFReadTile(tif, buf.data(), x0, y0, 0, 0) < 0) die(path.string() + ": cannot read a tile");
      for (uint32_t y = 0; y < th && y0 + y < h; ++y)
        for (uint32_t x = 0; x < tw && x0 + x < w; ++x)
          for (int c = 0; c < channels; ++c)
            dst[(size_t(y0 + y) * stride + (x0 + x)) * channels + c] = buf[(size_t(y) * tw + x) * channels + c];
    }
  TIFFClose(tif);
}

std::vector<double> parseList(const std::string& s, size_t count, const char* what) {
  std::vector<double> v;
  size_t pos = 0;
  while (pos <= s.size()) {
    const size_t comma = s.find(',', pos);
    v.push_back(std::stod(s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos)));
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  if (v.size() != count) die(std::string(what) + ": expected " + std::to_string(count) + " comma-separated numbers");
  return v;
}

// Unit quaternion (x, y, z, w) turning glTF's camera (looking down -Z, +Y up) to look along `dir`.
std::array<double, 4> lookRotation(double dx, double dy, double dz) {
  const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
  const double fx = dx / len, fy = dy / len, fz = dz / len;  // forward
  // right = forward x up(0,1,0), up = right x forward; camera axes: +X right, +Y up, -Z forward.
  double rx = -fz, ry = 0, rz = fx;
  const double rl = std::sqrt(rx * rx + rz * rz);
  rx /= rl;
  rz /= rl;
  const double ux = ry * fz - rz * fy, uy = rz * fx - rx * fz, uz = rx * fy - ry * fx;
  // Rotation matrix columns: right, up, -forward.
  const double m00 = rx, m01 = ux, m02 = -fx;
  const double m10 = ry, m11 = uy, m12 = -fy;
  const double m20 = rz, m21 = uz, m22 = -fz;
  const double tr = m00 + m11 + m22;
  double qw, qx, qy, qz;
  if (tr > 0) {
    const double s = std::sqrt(tr + 1) * 2;
    qw = s / 4, qx = (m21 - m12) / s, qy = (m02 - m20) / s, qz = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    const double s = std::sqrt(1 + m00 - m11 - m22) * 2;
    qw = (m21 - m12) / s, qx = s / 4, qy = (m01 + m10) / s, qz = (m02 + m20) / s;
  } else if (m11 > m22) {
    const double s = std::sqrt(1 + m11 - m00 - m22) * 2;
    qw = (m02 - m20) / s, qx = (m01 + m10) / s, qy = s / 4, qz = (m12 + m21) / s;
  } else {
    const double s = std::sqrt(1 + m22 - m00 - m11) * 2;
    qw = (m10 - m01) / s, qx = (m02 + m20) / s, qy = (m12 + m21) / s, qz = s / 4;
  }
  return {qx, qy, qz, qw};
}

json readJson(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) die("cannot open " + p.string());
  try {
    return json::parse(in);
  } catch (const json::exception& e) {
    die(p.string() + ": " + e.what());
  }
}

int stacNext(const fs::path& page) {
  const json j = readJson(page);
  for (const auto& l : j.value("links", json::array()))
    if (l.value("rel", "") == "next") std::printf("%s\n", l.value("href", "").c_str());
  return 0;
}

// Items are named like swissalti3d_2019_2642-1157 or swissimage-dop10_2024_2642-1157 (year, E-N km).
int stacList(const std::vector<double>& bb, const std::vector<fs::path>& pages) {
  struct Best {
    int year = -1;
    std::string url, sha;
  };
  std::map<std::string, Best> best;  // "kind E-N" -> newest
  for (const auto& page : pages)
    for (const auto& f : readJson(page).value("features", json::array())) {
      const std::string id = f.value("id", "");
      int year = 0, e = 0, n = 0;
      const size_t u1 = id.find('_'), u2 = id.find('_', u1 + 1);
      if (u1 == std::string::npos || u2 == std::string::npos ||
          std::sscanf(id.c_str() + u1 + 1, "%d_%d-%d", &year, &e, &n) != 3)
        continue;
      if (e < bb[0] || e >= bb[2] || n < bb[1] || n >= bb[3]) continue;
      const json assets = f.value("assets", json::object());  // kept: items() refers to it
      for (const auto& [key, a] : assets.items()) {
        const bool alti = key.size() > 16 && key.compare(key.size() - 16, 16, "_2_2056_5728.tif") == 0;
        const bool photo = key.size() > 11 && key.compare(key.size() - 11, 11, "_2_2056.tif") == 0;
        if (!alti && !photo) continue;
        std::string sha = a.value("checksum:multihash", "");
        if (sha.size() != 68 || sha.compare(0, 4, "1220") != 0) die(id + ": no SHA-256 in the listing");
        sha = sha.substr(4);
        std::transform(sha.begin(), sha.end(), sha.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        Best& b = best[std::string(alti ? "alti " : "photo ") + std::to_string(e) + "-" + std::to_string(n)];
        if (year > b.year) b = {year, a.value("href", ""), sha};
      }
    }
  const size_t want = size_t((bb[2] - bb[0]) * (bb[3] - bb[1])) * 2;
  if (best.size() != want)
    die("the listing has " + std::to_string(best.size()) + " of the " + std::to_string(want) + " tiles of the bbox");
  for (const auto& [k, b] : best) std::printf("%s %s\n", b.sha.c_str(), b.url.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--stac-next") return stacNext(argv[2]);
  if (argc >= 5 && std::string(argv[1]) == "--stac-list" && std::string(argv[2]) == "--bbox") {
    std::vector<fs::path> pages(argv + 4, argv + argc);
    return stacList(parseList(argv[3], 4, "--bbox"), pages);
  }
  fs::path tiles, out;
  std::string name = "terrain", bboxArg, cameraArg, targetArg;
  double grid = 4, texture = 2, yfov = 30;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc) die(a + " needs a value");
      return argv[++i];
    };
    if (a == "--tiles") tiles = next();
    else if (a == "--out") out = next();
    else if (a == "--name") name = next();
    else if (a == "--bbox") bboxArg = next();
    else if (a == "--grid") grid = std::stod(next());
    else if (a == "--texture") texture = std::stod(next());
    else if (a == "--camera") cameraArg = next();
    else if (a == "--target") targetArg = next();
    else if (a == "--yfov") yfov = std::stod(next());
    else die("unknown argument " + a + " (see the comment at the top of tools/terrain.cpp)");
  }
  if (tiles.empty() || out.empty() || bboxArg.empty()) die("--tiles, --out and --bbox are required");
  const std::vector<double> bb = parseList(bboxArg, 4, "--bbox");
  const int e0 = int(bb[0]), n0 = int(bb[1]), e1 = int(bb[2]), n1 = int(bb[3]);
  if (e1 <= e0 || n1 <= n0) die("--bbox: E0,N0,E1,N1 with E1 > E0 and N1 > N0 (LV95 kilometres)");
  const int gs = std::max(1, int(std::lround(grid / 2))), ts = std::max(1, int(std::lround(texture / 2)));
  if (kTile % gs != 0 || kTile % ts != 0) die("--grid and --texture must divide 1000 m in 2 m steps (2, 4, 10...)");
  fs::create_directories(out);

  // Mosaics: row 0 is the north edge.
  const size_t W = size_t(e1 - e0) * kTile, H = size_t(n1 - n0) * kTile;
  std::vector<float> height(W * H);
  std::vector<uint8_t> photo(W * H * 3);
  for (int n = n0; n < n1; ++n)
    for (int e = e0; e < e1; ++e) {
      const size_t ox = size_t(e - e0) * kTile, oy = size_t(n1 - 1 - n) * kTile;
      readTile(findTile(tiles, e, n, "_2_2056_5728.tif"), 1, height.data() + oy * W + ox, W);
      readTile(findTile(tiles, e, n, "_2_2056.tif"), 3, photo.data() + (oy * W + ox) * 3, W);
    }
  std::printf("read %d x %d tiles: %zu x %zu samples\n", e1 - e0, n1 - n0, W, H);
  const auto peak = std::max_element(height.begin(), height.end());
  const size_t pi = size_t(peak - height.begin());
  std::printf("highest point %.1f m at E %.0f N %.0f\n", *peak, e0 * 1000.0 + (pi % W + 0.5) * 2,
              n1 * 1000.0 - (pi / W + 0.5) * 2);

  // Mesh: a vertex every gs samples. Sample (i, j) sits at E = e0 km + (i + 0.5) 2 m, N = n1 km - (j + 0.5) 2 m.
  const double ec = (e0 + e1) * 500.0, nc = (n0 + n1) * 500.0;  // centre, metres
  const size_t vx = (W - 1) / gs + 1, vy = (H - 1) / gs + 1;
  std::vector<float> pos, nrm, uv;
  pos.reserve(vx * vy * 3);
  nrm.reserve(vx * vy * 3);
  uv.reserve(vx * vy * 2);
  const auto h = [&](long i, long j) {
    i = std::clamp<long>(i, 0, long(W) - 1);
    j = std::clamp<long>(j, 0, long(H) - 1);
    return double(height[size_t(j) * W + size_t(i)]);
  };
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (size_t j = 0; j < vy; ++j)
    for (size_t i = 0; i < vx; ++i) {
      const long si = long(i * gs), sj = long(j * gs);
      const double E = e0 * 1000.0 + (si + 0.5) * 2, N = n1 * 1000.0 - (sj + 0.5) * 2;
      const float p[3] = {float(E - ec), float(h(si, sj)), float(-(N - nc))};
      for (int c = 0; c < 3; ++c) {
        pos.push_back(p[c]);
        lo[c] = std::min(lo[c], p[c]);
        hi[c] = std::max(hi[c], p[c]);
      }
      // Normal from central differences over one grid step (x east, z south).
      const double dx = (h(si + gs, sj) - h(si - gs, sj)) / (4.0 * gs);
      const double dz = (h(si, sj + gs) - h(si, sj - gs)) / (4.0 * gs);
      const double l = std::sqrt(dx * dx + 1 + dz * dz);
      nrm.push_back(float(-dx / l));
      nrm.push_back(float(1 / l));
      nrm.push_back(float(-dz / l));
      uv.push_back(float((si + 0.5) / W));
      uv.push_back(float((sj + 0.5) / H));
    }
  std::vector<uint32_t> idx;
  idx.reserve((vx - 1) * (vy - 1) * 6);
  for (size_t j = 0; j + 1 < vy; ++j)
    for (size_t i = 0; i + 1 < vx; ++i) {
      const uint32_t a = uint32_t(j * vx + i), b = a + 1, c = uint32_t((j + 1) * vx + i), d = c + 1;
      idx.insert(idx.end(), {a, c, b, b, c, d});  // counter-clockwise seen from above (+Y)
    }
  std::printf("mesh %zu x %zu vertices (%.0f m apart), %zu triangles\n", vx, vy, gs * 2.0, idx.size() / 3);

  // Texture.
  const size_t tw = W / ts, th = H / ts;
  std::vector<uint8_t> tex(tw * th * 3);
  for (size_t y = 0; y < th; ++y)
    for (size_t x = 0; x < tw; ++x)
      for (int c = 0; c < 3; ++c) {
        unsigned sum = 0;
        for (int dy = 0; dy < ts; ++dy)
          for (int dx = 0; dx < ts; ++dx) sum += photo[((y * ts + dy) * W + x * ts + dx) * 3 + c];
        tex[(y * tw + x) * 3 + c] = uint8_t(sum / unsigned(ts * ts));
      }
  const std::string jpgName = name + ".jpg";
  if (!stbi_write_jpg((out / jpgName).string().c_str(), int(tw), int(th), 3, tex.data(), 92))
    die("cannot write " + (out / jpgName).string());
  std::printf("texture %zu x %zu (%.0f m per pixel)\n", tw, th, ts * 2.0);

  // glTF with one external buffer.
  const std::string binName = name + ".bin";
  std::ofstream bin(out / binName, std::ios::binary);
  const auto put = [&](const void* data, size_t bytes) {
    const size_t at = size_t(bin.tellp());
    bin.write(static_cast<const char*>(data), std::streamsize(bytes));
    return at;
  };
  const size_t oPos = put(pos.data(), pos.size() * 4), oNrm = put(nrm.data(), nrm.size() * 4);
  const size_t oUv = put(uv.data(), uv.size() * 4), oIdx = put(idx.data(), idx.size() * 4);
  const size_t binBytes = size_t(bin.tellp());
  bin.close();
  json g;
  g["asset"] = {{"version", "2.0"}, {"generator", "penumbra-terrain"}, {"copyright", "(c) swisstopo"}};
  g["buffers"] = json::array({{{"uri", binName}, {"byteLength", binBytes}}});
  const auto view = [&](size_t offset, size_t bytes, int target) {
    g["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", offset}, {"byteLength", bytes}, {"target", target}});
    return int(g["bufferViews"].size()) - 1;
  };
  const size_t nv = vx * vy;
  g["accessors"] = json::array();
  g["accessors"].push_back({{"bufferView", view(oPos, nv * 12, 34962)}, {"componentType", 5126}, {"count", nv},
                            {"type", "VEC3"}, {"min", {lo[0], lo[1], lo[2]}}, {"max", {hi[0], hi[1], hi[2]}}});
  g["accessors"].push_back({{"bufferView", view(oNrm, nv * 12, 34962)}, {"componentType", 5126}, {"count", nv}, {"type", "VEC3"}});
  g["accessors"].push_back({{"bufferView", view(oUv, nv * 8, 34962)}, {"componentType", 5126}, {"count", nv}, {"type", "VEC2"}});
  g["accessors"].push_back({{"bufferView", view(oIdx, idx.size() * 4, 34963)}, {"componentType", 5125},
                            {"count", idx.size()}, {"type", "SCALAR"}});
  g["images"] = json::array({{{"uri", jpgName}}});
  g["samplers"] = json::array({{{"wrapS", 33071}, {"wrapT", 33071}}});
  g["textures"] = json::array({{{"source", 0}, {"sampler", 0}}});
  g["materials"] = json::array({{{"name", "terrain"},
                                 {"pbrMetallicRoughness",
                                  {{"baseColorTexture", {{"index", 0}}}, {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}}}}});
  g["meshes"] = json::array({{{"name", "terrain"},
                              {"primitives", json::array({{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
                                                           {"indices", 3},
                                                           {"material", 0}}})}}});
  g["nodes"] = json::array({{{"name", "terrain"}, {"mesh", 0}}});
  if (!cameraArg.empty()) {
    const std::vector<double> c = parseList(cameraArg, 3, "--camera");
    const long ci = long((c[0] - e0 * 1000.0) / 2), cj = long((n1 * 1000.0 - c[1]) / 2);
    if (ci >= 0 && cj >= 0 && ci < long(W) && cj < long(H))
      std::printf("camera %.0f m above sea level, %.0f m above the ground\n", c[2], c[2] - h(ci, cj));
    const std::vector<double> t = targetArg.empty() ? std::vector<double>{c[0], c[1] + 1000, c[2]}
                                                    : parseList(targetArg, 3, "--target");
    const std::array<double, 4> q = lookRotation(t[0] - c[0], t[2] - c[2], -(t[1] - c[1]));
    g["cameras"] = json::array({{{"name", "view"}, {"type", "perspective"},
                                 {"perspective", {{"yfov", yfov * 3.14159265358979 / 180}, {"znear", 0.5}}}}});
    g["nodes"].push_back({{"name", "view"}, {"camera", 0}, {"translation", {c[0] - ec, c[2], -(c[1] - nc)}},
                          {"rotation", {q[0], q[1], q[2], q[3]}}});
  }
  g["scenes"] = json::array({{{"nodes", cameraArg.empty() ? json::array({0}) : json::array({0, 1})}}});
  g["scene"] = 0;
  std::ofstream(out / (name + ".gltf"), std::ios::binary) << g.dump(1) << "\n";
  std::printf("wrote %s (%.0f MB of geometry)\n", (out / (name + ".gltf")).string().c_str(), binBytes / 1e6);
  return 0;
}
