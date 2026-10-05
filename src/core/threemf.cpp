// 3MF loader: core spec + production extension (multi-file models, used by
// Bambu Studio and OrcaSlicer) + slicer colour metadata:
//   Bambu/Orca: Metadata/project_settings.config (filament_colour),
//               Metadata/model_settings.config (extruder per object/part,
//               part subtypes), triangle "paint_color" (MMU painting)
//   PrusaSlicer: Metadata/Slic3r_PE.config, "slic3rpe:mmu_segmentation"
//   Generic:    <basematerials>/<colorgroup> with pid/pindex and pid/p1

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bambu_paint.h"
#include "formats.h"
#include "parallel.h"
#include "xml_scan.h"
#include "zip.h"

namespace v3d {

namespace {

std::string normPath(std::string_view p) {
    if (!p.empty() && p[0] == '/') p.remove_prefix(1);
    std::string s(p);
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parseColor(std::string_view s, uint32_t& out) {
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
    if (s.empty() || s[0] != '#') return false;
    s.remove_prefix(1);
    if (s.size() != 6 && s.size() != 8) return false;
    uint32_t v = 0;
    for (char ch : s) {
        int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | uint32_t(d);
    }
    if (s.size() == 8) v >>= 8;  // drop alpha: previews are opaque
    out = rgba(uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v));
    return true;
}

// 3MF matrices are 4x3, row-vector convention: "m00 m01 m02 m10 ... m32".
// Read in order they fill a column-major 4x4 directly.
Mat4 parseTransform(std::string_view s) {
    Mat4 m;
    if (s.empty()) return m;
    float v[12];
    const char* p = s.data();
    const char* e = p + s.size();
    for (float& x : v)
        if (!(p = parseFloat(p, e, x))) return Mat4{};
    static constexpr int kSlot[12] = {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14};
    for (int i = 0; i < 12; ++i) m.m[kSlot[i]] = v[i];
    return m;
}

struct Component {
    std::string path;  // empty: same file
    uint32_t objectId = 0;
    Mat4 transform;
};

struct ObjectDef {
    int mesh = -1;  // local mesh index in its ModelFile
    std::vector<Component> components;
    bool hasProp = false;
    uint32_t pid = 0, pindex = 0;
};

struct BuildItem {
    std::string path;
    uint32_t objectId = 0;
    Mat4 transform;
};

struct ParsedMesh {
    Mesh mesh;
    // triColor holds 1 + an index into ModelFile::colors instead of a scene
    // palette index; remapped when the file is merged into the scene.
    bool localColors = false;
};

struct ModelFile {
    std::unordered_map<uint32_t, ObjectDef> objects;
    std::vector<BuildItem> build;
    std::vector<ParsedMesh> meshes;
    std::vector<uint32_t> colors;  // local material palette
    std::unordered_map<uint64_t, uint32_t> propIndex;  // (pid << 32 | index) -> colors[]
    float unitScale = 1;
    std::string error;
};

uint64_t key2(uint32_t a, uint32_t b) { return uint64_t(a) << 32 | b; }

// ---------------------------------------------------------------- mesh body

struct PaintRef {
    uint32_t tri;
    std::string_view code;
};
struct TriProp {
    uint32_t tri, pid, p1;
};

// Splits [b, e) into chunks starting at '<' so each can be scanned independently.
std::vector<const char*> splitAtTags(const char* b, const char* e, size_t parts) {
    std::vector<const char*> cuts{b};
    for (size_t i = 1; i < parts; ++i) {
        const char* c = b + (e - b) * ptrdiff_t(i) / ptrdiff_t(parts);
        c = std::max(c, cuts.back());
        const char* lt = static_cast<const char*>(std::memchr(c, '<', size_t(e - c)));
        cuts.push_back(lt ? lt : e);
    }
    cuts.push_back(e);
    return cuts;
}

size_t chunkCount(size_t bytes) {
    constexpr size_t kMinChunk = 1u << 20;
    return std::clamp<size_t>(bytes / kMinChunk, 1, hardwareThreads());
}

// Appends the parts to `out` (copied in parallel), followed by `tail`
// uninitialised elements for the caller to fill.
template <class T>
void appendParts(Buf<T>& out, std::vector<Buf<T>>&& parts, size_t tail = 0) {
    if (out.empty() && parts.size() == 1) {
        out = std::move(parts[0]);
        out.resize(out.size() + tail);
        return;
    }
    std::vector<size_t> offset(parts.size() + 1, out.size());
    for (size_t i = 0; i < parts.size(); ++i) offset[i + 1] = offset[i] + parts[i].size();
    out.resize(offset.back() + tail);
    parallelFor(parts.size(), [&](size_t i) {
        if (!parts[i].empty())  // a slice may hold only vertices or only triangles
            std::memcpy(out.data() + offset[i], parts[i].data(), parts[i].size() * sizeof(T));
    });
}

// Walks the attributes of one tag: calls fn(name, value) with the local name.
template <class Fn>
bool forEachAttr(const char* p, const char* gt, Fn fn) {
    while (p < gt) {
        const char* eq = static_cast<const char*>(std::memchr(p, '=', size_t(gt - p)));
        if (!eq) break;
        const char* ne = eq;
        while (ne > p && isSpace(ne[-1])) --ne;
        const char* nb = ne;
        while (nb > p && !isSpace(nb[-1]) && nb[-1] != ':') --nb;
        const char* q = skipSpace(eq + 1, gt);
        if (q >= gt || (*q != '"' && *q != '\'')) return false;
        char quote = *q++;
        const char* qe = static_cast<const char*>(std::memchr(q, quote, size_t(gt - q)));
        if (!qe) return false;
        if (!fn(std::string_view(nb, size_t(ne - nb)), std::string_view(q, size_t(qe - q)))) return false;
        p = qe + 1;
    }
    return true;
}

// First occurrence of `needle` in [b, e), searching a slice in parallel.
const char* searchSlice(const char* b, const char* e, std::string_view needle) {
    const size_t n = size_t(e - b);
    const size_t parts = std::clamp<size_t>(n / (8u << 20), 1, hardwareThreads());
    if (parts == 1) return static_cast<const char*>(memmem(b, n, needle.data(), needle.size()));
    std::vector<const char*> hits(parts, nullptr);
    parallelFor(parts, [&](size_t i) {
        // Slices overlap by needle.size() - 1 so no match straddles a cut unseen.
        const char* cb = b + n * i / parts;
        const char* ce = std::min(e, b + n * (i + 1) / parts + needle.size() - 1);
        hits[i] = static_cast<const char*>(memmem(cb, size_t(ce - cb), needle.data(), needle.size()));
    });
    for (const char* h : hits)
        if (h) return h;
    return nullptr;
}

// First occurrence of `needle` in [b, e). Windows double in size, so the bytes
// scanned stay within ~2x the distance to the match: cheap for the many small
// meshes of one document, parallel for single meshes of hundreds of MB.
const char* findText(const char* b, const char* e, std::string_view needle) {
    size_t window = 1u << 20;
    for (const char* p = b; p < e; window *= 2) {
        const char* we = size_t(e - p) <= window ? e : p + window;
        if (const char* hit = searchSlice(p, we, needle)) return hit;
        if (we == e) break;
        p = we - (needle.size() - 1);
    }
    return nullptr;
}

// One slice of a mesh body, in document order. Triangle indices are global;
// the `tri` fields of paints and props are relative to the slice.
struct MeshChunk {
    Buf<float> positions;
    Buf<uint32_t> indices;
    std::vector<PaintRef> paints;
    std::vector<TriProp> props;
    uint32_t maxIndex = 0;  // validated against the vertex count after merging
};

bool parseVertex(const char* p, const char* gt, MeshChunk& out) {
    float xyz[3] = {0, 0, 0};
    bool ok = forEachAttr(p, gt, [&](std::string_view n, std::string_view v) {
        if (n.size() == 1 && n[0] >= 'x' && n[0] <= 'z') return toFloat(v, xyz[n[0] - 'x']);
        return true;
    });
    out.positions.insert(out.positions.end(), xyz, xyz + 3);
    return ok;
}

bool parseTriangle(const char* p, const char* gt, MeshChunk& out) {
    uint32_t v[3] = {0, 0, 0}, pid = UINT32_MAX, p1 = UINT32_MAX;
    std::string_view paint;
    bool ok = forEachAttr(p, gt, [&](std::string_view n, std::string_view val) {
        if (n.size() == 2 && n[0] == 'v' && n[1] >= '1' && n[1] <= '3') return toUint(val, v[n[1] - '1']);
        if (n == "paint_color" || n == "mmu_segmentation") paint = val;
        else if (n == "pid") toUint(val, pid);
        else if (n == "p1") toUint(val, p1);
        return true;
    });
    uint32_t tri = uint32_t(out.indices.size() / 3);
    out.indices.insert(out.indices.end(), v, v + 3);
    out.maxIndex = std::max({out.maxIndex, v[0], v[1], v[2]});
    if (!paint.empty()) out.paints.push_back({tri, paint});
    if (pid != UINT32_MAX && p1 != UINT32_MAX) out.props.push_back({tri, pid, p1});
    return ok;
}

// True if the tag name at p (just past '<') is exactly `name`.
bool isTag(const char* p, const char* e, std::string_view name) {
    return size_t(e - p) > name.size() && std::memcmp(p, name.data(), name.size()) == 0 && isSpace(p[name.size()]);
}

// Parses every <vertex> and <triangle> in [b, e), skipping all other tags.
// Both elements name themselves, so any slice of a mesh body parses on its
// own: no need to locate <vertices> and <triangles> first.
bool parseChunk(const char* b, const char* e, MeshChunk& out) {
    out.positions.reserve(size_t(e - b) / 40 * 3);
    out.indices.reserve(size_t(e - b) / 36 * 3);
    for (const char* p = b;;) {
        const char* lt = static_cast<const char*>(std::memchr(p, '<', size_t(e - p)));
        if (!lt) return true;
        p = lt + 1;
        const bool vertex = isTag(p, e, "vertex");
        if (!vertex && !isTag(p, e, "triangle")) continue;
        const char* gt = static_cast<const char*>(std::memchr(p, '>', size_t(e - p)));
        if (!gt) return false;
        if (!(vertex ? parseVertex(p + 6, gt, out) : parseTriangle(p + 8, gt, out))) return false;
        p = gt + 1;
    }
}

// Per-triangle colours of one slice, matching its triangles after resolution.
struct SliceColors {
    Buf<uint16_t> kept;        // one per triangle left in MeshChunk::indices
    Buf<float> leafPositions;  // paint subdivision leaves: three fresh vertices each
    Buf<uint16_t> leafStates;  // and their paint state
};

// Paint states of one slice. Triangles that stay whole are compacted in place
// in c.indices; subdivided ones leave, replaced by leaves. Returns whether any
// state is non-zero.
bool paintChunk(MeshChunk& c, const Buf<float>& positions, SliceColors& out) {
    const size_t nTris = c.indices.size() / 3;
    out.kept.resize(nTris);
    auto vertex = [&](uint32_t i) { return &positions[size_t(i) * 3]; };
    std::vector<PaintLeaf> leaves;
    bool painted = false;
    size_t kept = 0, k = 0;
    for (size_t t = 0; t < nTris; ++t) {
        uint32_t* tri = &c.indices[t * 3];
        uint16_t state = 0;
        if (k < c.paints.size() && c.paints[k].tri == t) {
            leaves.clear();
            decodePaint(c.paints[k++].code, vertex(tri[0]), vertex(tri[1]), vertex(tri[2]), leaves);
            if (leaves.size() == 1) {  // not subdivided: the triangle stays, coloured
                state = leaves[0].state;
            } else if (leaves.size() > 1) {  // replaced by its leaves
                for (const PaintLeaf& l : leaves) {
                    out.leafPositions.insert(out.leafPositions.end(), l.v, l.v + 9);
                    out.leafStates.push_back(l.state);
                    painted |= l.state != 0;
                }
                continue;
            }  // no leaves: undecodable code, the triangle stays unpainted
        }
        std::copy(tri, tri + 3, &c.indices[kept * 3]);
        out.kept[kept++] = state;
        painted |= state != 0;
    }
    c.indices.resize(kept * 3);
    out.kept.resize(kept);
    return painted;
}

// Material colours of one slice: 1 + index into the file's local colours,
// 0 where the material is unknown.
Buf<uint16_t> materialColors(const MeshChunk& c, const ModelFile& mf) {
    Buf<uint16_t> colors;
    colors.assign(c.indices.size() / 3, 0);
    for (const TriProp& p : c.props) {
        auto it = mf.propIndex.find(key2(p.pid, p.p1));
        if (it != mf.propIndex.end()) colors[p.tri] = uint16_t(std::min<uint32_t>(it->second + 1, 0xFFFF));
    }
    return colors;
}

// Joins the slices of one mesh. Triangles that stay whole keep their order;
// paint subdivision leaves go last: they own fresh, unshared vertices, so
// their indices are just consecutive numbers after the existing vertices.
bool mergeChunks(std::vector<MeshChunk>& chunks, ModelFile& mf, ParsedMesh& out) {
    const size_t n = chunks.size();
    Mesh& mesh = out.mesh;
    std::vector<Buf<float>> positions(n);
    uint32_t maxIndex = 0;
    bool anyTriangles = false, anyPaint = false, anyProps = false;
    for (size_t i = 0; i < n; ++i) {
        positions[i] = std::move(chunks[i].positions);
        maxIndex = std::max(maxIndex, chunks[i].maxIndex);
        anyTriangles |= !chunks[i].indices.empty();
        anyPaint |= !chunks[i].paints.empty();
        anyProps |= !chunks[i].props.empty();
    }
    appendParts(mesh.positions, std::move(positions));
    if (anyTriangles && maxIndex >= mesh.vertexCount()) return mf.error = "índice de vértice 3MF fuera de rango", false;

    // Per-triangle colours, slice by slice: paint states (which may subdivide
    // triangles) take precedence over material colours.
    std::vector<SliceColors> colors(n);
    std::atomic<bool> painted{false};
    if (anyPaint) {
        parallelFor(n, [&](size_t i) {
            if (paintChunk(chunks[i], mesh.positions, colors[i])) painted = true;
        });
    } else if (anyProps) {
        parallelFor(n, [&](size_t i) { colors[i].kept = materialColors(chunks[i], mf); });
        out.localColors = true;
    }

    std::vector<Buf<uint32_t>> indices(n);
    std::vector<Buf<float>> leafPositions(n);
    std::vector<Buf<uint16_t>> colorParts;  // kept colours of every slice, then leaf states
    size_t leafCount = 0;
    for (size_t i = 0; i < n; ++i) {
        indices[i] = std::move(chunks[i].indices);
        leafPositions[i] = std::move(colors[i].leafPositions);
        leafCount += colors[i].leafStates.size();
        colorParts.push_back(std::move(colors[i].kept));
    }
    for (SliceColors& c : colors) colorParts.push_back(std::move(c.leafStates));

    const uint32_t firstLeafVertex = uint32_t(mesh.vertexCount());
    appendParts(mesh.indices, std::move(indices), 3 * leafCount);
    std::iota(mesh.indices.end() - ptrdiff_t(3 * leafCount), mesh.indices.end(), firstLeafVertex);
    if (leafCount) appendParts(mesh.positions, std::move(leafPositions));
    if (painted || out.localColors) appendParts(mesh.triColor, std::move(colorParts));
    return true;
}

// Parses one <mesh> element whose content starts at b; e is the end of the
// document. Returns the position after </mesh>, or nullptr on error.
const char* parseMesh(const char* b, const char* e, ModelFile& mf, ParsedMesh& out) {
    constexpr std::string_view kClose = "</mesh>";
    const char* bodyEnd = findText(b, e, kClose);
    if (!bodyEnd) return mf.error = "malla 3MF sin cerrar", nullptr;

    const auto cuts = splitAtTags(b, bodyEnd, chunkCount(size_t(bodyEnd - b)));
    std::vector<MeshChunk> chunks(cuts.size() - 1);
    std::atomic<bool> ok{true};
    parallelFor(chunks.size(), [&](size_t i) {
        if (!parseChunk(cuts[i], cuts[i + 1], chunks[i])) ok = false;
    });
    if (!ok) return mf.error = "malla 3MF no válida", nullptr;
    if (!mergeChunks(chunks, mf, out)) return nullptr;
    return bodyEnd + kClose.size();
}

// ---------------------------------------------------------------- model file

float unitScale(std::string_view u) {
    if (u == "micron") return 0.001f;
    if (u == "centimeter") return 10.f;
    if (u == "inch") return 25.4f;
    if (u == "foot") return 304.8f;
    if (u == "meter") return 1000.f;
    return 1.f;  // millimeter (default)
}

bool parseModel(const char* b, const char* e, ModelFile& mf) {
    XmlScanner sc(b, e);
    XmlTag t;
    ObjectDef* cur = nullptr;
    int64_t group = -1;  // open <basematerials>/<colorgroup> id
    uint32_t groupIndex = 0;
    while (sc.next(t)) {
        std::string_view n = t.name;
        if (t.closing) {
            if (n == "object") cur = nullptr;
            else if (n == "basematerials" || n == "colorgroup") group = -1;
            continue;
        }
        if (n == "vertex" || n == "triangle") continue;
        if (n == "model") {
            mf.unitScale = unitScale(xmlAttr(t, "unit"));
        } else if (n == "object") {
            uint32_t id = 0;
            toUint(xmlAttr(t, "id"), id);
            cur = &mf.objects[id];
            if (toUint(xmlAttr(t, "pid"), cur->pid)) {
                cur->hasProp = true;
                toUint(xmlAttr(t, "pindex"), cur->pindex);
            }
        } else if (n == "mesh" && cur && !t.selfClosing) {
            ParsedMesh mesh;
            const char* after = parseMesh(sc.pos(), e, mf, mesh);
            if (!after) return false;
            cur->mesh = int(mf.meshes.size());
            mf.meshes.push_back(std::move(mesh));
            sc.seek(after);
        } else if (n == "component" && cur) {
            Component c;
            toUint(xmlAttr(t, "objectid"), c.objectId);
            c.path = std::string(xmlAttrLocal(t, "path"));
            c.transform = parseTransform(xmlAttr(t, "transform"));
            cur->components.push_back(std::move(c));
        } else if (n == "item") {
            BuildItem it;
            toUint(xmlAttr(t, "objectid"), it.objectId);
            it.path = std::string(xmlAttrLocal(t, "path"));
            it.transform = parseTransform(xmlAttr(t, "transform"));
            mf.build.push_back(std::move(it));
        } else if (n == "basematerials" || n == "colorgroup") {
            uint32_t id = 0;
            toUint(xmlAttr(t, "id"), id);
            group = t.selfClosing ? -1 : int64_t(id);
            groupIndex = 0;
        } else if ((n == "base" || n == "color") && group >= 0) {
            uint32_t c = kDefaultColor;
            parseColor(xmlAttr(t, n == "base" ? "displaycolor" : "color"), c);
            mf.propIndex[key2(uint32_t(group), groupIndex++)] = uint32_t(mf.colors.size());
            mf.colors.push_back(c);
        }
    }
    return true;
}

// ---------------------------------------------------------------- metadata

struct SlicerInfo {
    std::vector<uint32_t> filaments;
    std::unordered_map<uint32_t, uint32_t> objectExtruder;
    std::unordered_map<uint64_t, uint32_t> partExtruder;
    std::unordered_set<uint64_t> hiddenParts;  // modifiers, blockers, negative volumes
};

bool extractText(const ZipArchive& zip, std::string_view name, Buf<char>& out) {
    const ZipArchive::Entry* e = zip.find(name);
    std::string err;
    return e && zip.extract(*e, out, err);
}

// Bambu/Orca: JSON, "filament_colour": ["#RRGGBB", ...]
void readProjectSettings(const ZipArchive& zip, SlicerInfo& info) {
    Buf<char> buf;
    if (!extractText(zip, "Metadata/project_settings.config", buf)) return;
    std::string_view s(buf.data(), buf.size());
    size_t k = s.find("\"filament_colour\"");
    if (k == std::string_view::npos) return;
    size_t open = s.find('[', k), close = s.find(']', k);
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) return;
    std::string_view arr = s.substr(open + 1, close - open - 1);
    size_t p = 0;
    while ((p = arr.find('"', p)) != std::string_view::npos) {
        size_t q = arr.find('"', p + 1);
        if (q == std::string_view::npos) break;
        uint32_t c;
        info.filaments.push_back(parseColor(arr.substr(p + 1, q - p - 1), c) ? c : kDefaultColor);
        p = q + 1;
    }
}

// PrusaSlicer: INI-style lines "; extruder_colour = #FF8000;#..." / filament_colour.
void readPrusaConfig(const ZipArchive& zip, SlicerInfo& info) {
    Buf<char> buf;
    if (!extractText(zip, "Metadata/Slic3r_PE.config", buf)) return;
    std::string_view s(buf.data(), buf.size());
    auto values = [&](std::string_view key) {
        std::vector<uint32_t> out;
        size_t k = s.find(key);
        if (k == std::string_view::npos) return out;
        size_t eq = s.find('=', k), nl = s.find('\n', k);
        if (eq == std::string_view::npos || eq > nl) return out;
        std::string_view line = s.substr(eq + 1, nl - eq - 1);
        size_t p = 0;
        while (p <= line.size()) {
            size_t q = line.find(';', p);
            if (q == std::string_view::npos) q = line.size();
            uint32_t c;
            if (parseColor(line.substr(p, q - p), c)) out.push_back(c);
            else out.push_back(0);
            p = q + 1;
        }
        return out;
    };
    auto ext = values("; extruder_colour =");
    auto fil = values("; filament_colour =");
    for (size_t i = 0; i < std::max(ext.size(), fil.size()); ++i) {
        uint32_t c = i < ext.size() ? ext[i] : 0;
        if (!c && i < fil.size()) c = fil[i];
        info.filaments.push_back(c ? c : kDefaultColor);
    }
}

// Bambu/Orca: extruder per object and part, part subtypes.
void readModelSettings(const ZipArchive& zip, SlicerInfo& info) {
    Buf<char> buf;
    if (!extractText(zip, "Metadata/model_settings.config", buf)) return;
    XmlScanner sc(buf.data(), buf.data() + buf.size());
    XmlTag t;
    int64_t obj = -1, part = -1;
    while (sc.next(t)) {
        if (t.closing) {
            if (t.name == "part") part = -1;
            else if (t.name == "object") obj = -1;
            continue;
        }
        if (t.name == "object") {
            uint32_t id;
            obj = toUint(xmlAttr(t, "id"), id) ? int64_t(id) : -1;
        } else if (t.name == "part" && obj >= 0) {
            uint32_t id;
            part = toUint(xmlAttr(t, "id"), id) ? int64_t(id) : -1;
            std::string_view sub = xmlAttr(t, "subtype");
            if (part >= 0 && !sub.empty() && sub != "normal_part")
                info.hiddenParts.insert(key2(uint32_t(obj), uint32_t(part)));
            if (t.selfClosing) part = -1;
        } else if (t.name == "metadata" && obj >= 0 && xmlAttr(t, "key") == "extruder") {
            uint32_t ext;
            if (!toUint(xmlAttr(t, "value"), ext) || ext == 0) continue;
            if (part >= 0) info.partExtruder[key2(uint32_t(obj), uint32_t(part))] = ext;
            else info.objectExtruder[uint32_t(obj)] = ext;
        }
    }
}

void readSlicerInfo(const ZipArchive& zip, SlicerInfo& info) {
    readProjectSettings(zip, info);
    if (info.filaments.empty()) readPrusaConfig(zip, info);
    readModelSettings(zip, info);
}

std::string findRootModel(const ZipArchive& zip) {
    Buf<char> buf;
    if (extractText(zip, "_rels/.rels", buf)) {
        XmlScanner sc(buf.data(), buf.data() + buf.size());
        XmlTag t;
        while (sc.next(t)) {
            if (t.closing || t.name != "Relationship") continue;
            std::string_view type = xmlAttr(t, "Type");
            if (type.size() >= 7 && type.substr(type.size() - 7) == "3dmodel") return normPath(xmlAttr(t, "Target"));
        }
    }
    return "3d/3dmodel.model";
}

// ---------------------------------------------------------------- assembly

struct Assembler {
    Scene& scene;
    std::unordered_map<std::string, ModelFile>& files;
    std::unordered_map<std::string, uint32_t> meshBase, colorBase;
    const SlicerInfo& info;
    uint16_t filamentEnd = 1;  // palette[1 .. filamentEnd) are filament colours

