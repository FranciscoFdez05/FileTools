#include "svgtext.h"

#include <dwrite.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <xmllite.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <map>
#include <unordered_map>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kSvgNs[] = L"http://www.w3.org/2000/svg";
constexpr wchar_t kXmlNs[] = L"http://www.w3.org/XML/1998/namespace";
constexpr float kDefaultFontSize = 16;  // CSS "medium"
constexpr float kPi = 3.14159265f;
// Browsers default to a serif face, but labels in diagrams are nearly always meant to be sans.
constexpr wchar_t kFallbackFont[] = L"Arial";

// Subtrees that are never painted where they stand (or that hold no drawable text).
constexpr const wchar_t* kSkippedElements[] = {
    L"defs",   L"symbol", L"clipPath", L"mask",     L"pattern", L"marker",         L"metadata",       L"title",
    L"desc",   L"filter", L"script",   L"textPath", L"use",     L"linearGradient", L"radialGradient", L"foreignObject",
    L"style",
};

// Generic families and the fonts Linux tools (Inkscape, LibreOffice) write by default, mapped to
// metric-compatible Windows fonts so centred labels keep their width.
constexpr std::pair<const wchar_t*, const wchar_t*> kFontAliases[] = {
    {L"sans-serif", L"Arial"},          {L"serif", L"Times New Roman"},   {L"monospace", L"Consolas"},
    {L"system-ui", L"Segoe UI"},        {L"cursive", L"Comic Sans MS"},   {L"fantasy", L"Impact"},
    {L"liberation sans", L"Arial"},     {L"liberation serif", L"Times New Roman"},
    {L"liberation mono", L"Courier New"}, {L"helvetica", L"Arial"},     {L"dejavu sans", L"Verdana"},
    {L"dejavu serif", L"Georgia"},      {L"dejavu sans mono", L"Consolas"}, {L"bitstream vera sans", L"Verdana"},
};

constexpr std::pair<const wchar_t*, uint32_t> kNamedColors[] = {
    {L"black", 0x000000},     {L"white", 0xFFFFFF},    {L"red", 0xFF0000},       {L"green", 0x008000},
    {L"blue", 0x0000FF},      {L"yellow", 0xFFFF00},   {L"gray", 0x808080},      {L"grey", 0x808080},
    {L"silver", 0xC0C0C0},    {L"maroon", 0x800000},   {L"navy", 0x000080},      {L"olive", 0x808000},
    {L"purple", 0x800080},    {L"teal", 0x008080},     {L"aqua", 0x00FFFF},      {L"cyan", 0x00FFFF},
    {L"fuchsia", 0xFF00FF},   {L"magenta", 0xFF00FF},  {L"lime", 0x00FF00},      {L"orange", 0xFFA500},
    {L"brown", 0xA52A2A},     {L"pink", 0xFFC0CB},     {L"darkgray", 0xA9A9A9},  {L"darkgrey", 0xA9A9A9},
    {L"lightgray", 0xD3D3D3}, {L"lightgrey", 0xD3D3D3}, {L"darkblue", 0x00008B}, {L"darkred", 0x8B0000},
    {L"darkgreen", 0x006400}, {L"dimgray", 0x696969},  {L"dimgrey", 0x696969},  {L"gold", 0xFFD700},
};

enum class Anchor { Start, Middle, End };

// The properties that matter for text, as inherited down the tree.
struct TextStyle {
    D2D1_COLOR_F fill = D2D1::ColorF(0, 0, 0);
    bool fillNone = false;
    float fillOpacity = 1;
    float opacity = 1;  // not inherited in CSS, but group opacity multiplies down to the glyphs
    std::wstring fontFamily;
    float fontSize = kDefaultFontSize;
    int weight = DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE fontStyle = DWRITE_FONT_STYLE_NORMAL;
    Anchor anchor = Anchor::Start;
    bool visible = true;
    bool preserveSpace = false;
};

// One element's own values on top of the inherited style: these two don't inherit.
struct ElementStyle {
    TextStyle style;
    float ownOpacity = 1;
    bool displayNone = false;
};

using Declarations = std::vector<std::pair<std::wstring, std::wstring>>;

IDWriteFactory* DWrite() {
    thread_local ComPtr<IDWriteFactory> factory;
    if (!factory)
        CheckHr(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                    reinterpret_cast<IUnknown**>(factory.GetAddressOf())),
                L"No se pudo iniciar DirectWrite");
    return factory.Get();
}

