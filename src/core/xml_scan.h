#pragma once

// Forward-only XML tag scanner for machine-written files (3MF models, Bambu
// configs). No DOM, no entity decoding, no validation: it walks tags and
// hands out attribute values as views into the source buffer.

#include <cstring>
#include <string_view>

#include "parse.h"

namespace v3d {

struct XmlTag {
    std::string_view name;   // local name, namespace prefix stripped
    const char* attrs;       // first byte after the name
    const char* end;         // '>' of the tag
    bool closing = false;    // </name>
    bool selfClosing = false;  // <name ... />
};

class XmlScanner {
public:
    XmlScanner(const char* begin, const char* end) : p_(begin), end_(end) {}

    // Advances to the next element tag; skips comments, PIs and CDATA.
    bool next(XmlTag& t) {
        while (true) {
            const char* lt = static_cast<const char*>(std::memchr(p_, '<', size_t(end_ - p_)));
            if (!lt || lt + 1 >= end_) return false;
            const char* q = lt + 1;
            if (*q == '?' || *q == '!') {
                const char* close = skipSpecial(q);
                if (!close) return false;
                p_ = close;
                continue;
            }
            t.closing = *q == '/';
            if (t.closing) ++q;
            const char* nameBegin = q;
            while (q < end_ && !isSpace(*q) && *q != '>' && *q != '/') ++q;
            std::string_view name(nameBegin, size_t(q - nameBegin));
            if (size_t colon = name.find(':'); colon != std::string_view::npos) name.remove_prefix(colon + 1);
            t.name = name;
            t.attrs = q;
            // Attribute values never contain a raw '>' in the files we read.
            const char* gt = static_cast<const char*>(std::memchr(q, '>', size_t(end_ - q)));
            if (!gt) return false;
            t.end = gt;
            t.selfClosing = gt > q && gt[-1] == '/';
            p_ = gt + 1;
            return true;
        }
    }

    const char* pos() const { return p_; }
    void seek(const char* p) { p_ = p; }

private:
    const char* skipSpecial(const char* q) {
        auto after = [&](const char* pat, size_t n) -> const char* {
            const char* f = static_cast<const char*>(memmem(q, size_t(end_ - q), pat, n));
            return f ? f + n : nullptr;
        };
        if (end_ - q >= 3 && q[0] == '!' && q[1] == '-' && q[2] == '-') return after("-->", 3);
        if (end_ - q >= 8 && std::memcmp(q, "![CDATA[", 8) == 0) return after("]]>", 3);
        return after(">", 1);
    }

    const char* p_;
    const char* end_;
};

// Iterates name="value" pairs between t.attrs and t.end.
class XmlAttrs {
public:
    explicit XmlAttrs(const XmlTag& t) : p_(t.attrs), end_(t.end) {}

    bool next(std::string_view& name, std::string_view& value) {
        p_ = skipSpace(p_, end_);
        const char* nb = p_;
        while (p_ < end_ && *p_ != '=' && !isSpace(*p_) && *p_ != '/') ++p_;
        if (p_ >= end_ || p_ == nb) return false;
        name = std::string_view(nb, size_t(p_ - nb));
        p_ = skipSpace(p_, end_);
        if (p_ >= end_ || *p_ != '=') return false;
        p_ = skipSpace(p_ + 1, end_);
        if (p_ >= end_ || (*p_ != '"' && *p_ != '\'')) return false;
        char quote = *p_++;
        const char* vb = p_;
        const char* ve = static_cast<const char*>(std::memchr(p_, quote, size_t(end_ - p_)));
        if (!ve) return false;
        value = std::string_view(vb, size_t(ve - vb));
        p_ = ve + 1;
        return true;
    }

private:
    const char* p_;
    const char* end_;
};

// Value of one attribute (exact name, including any prefix), or empty.
inline std::string_view xmlAttr(const XmlTag& t, std::string_view wanted) {
    XmlAttrs it(t);
    std::string_view n, v;
    while (it.next(n, v))
        if (n == wanted) return v;
    return {};
}

// Attribute lookup that ignores the namespace prefix ("p:path" matches "path").
inline std::string_view xmlAttrLocal(const XmlTag& t, std::string_view wanted) {
    XmlAttrs it(t);
    std::string_view n, v;
    while (it.next(n, v)) {
        if (size_t colon = n.find(':'); colon != std::string_view::npos) n.remove_prefix(colon + 1);
        if (n == wanted) return v;
    }
    return {};
}

inline bool toUint(std::string_view s, uint32_t& out) {
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && s.size() > 0;
}

inline bool toFloat(std::string_view s, float& out) {
    const char* b = s.data();
    const char* e = b + s.size();
    b = skipSpace(b, e);
    if (b < e && *b == '+') ++b;
    auto r = std::from_chars(b, e, out);
    return r.ec == std::errc();
}

}  // namespace v3d