    void expand(const std::string& fileKey, uint32_t objectId, const Mat4& t, uint32_t root, int64_t part,
                int64_t inheritedColor, int depth) {
        if (depth > 16) return;  // malformed cyclic references
        auto f = files.find(fileKey);
        if (f == files.end()) return;
        auto o = f->second.objects.find(objectId);
        if (o == f->second.objects.end()) return;
        const ObjectDef& obj = o->second;

        int64_t color = inheritedColor;
        if (obj.hasProp) {
            auto pi = f->second.propIndex.find(key2(obj.pid, obj.pindex));
            if (pi != f->second.propIndex.end()) color = colorBase[fileKey] + pi->second;
        }
        if (obj.mesh >= 0) {
            uint16_t c = color >= 0 ? uint16_t(color) : 0;
            if (!info.filaments.empty()) {
                uint32_t ext = 0;
                if (part >= 0) {
                    auto it = info.partExtruder.find(key2(root, uint32_t(part)));
                    if (it != info.partExtruder.end()) ext = it->second;
                }
                if (!ext) {
                    auto it = info.objectExtruder.find(root);
                    ext = it != info.objectExtruder.end() ? it->second : 1;
                }
                c = ext < filamentEnd ? uint16_t(ext) : 0;
            }
            scene.instances.push_back({meshBase[fileKey] + uint32_t(obj.mesh), c, t});
        }
        for (const Component& comp : obj.components) {
            int64_t childPart = depth == 0 ? int64_t(comp.objectId) : part;
            if (depth == 0 && info.hiddenParts.count(key2(root, comp.objectId))) continue;
            std::string key = comp.path.empty() ? fileKey : normPath(comp.path);
            expand(key, comp.objectId, Mat4::mul(t, comp.transform), root, childPart, color, depth + 1);
        }
    }
};

// Distinct colours for paint states that have no filament colour.
constexpr uint32_t kFallbackColors[] = {0xFF42AE00, 0xFF3C3CE6, 0xFFE6A03C, 0xFF3CC8E6, 0xFFB43CE6,
                                        0xFF3CE6A0, 0xFFE63C8C, 0xFF8C8C8C};

}  // namespace