bool IsSpace(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; }

std::wstring_view TrimView(std::wstring_view s) {
    while (!s.empty() && IsSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && IsSpace(s.back())) s.remove_suffix(1);
    return s;
}

// Reads one number and moves `p` past it and any separator that follows.
bool NextNumber(const wchar_t*& p, float& out) {
    while (IsSpace(*p) || *p == L',') ++p;
    wchar_t* end = nullptr;
    double v = wcstod(p, &end);
    if (end == p) return false;
    out = float(v);
    p = end;
    return true;
}

// x="10 20 30" positions each glyph on its own; only the first value is honoured.
bool FirstNumber(const std::wstring& s, float& out) {
    const wchar_t* p = s.c_str();
    return NextNumber(p, out);
}

D2D1::Matrix3x2F ParseTransform(const std::wstring& s) {
    D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Identity();
    const wchar_t* p = s.c_str();
    for (;;) {
        while (IsSpace(*p) || *p == L',') ++p;
        const wchar_t* nameStart = p;
        while (iswalpha(*p)) ++p;
        std::wstring_view name(nameStart, p - nameStart);
        while (IsSpace(*p)) ++p;
        if (name.empty() || *p != L'(') break;
        ++p;
        float a[6] = {};
        int count = 0;
        while (count < 6 && NextNumber(p, a[count])) ++count;
        while (*p && *p != L')') ++p;
        if (*p) ++p;

        D2D1::Matrix3x2F t = D2D1::Matrix3x2F::Identity();
        if (name == L"matrix" && count == 6) t = D2D1::Matrix3x2F(a[0], a[1], a[2], a[3], a[4], a[5]);
        else if (name == L"translate" && count >= 1) t = D2D1::Matrix3x2F::Translation(a[0], count > 1 ? a[1] : 0);
        else if (name == L"scale" && count >= 1) t = D2D1::Matrix3x2F::Scale(a[0], count > 1 ? a[1] : a[0]);
        else if (name == L"rotate" && count >= 1)
            t = D2D1::Matrix3x2F::Rotation(a[0], count >= 3 ? D2D1::Point2F(a[1], a[2]) : D2D1::Point2F());
        else if (name == L"skewX" && count >= 1) t = D2D1::Matrix3x2F(1, 0, std::tan(a[0] * kPi / 180), 1, 0, 0);
        else if (name == L"skewY" && count >= 1) t = D2D1::Matrix3x2F(1, std::tan(a[0] * kPi / 180), 0, 1, 0, 0);
        // In "A B" B reaches the points first; with Direct2D's row vectors that is B * A.
        m = t * m;
    }
    return m;
}

D2D1_COLOR_F Rgb(uint32_t rgb) { return D2D1::ColorF(rgb); }

bool ParseHexColor(std::wstring_view hex, D2D1_COLOR_F& out) {
    if (hex.size() != 3 && hex.size() != 6) return false;
    uint32_t v = 0;
    for (wchar_t c : hex) {
        if (!iswxdigit(c)) return false;
        v = v * 16 + (iswdigit(c) ? c - L'0' : (towlower(c) - L'a' + 10));
    }
    if (hex.size() == 3)
        v = ((v >> 8 & 0xF) * 0x110000) | ((v >> 4 & 0xF) * 0x1100) | ((v & 0xF) * 0x11);
    out = Rgb(v);
    return true;
}

// rgb(255, 0, 0), rgb(100%, 0%, 0%) and rgba(...) with a trailing alpha.
bool ParseRgbFunction(std::wstring_view args, D2D1_COLOR_F& out) {
    std::wstring s(args);
    const wchar_t* p = s.c_str();
    float c[4] = {0, 0, 0, 1};
    for (int i = 0; i < 4; ++i) {
        if (!NextNumber(p, c[i])) {
            if (i < 3) return false;
            break;
        }
        if (*p == L'%') {
            c[i] = i < 3 ? c[i] * 2.55f : c[i] / 100;
            ++p;
        }
    }
    out = D2D1::ColorF(std::clamp(c[0], 0.f, 255.f) / 255, std::clamp(c[1], 0.f, 255.f) / 255,
                       std::clamp(c[2], 0.f, 255.f) / 255, std::clamp(c[3], 0.f, 1.f));
    return true;
}

