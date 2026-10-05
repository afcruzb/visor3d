#include "bambu_paint.h"

#include <cstring>

namespace v3d {

namespace {

struct V3 {
    float x, y, z;
};

V3 mid(const V3& a, const V3& b) { return {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f}; }

// Every field in the stream is a whole nibble, so reading hex digits from the
// end of the string yields the original bitstream order directly.
struct NibbleReader {
    std::string_view s;
    size_t k = 0;
    bool ok = true;

    int next() {
        if (k >= s.size()) {
            ok = false;
            return 0;
        }
        char ch = s[s.size() - 1 - k++];
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        ok = false;
        return 0;
    }
};

constexpr int kMaxDepth = 32;

void decodeNode(NibbleReader& r, const V3& a, const V3& b, const V3& c, int depth, std::vector<PaintLeaf>& out) {
    if (!r.ok) return;
    if (depth > kMaxDepth) {
        r.ok = false;
        return;
    }
    int code = r.next();
    int splits = code & 0b11;
    if (splits == 0) {
        int state = code >> 2;
        if (state == 3) {  // extended: next nibble holds state - 3, 0b1111 escapes to state - 18
            int n = r.next();
            state = n == 0b1111 ? r.next() + 18 : n + 3;
        }
        if (!r.ok) return;
        PaintLeaf leaf;
        std::memcpy(leaf.v + 0, &a, 12);
        std::memcpy(leaf.v + 3, &b, 12);
        std::memcpy(leaf.v + 6, &c, 12);
        leaf.state = uint8_t(state);
        out.push_back(leaf);
        return;
    }

    int special = splits == 3 ? 0 : code >> 2;
    if (special > 2) {
        r.ok = false;
        return;
    }
    // Rotate so the special side's vertex comes first, then split exactly as
    // the slicers do (TriangleSelector::perform_split).
    const V3* v[3] = {&a, &b, &c};
    const V3& A = *v[special];
    const V3& B = *v[(special + 1) % 3];
    const V3& C = *v[(special + 2) % 3];

    V3 child[4][3];
    int n = splits + 1;
    if (splits == 1) {
        V3 m = mid(C, B);
        child[0][0] = A, child[0][1] = B, child[0][2] = m;
        child[1][0] = m, child[1][1] = C, child[1][2] = A;
    } else if (splits == 2) {
        V3 mab = mid(B, A), mac = mid(A, C);
        child[0][0] = A, child[0][1] = mab, child[0][2] = mac;
        child[1][0] = mab, child[1][1] = B, child[1][2] = mac;
        child[2][0] = B, child[2][1] = C, child[2][2] = mac;
    } else {
        V3 mab = mid(B, A), mbc = mid(C, B), mca = mid(A, C);
        child[0][0] = A, child[0][1] = mab, child[0][2] = mca;
        child[1][0] = mab, child[1][1] = B, child[1][2] = mbc;
        child[2][0] = mbc, child[2][1] = C, child[2][2] = mca;
        child[3][0] = mab, child[3][1] = mbc, child[3][2] = mca;
    }
    // Children are stored last to first.
    for (int i = n - 1; i >= 0 && r.ok; --i) decodeNode(r, child[i][0], child[i][1], child[i][2], depth + 1, out);
}

}  // namespace

bool decodePaint(std::string_view hex, const float a[3], const float b[3], const float c[3],
                 std::vector<PaintLeaf>& out) {
    size_t before = out.size();
    NibbleReader r{hex};
    V3 va{a[0], a[1], a[2]}, vb{b[0], b[1], b[2]}, vc{c[0], c[1], c[2]};
    decodeNode(r, va, vb, vc, 0, out);
    if (!r.ok) {
        out.resize(before);
        return false;
    }
    return true;
}

}  // namespace v3d