void load3mf(const uint8_t* data, size_t size, Scene& scene) {
    ZipArchive zip;
    std::string err;
    if (!zip.open(data, size, err)) {
        scene.error = "3MF no válido: " + err;
        return;
    }

    std::string rootKey = findRootModel(zip);
    const ZipArchive::Entry* rootEntry = zip.find(rootKey);
    if (!rootEntry) {
        for (const auto& e : zip.entries())
            if (e.name.size() > 6 && normPath(e.name).ends_with(".model")) {
                rootEntry = &e, rootKey = normPath(e.name);
                break;
            }
    }
    if (!rootEntry) {
        scene.error = "3MF sin modelo 3D (¿archivo .gcode.3mf ya laminado?)";
        return;
    }

    std::unordered_map<std::string, ModelFile> files;
    std::vector<Buf<char>> buffers;  // keep XML alive: paint codes are views into it
    buffers.reserve(64);
    buffers.emplace_back();
    if (!zip.extract(*rootEntry, buffers.back(), err)) {
        scene.error = "3MF: " + err;
        return;
    }
    ModelFile& root = files[rootKey];
    if (!parseModel(buffers.back().data(), buffers.back().data() + buffers.back().size(), root)) {
        scene.error = "3MF: " + root.error;
        return;
    }

    // Production extension: object files referenced from the root, parsed in parallel.
    std::vector<std::string> keys;
    for (auto& [id, obj] : root.objects)
        for (auto& c : obj.components)
            if (!c.path.empty()) keys.push_back(normPath(c.path));
    for (auto& it : root.build)
        if (!it.path.empty()) keys.push_back(normPath(it.path));
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::erase(keys, rootKey);

    std::vector<ModelFile*> targets;
    for (const std::string& k : keys) targets.push_back(&files[k]);  // no rehash once threads run
    std::vector<Buf<char>> subBuffers(keys.size());
    auto parseObjectFile = [&](size_t i) {
        const ZipArchive::Entry* e = zip.find(keys[i]);
        if (!e) {
            targets[i]->error = "falta " + keys[i];
            return;
        }
        std::string werr;
        if (!zip.extract(*e, subBuffers[i], werr)) {
            targets[i]->error = werr;
            return;
        }
        parseModel(subBuffers[i].data(), subBuffers[i].data() + subBuffers[i].size(), *targets[i]);
    };
    SlicerInfo info;
    parallelInvoke({[&] { readSlicerInfo(zip, info); }, [&] { parallelFor(keys.size(), parseObjectFile); }});
    for (size_t i = 0; i < keys.size(); ++i)
        if (!targets[i]->error.empty()) {
            scene.error = "3MF: " + targets[i]->error;
            return;
        }

    // Palette: [0] default, [1..N] filaments, then per-file material colours.
    scene.palette.insert(scene.palette.end(), info.filaments.begin(), info.filaments.end());

    Assembler as{scene, files, {}, {}, info, uint16_t(scene.palette.size())};
    for (auto& [key, mf] : files) {
        as.meshBase[key] = uint32_t(scene.meshes.size());
        uint32_t cbase = uint32_t(scene.palette.size());
        as.colorBase[key] = cbase;
        scene.palette.insert(scene.palette.end(), mf.colors.begin(), mf.colors.end());
        for (ParsedMesh& pm : mf.meshes) {
            if (pm.localColors)
                for (uint16_t& c : pm.mesh.triColor)
                    if (c) c = uint16_t(std::min<uint32_t>(cbase + c - 1, 0xFFFF));
            scene.meshes.push_back(std::move(pm.mesh));
        }
    }

    Mat4 unit;
    unit.m[0] = unit.m[5] = unit.m[10] = root.unitScale;
    for (const BuildItem& it : root.build) {
        std::string key = it.path.empty() ? rootKey : normPath(it.path);
        as.expand(key, it.objectId, Mat4::mul(unit, it.transform), it.objectId, -1, -1, 0);
    }

    // Paint states beyond the known filaments still need a visible colour.
    uint16_t maxState = 0;
    for (const Mesh& m : scene.meshes)
        for (uint16_t c : m.triColor) maxState = std::max(maxState, c);
    while (scene.palette.size() <= maxState)
        scene.addColor(kFallbackColors[scene.palette.size() % std::size(kFallbackColors)]);
}

}  // namespace v3d