bool ParseColor(std::wstring_view value, D2D1_COLOR_F& out) {
    value = TrimView(value);
    if (value.empty()) return false;
    if (value.front() == L'#') return ParseHexColor(value.substr(1), out);
    std::wstring lower = ToLower(std::wstring(value));
    if (lower.starts_with(L"rgb")) {
        size_t open = lower.find(L'('), close = lower.find(L')');
        return open != std::wstring::npos && close > open && ParseRgbFunction(value.substr(open + 1, close - open - 1), out);
    }
    for (auto& [name, rgb] : kNamedColors) {
        if (lower == name) {
            out = Rgb(rgb);
            return true;
        }
    }
    return false;
}

void ApplyFill(TextStyle& s, std::wstring_view value) {
    value = TrimView(value);
    if (value == L"none") {
        s.fillNone = true;
        return;
    }
    // Gradients and patterns aren't available here: use their fallback colour if one is given,
    // otherwise keep the inherited fill so the text still shows.
    if (value.starts_with(L"url(")) {
        size_t close = value.find(L')');
        if (close == std::wstring_view::npos) return;
        value = value.substr(close + 1);
    }
    D2D1_COLOR_F color;
    if (ParseColor(value, color)) {
        s.fill = color;
        s.fillNone = false;
    }
}

bool ParseFontSize(std::wstring_view value, float parent, float& out) {
    std::wstring s(TrimView(value));
    static const std::pair<const wchar_t*, float> keywords[] = {
        {L"xx-small", 9.f}, {L"x-small", 10.f}, {L"small", 13.f},   {L"medium", 16.f},
        {L"large", 18.f},   {L"x-large", 24.f}, {L"xx-large", 32.f}, {L"xxx-large", 48.f},
    };
    for (auto& [name, size] : keywords) {
        if (s == name) {
            out = size;
            return true;
        }
    }
    if (s == L"larger" || s == L"smaller") {
        out = s == L"larger" ? parent * 1.2f : parent / 1.2f;
        return true;
    }

    wchar_t* end = nullptr;
    double n = wcstod(s.c_str(), &end);
    if (end == s.c_str() || n <= 0) return false;
    std::wstring unit = ToLower(std::wstring(TrimView(end)));
    if (unit.empty() || unit == L"px") out = float(n);
    else if (unit == L"pt") out = float(n * 4 / 3);
    else if (unit == L"pc") out = float(n * 16);
    else if (unit == L"in") out = float(n * 96);
    else if (unit == L"cm") out = float(n * 96 / 2.54);
    else if (unit == L"mm") out = float(n * 96 / 25.4);
    else if (unit == L"em" || unit == L"rem") out = float(n * parent);
    else if (unit == L"ex") out = float(n * parent / 2);
    else if (unit == L"%") out = float(n * parent / 100);
    else return false;
    return true;
}

int ParseFontWeight(std::wstring_view value, int parent) {
    if (value == L"normal") return 400;
    if (value == L"bold") return 700;
    if (value == L"bolder") return parent < 350 ? 400 : parent < 550 ? 700 : 900;
    if (value == L"lighter") return parent < 550 ? 100 : parent < 750 ? 400 : 700;
    int n = _wtoi(std::wstring(value).c_str());
    return n >= 1 && n <= 1000 ? n : parent;
}

float ParseOpacity(std::wstring_view value, float current) {
    std::wstring s(TrimView(value));
    wchar_t* end = nullptr;
    double n = wcstod(s.c_str(), &end);
    if (end == s.c_str()) return current;
    if (*end == L'%') n /= 100;
    return std::clamp(float(n), 0.f, 1.f);
}

void ApplyProperty(ElementStyle& e, const TextStyle& parent, const std::wstring& name, std::wstring_view value) {
    value = TrimView(value);
    if (value.empty() || value == L"inherit") return;
    TextStyle& s = e.style;
    if (name == L"display") e.displayNone = value == L"none";
    else if (name == L"visibility") s.visible = value == L"visible";
    else if (name == L"opacity") e.ownOpacity = ParseOpacity(value, e.ownOpacity);
    else if (name == L"fill") ApplyFill(s, value);
    else if (name == L"fill-opacity") s.fillOpacity = ParseOpacity(value, s.fillOpacity);
    else if (name == L"font-family") s.fontFamily = value;
    else if (name == L"font-size") ParseFontSize(value, parent.fontSize, s.fontSize);
    else if (name == L"font-weight") s.weight = ParseFontWeight(value, parent.weight);
    else if (name == L"font-style")
        s.fontStyle = value == L"italic"    ? DWRITE_FONT_STYLE_ITALIC
                      : value == L"oblique" ? DWRITE_FONT_STYLE_OBLIQUE
                                            : DWRITE_FONT_STYLE_NORMAL;
    else if (name == L"text-anchor")
        s.anchor = value == L"middle" ? Anchor::Middle : value == L"end" ? Anchor::End : Anchor::Start;
}

// "fill:#000; font-size:12px" -> {{"fill", "#000"}, {"font-size", "12px"}}
Declarations ParseDeclarations(std::wstring_view css) {
    Declarations out;
    while (!css.empty()) {
        size_t semi = css.find(L';');
        std::wstring_view item = css.substr(0, semi);
        size_t colon = item.find(L':');
        if (colon != std::wstring_view::npos) {
            std::wstring_view name = TrimView(item.substr(0, colon));
            std::wstring_view value = TrimView(item.substr(colon + 1));
            if (value.ends_with(L"!important")) value = TrimView(value.substr(0, value.size() - 10));
            if (!name.empty()) out.emplace_back(ToLower(std::wstring(name)), std::wstring(value));
        }
        if (semi == std::wstring_view::npos) break;
        css.remove_prefix(semi + 1);
    }
    return out;
}

// Only tag, .class and #id selectors are kept: they cover what editors export, and anything
// fancier would need a real CSS engine.
bool IsSimpleSelector(std::wstring_view sel) {
    if (sel.empty()) return false;
    size_t start = sel.front() == L'.' || sel.front() == L'#' ? 1 : 0;
    if (start == sel.size()) return false;
    return std::all_of(sel.begin() + start, sel.end(), [](wchar_t c) { return iswalnum(c) || c == L'-' || c == L'_'; });
}

void ParseStyleSheet(std::wstring css, std::unordered_map<std::wstring, Declarations>& rules) {
    for (size_t c; (c = css.find(L"/*")) != std::wstring::npos;) {
        size_t end = css.find(L"*/", c + 2);
        css.erase(c, end == std::wstring::npos ? std::wstring::npos : end + 2 - c);
    }
    std::wstring_view rest(css);
    while (!rest.empty()) {
        size_t open = rest.find(L'{');
        if (open == std::wstring_view::npos) break;
        std::wstring_view selectors = TrimView(rest.substr(0, open));
        // Find the matching brace so at-rules with nested blocks (@media) are skipped whole.
        size_t close = open;
        for (int depth = 0; close < rest.size(); ++close) {
            if (rest[close] == L'{') ++depth;
            else if (rest[close] == L'}' && --depth == 0) break;
        }
        std::wstring_view body = rest.substr(open + 1, std::min(close, rest.size()) - open - 1);
        if (!selectors.starts_with(L"@")) {
            Declarations decls = ParseDeclarations(body);
            while (!selectors.empty()) {
                size_t comma = selectors.find(L',');
                std::wstring_view sel = TrimView(selectors.substr(0, comma));
                if (IsSimpleSelector(sel)) {
                    auto& target = rules[std::wstring(sel)];
                    target.insert(target.end(), decls.begin(), decls.end());
                }
                if (comma == std::wstring_view::npos) break;
                selectors.remove_prefix(comma + 1);
            }
        }
        if (close >= rest.size()) break;
        rest.remove_prefix(close + 1);
    }
}

// Picks the first family in the list that is installed (directly or through an alias).
class FontResolver {
public:
    const std::wstring& Resolve(const std::wstring& familyList) {
        auto it = cache_.find(familyList);
        if (it != cache_.end()) return it->second;
        return cache_[familyList] = Lookup(familyList);
    }

private:
    bool Installed(const std::wstring& name) {
        if (!fonts_) CheckHr(DWrite()->GetSystemFontCollection(&fonts_), L"DirectWrite");
        UINT32 index;
        BOOL exists = FALSE;
        return SUCCEEDED(fonts_->FindFamilyName(name.c_str(), &index, &exists)) && exists;
    }

    std::wstring Lookup(std::wstring_view list) {
        while (!list.empty()) {
            size_t comma = list.find(L',');
            std::wstring_view name = TrimView(list.substr(0, comma));
            if (name.size() >= 2 && (name.front() == L'\'' || name.front() == L'"') && name.back() == name.front())
                name = TrimView(name.substr(1, name.size() - 2));
            std::wstring family(name);
            if (!family.empty() && Installed(family)) return family;
            std::wstring lower = ToLower(family);
            for (auto& [alias, windowsFont] : kFontAliases)
                if (lower == alias && Installed(windowsFont)) return windowsFont;
            if (comma == std::wstring_view::npos) break;
            list.remove_prefix(comma + 1);
        }
        return kFallbackFont;
    }

    ComPtr<IDWriteFontCollection> fonts_;
    std::map<std::wstring, std::wstring> cache_;
};

struct Frame {
    TextStyle style;
    D2D1::Matrix3x2F transform = D2D1::Matrix3x2F::Identity();  // element user space -> root user space
    bool skip = false;
    bool styleSheet = false;
    bool textRoot = false;
};

struct Run {
    ComPtr<IDWriteTextLayout> layout;
    D2D1_POINT_2F baselineOrigin;
    float baseline;
    D2D1_COLOR_F color;
    D2D1::Matrix3x2F transform;
};

class TextPainter {
public:
    TextPainter(ID2D1RenderTarget* target, const D2D1_MATRIX_3X2_F& toPixels)
        : target_(target),
          toPixels_(toPixels._11, toPixels._12, toPixels._21, toPixels._22, toPixels._31, toPixels._32) {}

    void Paint(const uint8_t* data, size_t size) {
        ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(data, (UINT)size));
        if (!stream) Fail(L"Sin memoria para leer el SVG");
        ComPtr<IXmlReader> reader;
        CheckHr(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr),
                L"XmlLite");
        // Old SVGs carry a DOCTYPE; parsing it is safe because no resolver is set (external DTDs
        // aren't fetched) and XmlLite caps entity expansion.
        CheckHr(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Parse), L"XmlLite");
        CheckHr(reader->SetInput(stream.Get()), L"XmlLite");

        XmlNodeType type;
        HRESULT hr;
        while ((hr = reader->Read(&type)) == S_OK) {
            if (type == XmlNodeType_Element) {
                bool empty = reader->IsEmptyElement();
                StartElement(reader.Get());
                if (empty) EndElement();
            } else if (type == XmlNodeType_EndElement) {
                EndElement();
            } else if (type == XmlNodeType_Text || type == XmlNodeType_CDATA || type == XmlNodeType_Whitespace) {
                const wchar_t* value;
                UINT len;
                CheckHr(reader->GetValue(&value, &len), L"XmlLite");
                Characters(std::wstring_view(value, len));
            }
        }
        // Browsers refuse malformed XML too; drawing a partial set of labels would hide the problem.
        if (FAILED(hr)) Fail(L"SVG no válido: el XML está mal formado");
    }

private:
    void StartElement(IXmlReader* reader) {
        const wchar_t *name, *ns;
        CheckHr(reader->GetLocalName(&name, nullptr), L"XmlLite");
        CheckHr(reader->GetNamespaceUri(&ns, nullptr), L"XmlLite");
        std::wstring tag = name;
        bool svgElement = !*ns || !wcscmp(ns, kSvgNs);

        Frame f;
        if (!frames_.empty()) f = frames_.back();
        f.styleSheet = f.textRoot = false;
        bool isRoot = frames_.empty();
        if (!f.skip && (!svgElement || std::find(std::begin(kSkippedElements), std::end(kSkippedElements), tag) !=
                                           std::end(kSkippedElements))) {
            f.skip = true;
            f.styleSheet = svgElement && tag == L"style";
        }
        if (f.skip) {
            frames_.push_back(std::move(f));
            return;
        }

        std::map<std::wstring, std::wstring> attrs = ReadAttributes(reader, f.style);
        const TextStyle& parent = frames_.empty() ? TextStyle() : frames_.back().style;
        ElementStyle e{f.style};
        ApplyStyles(e, parent, tag, attrs);
        if (e.displayNone) {
            f.skip = true;
            frames_.push_back(std::move(f));
            return;
        }
        f.style = e.style;
        f.style.opacity = parent.opacity * e.ownOpacity;
        if (auto t = attrs.find(L"transform"); t != attrs.end()) f.transform = ParseTransform(t->second) * f.transform;
        // A nested <svg> opens a new viewport at x/y; its own viewBox is not applied.
        if (tag == L"svg" && !isRoot) {
            float x = 0, y = 0;
            if (auto a = attrs.find(L"x"); a != attrs.end()) FirstNumber(a->second, x);
            if (auto a = attrs.find(L"y"); a != attrs.end()) FirstNumber(a->second, y);
            f.transform = D2D1::Matrix3x2F::Translation(x, y) * f.transform;
        }

        if (tag == L"text" && !inText_) {
            inText_ = true;
            f.textRoot = true;
            pen_ = D2D1::Point2F();
            StartChunk(f.style.anchor);
        }
        if (inText_ && (tag == L"text" || tag == L"tspan")) Position(f.style, attrs);
        frames_.push_back(std::move(f));
    }

    void EndElement() {
        if (frames_.empty()) return;
        Frame f = std::move(frames_.back());
        frames_.pop_back();
        if (f.styleSheet) {
            ParseStyleSheet(std::move(css_), rules_);
            css_.clear();
        }
        if (f.textRoot) {
            FlushChunk();
            inText_ = false;
        }
    }

    // Unprefixed attributes by name; xml:space goes straight into the style since it inherits.
    std::map<std::wstring, std::wstring> ReadAttributes(IXmlReader* reader, TextStyle& style) {
        std::map<std::wstring, std::wstring> attrs;
        for (HRESULT hr = reader->MoveToFirstAttribute(); hr == S_OK; hr = reader->MoveToNextAttribute()) {
            const wchar_t *name, *ns, *value;
            CheckHr(reader->GetLocalName(&name, nullptr), L"XmlLite");
            CheckHr(reader->GetNamespaceUri(&ns, nullptr), L"XmlLite");
            CheckHr(reader->GetValue(&value, nullptr), L"XmlLite");
            if (!*ns) attrs[name] = value;
            else if (!wcscmp(ns, kXmlNs) && !wcscmp(name, L"space")) style.preserveSpace = !wcscmp(value, L"preserve");
        }
        CheckHr(reader->MoveToElement(), L"XmlLite");
        return attrs;
    }

    // Cascade order: presentation attributes, then <style> rules by specificity, then style="".
    void ApplyStyles(ElementStyle& e, const TextStyle& parent, const std::wstring& tag,
                     const std::map<std::wstring, std::wstring>& attrs) {
        for (auto& [name, value] : attrs) ApplyProperty(e, parent, name, value);
        auto applyRule = [&](const std::wstring& selector) {
            auto rule = rules_.find(selector);
            if (rule == rules_.end()) return;
            for (auto& [name, value] : rule->second) ApplyProperty(e, parent, name, value);
        };
        if (!rules_.empty()) {
            applyRule(tag);
            if (auto cls = attrs.find(L"class"); cls != attrs.end()) {
                std::wstring_view list = cls->second;
                while (!list.empty()) {
                    size_t sp = list.find_first_of(L" \t\r\n");
                    if (sp != 0) applyRule(L"." + std::wstring(list.substr(0, sp)));
                    if (sp == std::wstring_view::npos) break;
                    list.remove_prefix(sp + 1);
                }
            }
            if (auto id = attrs.find(L"id"); id != attrs.end()) applyRule(L"#" + id->second);
        }
        if (auto style = attrs.find(L"style"); style != attrs.end())
            for (auto& [name, value] : ParseDeclarations(style->second)) ApplyProperty(e, parent, name, value);
    }

    // An absolute x or y starts a new text chunk, which is the unit text-anchor aligns.
    void Position(const TextStyle& style, const std::map<std::wstring, std::wstring>& attrs) {
        float v;
        bool moved = false;
        if (auto a = attrs.find(L"x"); a != attrs.end() && FirstNumber(a->second, v)) {
            if (!moved) FlushChunk();
            pen_.x = v;
            moved = true;
        }
        if (auto a = attrs.find(L"y"); a != attrs.end() && FirstNumber(a->second, v)) {
            if (!moved) FlushChunk();
            pen_.y = v;
            moved = true;
        }
        if (moved) StartChunk(style.anchor);
        if (auto a = attrs.find(L"dx"); a != attrs.end() && FirstNumber(a->second, v)) pen_.x += v;
        if (auto a = attrs.find(L"dy"); a != attrs.end() && FirstNumber(a->second, v)) pen_.y += v;
    }

    void StartChunk(Anchor anchor) {
        chunkAnchor_ = anchor;
        chunkStartX_ = pen_.x;
        pendingSpace_ = false;
    }

    void Characters(std::wstring_view raw) {
        if (frames_.empty()) return;
        const Frame& f = frames_.back();
        if (f.styleSheet) {
            css_ += raw;
            return;
        }
        if (f.skip || !inText_) return;
        std::wstring text = NormalizeSpace(raw, f.style.preserveSpace);
        if (text.empty()) return;
        AddRun(f, text);
    }

    // Outside xml:space="preserve" whitespace collapses to one space and never starts or ends a
    // chunk; a space between two runs is kept by remembering it until the next visible text.
    std::wstring NormalizeSpace(std::wstring_view raw, bool preserve) {
        std::wstring text;
        text.reserve(raw.size());
        for (wchar_t c : raw) {
            if (!IsSpace(c)) text += c;
            else if (preserve) text += L' ';
            else if (text.empty() || text.back() != L' ') text += L' ';
        }
        if (preserve) return text;
        bool leading = !text.empty() && text.front() == L' ';
        bool trailing = !text.empty() && text.back() == L' ';
        std::wstring trimmed(TrimView(text));
        if (trimmed.empty()) {
            pendingSpace_ = pendingSpace_ || leading;
            return trimmed;
        }
        if ((leading || pendingSpace_) && pen_.x != chunkStartX_) trimmed.insert(0, 1, L' ');
        pendingSpace_ = trailing;
        return trimmed;
    }

    void AddRun(const Frame& f, const std::wstring& text) {
        const TextStyle& s = f.style;
        ComPtr<IDWriteTextFormat> format;
        CheckHr(DWrite()->CreateTextFormat(fonts_.Resolve(s.fontFamily).c_str(), nullptr, DWRITE_FONT_WEIGHT(s.weight),
                                           s.fontStyle, DWRITE_FONT_STRETCH_NORMAL, s.fontSize, L"", &format),
                L"DirectWrite");
        CheckHr(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), L"DirectWrite");
        Run run;
        CheckHr(DWrite()->CreateTextLayout(text.c_str(), (UINT32)text.size(), format.Get(), 0, 0, &run.layout),
                L"DirectWrite");
        DWRITE_TEXT_METRICS metrics;
        CheckHr(run.layout->GetMetrics(&metrics), L"DirectWrite");
        DWRITE_LINE_METRICS line;
        UINT32 lineCount;
        CheckHr(run.layout->GetLineMetrics(&line, 1, &lineCount), L"DirectWrite");

        run.baselineOrigin = pen_;
        run.baseline = line.baseline;
        run.color = s.fill;
        run.color.a *= s.fillOpacity * s.opacity;
        run.transform = f.transform;
        // Hidden text still takes its space in the chunk.
        pen_.x += metrics.widthIncludingTrailingWhitespace;
        if (s.visible && !s.fillNone && run.color.a > 0) chunk_.push_back(std::move(run));
    }

    void FlushChunk() {
        float width = pen_.x - chunkStartX_;
        float shift = chunkAnchor_ == Anchor::Middle ? width / 2 : chunkAnchor_ == Anchor::End ? width : 0;
        for (const Run& run : chunk_) {
            ComPtr<ID2D1SolidColorBrush> brush;
            CheckHr(target_->CreateSolidColorBrush(run.color, &brush), L"Direct2D");
            target_->SetTransform(run.transform * toPixels_);
            D2D1_POINT_2F topLeft = D2D1::Point2F(run.baselineOrigin.x - shift, run.baselineOrigin.y - run.baseline);
            // NO_SNAP: glyphs may be scaled or rotated, and pixel snapping would misplace them.
            target_->DrawTextLayout(topLeft, run.layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
        }
        chunk_.clear();
    }

    ID2D1RenderTarget* target_;
    D2D1::Matrix3x2F toPixels_;
    FontResolver fonts_;
    std::vector<Frame> frames_;
    std::unordered_map<std::wstring, Declarations> rules_;
    std::wstring css_;
    bool inText_ = false;
    D2D1_POINT_2F pen_{};
    float chunkStartX_ = 0;
    Anchor chunkAnchor_ = Anchor::Start;
    bool pendingSpace_ = false;
    std::vector<Run> chunk_;
};

}  // namespace

void DrawSvgText(ID2D1RenderTarget* target, const uint8_t* data, size_t size, const D2D1_MATRIX_3X2_F& toPixels) {
    D2D1_MATRIX_3X2_F saved;
    target->GetTransform(&saved);
    // ClearType needs an opaque background; the bitmap here is transparent.
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    TextPainter(target, toPixels).Paint(data, size);
    target->SetTransform(saved);
}
