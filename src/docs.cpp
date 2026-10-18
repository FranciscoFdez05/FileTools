#include "docs.h"

#include "image.h"
#include "pdf.h"
#include "zip.h"

#include <algorithm>
#include <cwctype>

namespace {

// ---------------------------------------------------------------------------
// Inline markup

struct Run {
    std::wstring text;
    bool bold = false, italic = false, code = false;
};

std::vector<Run> ParseInline(const std::wstring& s, bool markup) {
    if (!markup) return {Run{s}};
    std::vector<Run> runs;
    Run cur;
    auto flush = [&] {
        if (!cur.text.empty()) runs.push_back(cur);
        cur.text.clear();
    };
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == L'\\' && i + 1 < s.size()) {
            cur.text += s[++i];
        } else if (c == L'`') {
            flush();
            cur.code = !cur.code;
        } else if (cur.code) {
            cur.text += c;
        } else if ((c == L'*' || c == L'_') && i + 1 < s.size() && s[i + 1] == c) {
            flush();
            cur.bold = !cur.bold;
            ++i;
        } else if (c == L'*') {
            flush();
            cur.italic = !cur.italic;
        } else if (c == L'[') {
            // [text](url) -> text
            size_t close = s.find(L"](", i);
            size_t end = close == std::wstring::npos ? close : s.find(L')', close);
            if (end != std::wstring::npos) {
                cur.text += s.substr(i + 1, close - i - 1);
                i = end;
            } else {
                cur.text += c;
            }
        } else {
            cur.text += c;
        }
    }
    flush();
    return runs;
}

std::wstring PlainText(const DocBlock& b) {
    std::wstring out;
    for (auto& r : ParseInline(b.text, b.markup)) out += r.text;
    return out;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

// ---------------------------------------------------------------------------
// XML helpers

std::string XmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default:
                if ((unsigned char)c >= 0x20 || c == '\t' || c == '\n') out += c;
        }
    }
    return out;
}

std::wstring HtmlEscape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        switch (c) {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'"': out += L"&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::wstring DecodeEntities(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'&') {
            out += s[i];
            continue;
        }
        size_t semi = s.find(L';', i);
        if (semi == std::wstring::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        std::wstring ent = s.substr(i + 1, semi - i - 1);
        wchar_t ch = 0;
        if (ent == L"amp") ch = L'&';
        else if (ent == L"lt") ch = L'<';
        else if (ent == L"gt") ch = L'>';
        else if (ent == L"quot") ch = L'"';
        else if (ent == L"apos" || ent == L"#39") ch = L'\'';
        else if (ent == L"nbsp") ch = L' ';
        else if (ent == L"mdash") ch = L'—';
        else if (ent == L"ndash") ch = L'–';
        else if (ent == L"hellip") ch = L'…';
        else if (ent == L"copy") ch = L'©';
        else if (!ent.empty() && ent[0] == L'#') {
            unsigned long v = (ent.size() > 1 && (ent[1] == L'x' || ent[1] == L'X')) ? wcstoul(ent.c_str() + 2, nullptr, 16)
                                                                                    : wcstoul(ent.c_str() + 1, nullptr, 10);
            if (v > 0 && v < 0x10000) ch = wchar_t(v);
        }
        if (ch) {
            out += ch;
            i = semi;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string Base64(const std::vector<uint8_t>& d) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((d.size() + 2) / 3 * 4);
    for (size_t i = 0; i < d.size(); i += 3) {
        uint32_t v = uint32_t(d[i]) << 16;
        if (i + 1 < d.size()) v |= uint32_t(d[i + 1]) << 8;
        if (i + 2 < d.size()) v |= d[i + 2];
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += i + 1 < d.size() ? t[(v >> 6) & 63] : '=';
        out += i + 2 < d.size() ? t[v & 63] : '=';
    }
    return out;
}

// ---------------------------------------------------------------------------
// Readers

Doc ParseText(const std::wstring& text) {
    Doc doc;
    std::wstring para;
    size_t start = 0;
    auto flush = [&] {
        if (!Trim(para).empty()) doc.push_back(DocBlock::Paragraph(para));
        para.clear();
    };
    while (start <= text.size()) {
        size_t nl = text.find(L'\n', start);
        std::wstring line = text.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.find(L'\f') != std::wstring::npos) {
            flush();
            DocBlock pb;
            pb.kind = DocBlock::PageBreak;
            doc.push_back(pb);
        } else if (Trim(line).empty()) {
            flush();
        } else {
            if (!para.empty()) para += L'\n';
            para += line;
        }
        if (nl == std::wstring::npos) break;
        start = nl + 1;
    }
    flush();
    return doc;
}

Doc ParseMarkdown(const std::wstring& text) {
    Doc doc;
    std::wstring para;
    bool inFence = false;
    std::wstring fence;
    auto flush = [&] {
        if (!Trim(para).empty()) doc.push_back(DocBlock::Paragraph(Trim(para), true));
        para.clear();
    };
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find(L'\n', start);
        std::wstring line = text.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::wstring t = Trim(line);
        if (t.starts_with(L"```")) {
            if (inFence) {
                DocBlock b = DocBlock::Paragraph(L"`" + fence + L"`", true);
                doc.push_back(b);
                fence.clear();
            } else {
                flush();
            }
            inFence = !inFence;
        } else if (inFence) {
            if (!fence.empty()) fence += L'\n';
            fence += line;
        } else if (t.empty()) {
            flush();
        } else if (t[0] == L'#') {
            flush();
            int level = 0;
            while (level < (int)t.size() && t[level] == L'#') ++level;
            DocBlock b = DocBlock::Paragraph(Trim(t.substr(level)), true);
            b.kind = DocBlock::Heading;
            b.level = std::clamp(level, 1, 6);
            doc.push_back(b);
        } else if (t == L"---" || t == L"***") {
            flush();
        } else if ((t[0] == L'-' || t[0] == L'*' || t[0] == L'+') && t.size() > 1 && t[1] == L' ') {
            flush();
            DocBlock b = DocBlock::Paragraph(Trim(t.substr(2)), true);
            b.kind = DocBlock::Bullet;
            doc.push_back(b);
        } else if (iswdigit(t[0]) && t.find(L". ") != std::wstring::npos && t.find(L". ") < 4) {
            flush();
            doc.push_back(DocBlock::Paragraph(t, true));
        } else if (t[0] == L'>') {
            flush();
            doc.push_back(DocBlock::Paragraph(L"*" + Trim(t.substr(1)) + L"*", true));
        } else {
            if (!para.empty()) para += L' ';
            para += t;
        }
        if (nl == std::wstring::npos) break;
        start = nl + 1;
    }
    flush();
    return doc;
}

Doc ParseHtml(const std::wstring& html) {
    Doc doc;
    DocBlock cur;
    cur.markup = true;
    auto flush = [&] {
        std::wstring t = Trim(cur.text);
        if (!t.empty() && t != L"**" && t != L"*") {
            cur.text = t;
            doc.push_back(cur);
        }
        cur = DocBlock();
        cur.markup = true;
    };
    bool pre = false;
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] == L'<') {
            size_t end = html.find(L'>', i);
            if (end == std::wstring::npos) break;
            std::wstring tag = ToLower(html.substr(i + 1, end - i - 1));
            i = end + 1;
            if (tag.starts_with(L"!--")) {
                size_t c = html.find(L"-->", i - 1);
                i = c == std::wstring::npos ? html.size() : c + 3;
                continue;
            }
            bool closing = !tag.empty() && tag[0] == L'/';
            if (closing) tag.erase(0, 1);
            std::wstring name = tag.substr(0, tag.find_first_of(L" \t\r\n/"));
            if (!closing && (name == L"script" || name == L"style" || name == L"head" || name == L"title")) {
                size_t c = ToLower(html.substr(i)).find(L"</" + name);
                i = c == std::wstring::npos ? html.size() : i + c;
                continue;
            }
            if (name.size() == 2 && name[0] == L'h' && name[1] >= L'1' && name[1] <= L'6') {
                flush();
                if (!closing) {
                    cur.kind = DocBlock::Heading;
                    cur.level = name[1] - L'0';
                }
            } else if (name == L"li") {
                flush();
                if (!closing) cur.kind = DocBlock::Bullet;
            } else if (name == L"p" || name == L"div" || name == L"section" || name == L"article" || name == L"tr" ||
                       name == L"blockquote" || name == L"table" || name == L"ul" || name == L"ol" ||
                       name == L"header" || name == L"footer" || name == L"main" || name == L"nav") {
                flush();
            } else if (name == L"pre") {
                flush();
                pre = !closing;
            } else if (name == L"br") {
                cur.text += L'\n';
            } else if (name == L"td" || name == L"th") {
                if (closing) cur.text += L"\t";
            } else if (name == L"b" || name == L"strong") {
                cur.text += L"**";
            } else if (name == L"i" || name == L"em") {
                cur.text += L"*";
            }
        } else {
            size_t next = html.find(L'<', i);
            std::wstring chunk = html.substr(i, next == std::wstring::npos ? std::wstring::npos : next - i);
            i = next == std::wstring::npos ? html.size() : next;
            if (!pre) {
                std::wstring collapsed;
                bool space = false;
                for (wchar_t c : chunk) {
                    if (iswspace(c)) {
                        space = true;
                    } else {
                        if (space && !collapsed.empty()) collapsed += L' ';
                        else if (space && !cur.text.empty() && cur.text.back() != L' ' && cur.text.back() != L'\n')
                            collapsed += L' ';
                        space = false;
                        collapsed += c;
                    }
                }
                if (space && !collapsed.empty()) collapsed += L' ';
                chunk = collapsed;
            }
            std::wstring decoded = DecodeEntities(chunk);
            // Escape literal markup characters so they are not read as formatting.
            for (wchar_t c : decoded) {
                if (c == L'*' || c == L'`' || c == L'[' || c == L'\\') cur.text += L'\\';
                cur.text += c;
            }
        }
    }
    flush();
    return doc;
}

std::string AttrValue(const std::string& xml, size_t from, size_t to, const char* attr) {
    std::string key = std::string(attr) + "=\"";
    size_t a = xml.find(key, from);
    if (a == std::string::npos || a > to) return {};
    a += key.size();
    size_t b = xml.find('"', a);
    return b == std::string::npos ? std::string() : xml.substr(a, b - a);
}

Doc ParseDocx(const fs::path& path) {
    ZipReader zip(path);
    auto bytes = zip.Read("word/document.xml");
    std::string xml(bytes.begin(), bytes.end());
    Doc doc;
    size_t pos = 0;
    while ((pos = xml.find("<w:p", pos)) != std::string::npos) {
        char c = pos + 4 < xml.size() ? xml[pos + 4] : 0;
        if (c != '>' && c != ' ' && c != '/') {
            pos += 4;
            continue;
        }
        size_t tagEnd = xml.find('>', pos);
        if (tagEnd == std::string::npos) break;
        if (xml[tagEnd - 1] == '/') {  // empty paragraph
            pos = tagEnd + 1;
            continue;
        }
        size_t end = xml.find("</w:p>", tagEnd);
        if (end == std::string::npos) break;
        DocBlock b;
        b.markup = true;
        std::string style = AttrValue(xml, tagEnd, xml.find("</w:pPr>", tagEnd) < end ? xml.find("</w:pPr>", tagEnd) : tagEnd,
                                      "<w:pStyle w:val");
        if (style.starts_with("Heading") && style.size() > 7 && isdigit((unsigned char)style[7])) {
            b.kind = DocBlock::Heading;
            b.level = std::clamp(style[7] - '0', 1, 6);
        } else if (style == "Title") {
            b.kind = DocBlock::Heading;
            b.level = 1;
        } else if (style == "Subtitle") {
            b.kind = DocBlock::Heading;
            b.level = 2;
        } else if (xml.find("<w:numPr>", tagEnd) < end) {
            b.kind = DocBlock::Bullet;
        }
        std::wstring text;
        bool pageBreak = false;
        size_t r = tagEnd;
        while ((r = xml.find("<w:r", r)) != std::string::npos && r < end) {
            char rc = xml[r + 4];
            if (rc != '>' && rc != ' ') {
                r += 4;
                continue;
            }
            size_t rEnd = xml.find("</w:r>", r);
            if (rEnd == std::string::npos || rEnd > end) rEnd = end;
            size_t rprEnd = xml.find("</w:rPr>", r);
            bool bold = false, italic = false;
            if (rprEnd != std::string::npos && rprEnd < rEnd) {
                std::string rpr = xml.substr(r, rprEnd - r);
                bold = (rpr.find("<w:b/>") != std::string::npos || rpr.find("<w:b w:val=\"1\"") != std::string::npos ||
                        rpr.find("<w:b w:val=\"true\"") != std::string::npos);
                italic = (rpr.find("<w:i/>") != std::string::npos || rpr.find("<w:i w:val=\"1\"") != std::string::npos ||
                          rpr.find("<w:i w:val=\"true\"") != std::string::npos);
            }
            std::wstring runText;
            size_t k = r;
            while (k < rEnd) {
                size_t lt = xml.find('<', k);
                if (lt == std::string::npos || lt >= rEnd) break;
                if (xml.compare(lt, 4, "<w:t") == 0 && (xml[lt + 4] == '>' || xml[lt + 4] == ' ')) {
                    size_t gt = xml.find('>', lt);
                    size_t close = xml.find("</w:t>", gt);
                    if (close == std::string::npos || close > rEnd) break;
                    std::wstring t = DecodeEntities(Utf8ToWide(xml.substr(gt + 1, close - gt - 1)));
                    for (wchar_t ch : t) {
                        if (ch == L'*' || ch == L'`' || ch == L'[' || ch == L'\\') runText += L'\\';
                        runText += ch;
                    }
                    k = close + 6;
                    continue;
                }
                if (xml.compare(lt, 8, "<w:tab/>") == 0) runText += L'\t';
                else if (xml.compare(lt, 7, "<w:cr/>") == 0) runText += L'\n';
                else if (xml.compare(lt, 5, "<w:br") == 0) {
                    size_t gt = xml.find('>', lt);
                    if (xml.substr(lt, gt - lt).find("type=\"page\"") != std::string::npos) pageBreak = true;
                    else runText += L'\n';
                }
                k = lt + 1;
            }
            if (!runText.empty()) {
                std::wstring mark = bold ? (italic ? L"***" : L"**") : (italic ? L"*" : L"");
                text += mark + runText + mark;
            }
            r = rEnd;
        }
        if (pageBreak) {
            DocBlock pb;
            pb.kind = DocBlock::PageBreak;
            doc.push_back(pb);
        }
        // Bullets written by WriteDocx are plain paragraphs starting with "•<tab>".
        if (b.kind == DocBlock::Para && text.starts_with(L"•\t")) {
            b.kind = DocBlock::Bullet;
            text.erase(0, 2);
        }
        b.text = text;
        if (!Trim(PlainText(b)).empty()) doc.push_back(b);
        pos = end + 6;
    }
    return doc;
}

// ---------------------------------------------------------------------------
// Writers

std::wstring ToTxt(const Doc& doc) {
    std::wstring out;
    for (const auto& b : doc) {
        switch (b.kind) {
            case DocBlock::Heading: out += PlainText(b) + L"\r\n\r\n"; break;
            case DocBlock::Bullet: out += L"• " + PlainText(b) + L"\r\n"; break;
            case DocBlock::Para: out += PlainText(b) + L"\r\n\r\n"; break;
            case DocBlock::PageBreak: out += L"\r\n"; break;
            case DocBlock::Picture: break;
        }
    }
    // Normalise bare newlines inside paragraphs to CRLF.
    std::wstring norm;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == L'\n' && (i == 0 || out[i - 1] != L'\r')) norm += L'\r';
        norm += out[i];
    }
    return norm;
}

std::wstring MdText(const DocBlock& b) {
    if (b.markup) return b.text;
    std::wstring out;
    for (wchar_t c : b.text) {
        if (c == L'*' || c == L'_' || c == L'`' || c == L'#') out += L'\\';
        out += c;
    }
    return out;
}

std::wstring ToMarkdown(const Doc& doc) {
    std::wstring out;
    bool prevBullet = false;
    for (const auto& b : doc) {
        if (prevBullet && b.kind != DocBlock::Bullet) out += L"\n";
        switch (b.kind) {
            case DocBlock::Heading: out += std::wstring(b.level, L'#') + L" " + MdText(b) + L"\n\n"; break;
            case DocBlock::Bullet: out += L"- " + MdText(b) + L"\n"; break;
            case DocBlock::Para: {
                std::wstring t = MdText(b);
                // Hard line breaks inside paragraphs: two trailing spaces.
                std::wstring withBreaks;
                for (wchar_t c : t) withBreaks += c == L'\n' ? std::wstring(L"  \n") : std::wstring(1, c);
                out += withBreaks + L"\n\n";
                break;
            }
            case DocBlock::PageBreak: out += L"---\n\n"; break;
            case DocBlock::Picture: break;
        }
        prevBullet = b.kind == DocBlock::Bullet;
    }
    return out;
}

std::wstring RunsToHtml(const DocBlock& b) {
    std::wstring out;
    for (const auto& r : ParseInline(b.text, b.markup)) {
        std::wstring t = HtmlEscape(r.text);
        std::wstring withBr;
        for (wchar_t c : t) withBr += c == L'\n' ? std::wstring(L"<br>") : std::wstring(1, c);
        if (r.code) withBr = L"<code>" + withBr + L"</code>";
        if (r.italic) withBr = L"<em>" + withBr + L"</em>";
        if (r.bold) withBr = L"<strong>" + withBr + L"</strong>";
        out += withBr;
    }
    return out;
}

std::wstring ToHtml(const Doc& doc, const std::wstring& title) {
    std::wstring out =
        L"<!DOCTYPE html>\n<html lang=\"es\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" "
        L"content=\"width=device-width, initial-scale=1\">\n<title>" +
        HtmlEscape(title) +
        L"</title>\n<style>body{font-family:'Segoe UI',system-ui,sans-serif;max-width:760px;margin:40px "
        L"auto;padding:0 16px;line-height:1.6;color:#1f2937}img{max-width:100%;height:auto}code{background:#f3f4f6;"
        L"padding:1px 4px;border-radius:4px}hr{border:0;border-top:1px solid #e5e7eb;margin:32px "
        L"0}</style>\n</head>\n<body>\n";
    bool inList = false;
    for (const auto& b : doc) {
        if (b.kind != DocBlock::Bullet && inList) {
            out += L"</ul>\n";
            inList = false;
        }
        switch (b.kind) {
            case DocBlock::Heading: {
                std::wstring h = L"h" + std::to_wstring(b.level);
                out += L"<" + h + L">" + RunsToHtml(b) + L"</" + h + L">\n";
                break;
            }
            case DocBlock::Bullet:
                if (!inList) out += L"<ul>\n";
                inList = true;
                out += L"<li>" + RunsToHtml(b) + L"</li>\n";
                break;
            case DocBlock::Para: out += L"<p>" + RunsToHtml(b) + L"</p>\n"; break;
            case DocBlock::PageBreak: out += L"<hr>\n"; break;
            case DocBlock::Picture:
                out += L"<p><img alt=\"\" src=\"data:image/" + Utf8ToWide(b.imageExt) + L";base64," +
                       Utf8ToWide(Base64(b.image)) + L"\"></p>\n";
                break;
        }
    }
    if (inList) out += L"</ul>\n";
    out += L"</body>\n</html>\n";
    return out;
}

std::string DocxRuns(const DocBlock& b) {
    std::string out;
    for (const auto& r : ParseInline(b.text, b.markup)) {
        std::string rpr;
        if (r.bold) rpr += "<w:b/>";
        if (r.italic) rpr += "<w:i/>";
        if (r.code) rpr += "<w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\"/>";
        std::string run = "<w:r>" + (rpr.empty() ? "" : "<w:rPr>" + rpr + "</w:rPr>");
        std::string text = WideToUtf8(r.text);
        std::string piece;
        auto emit = [&] {
            if (!piece.empty()) run += "<w:t xml:space=\"preserve\">" + XmlEscape(piece) + "</w:t>";
            piece.clear();
        };
        for (char c : text) {
            if (c == '\n') {
                emit();
                run += "<w:br/>";
            } else if (c == '\t') {
                emit();
                run += "<w:tab/>";
            } else if (c != '\r') {
                piece += c;
            }
        }
        emit();
        out += run + "</w:r>";
    }
    return out;
}

void WriteDocx(const fs::path& path, const Doc& doc) {
    std::string body;
    std::string rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships "
        "xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>";
    ZipWriter zip;
    int pic = 0;
    for (const auto& b : doc) {
        switch (b.kind) {
            case DocBlock::Heading:
                body += "<w:p><w:pPr><w:pStyle w:val=\"Heading" + std::to_string(b.level) + "\"/></w:pPr>" +
                        DocxRuns(b) + "</w:p>";
                break;
            case DocBlock::Bullet: {
                DocBlock bullet = b;
                bullet.text = L"•\t" + b.text;
                body += "<w:p><w:pPr><w:spacing w:after=\"60\"/><w:ind w:left=\"360\" w:hanging=\"360\"/></w:pPr>" +
                        DocxRuns(bullet) + "</w:p>";
                break;
            }
            case DocBlock::Para: body += "<w:p>" + DocxRuns(b) + "</w:p>"; break;
            case DocBlock::PageBreak: body += "<w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>"; break;
            case DocBlock::Picture: {
                ++pic;
                std::string rid = "rId" + std::to_string(100 + pic);
                std::string file = "image" + std::to_string(pic) + "." + b.imageExt;
                zip.Add("word/media/" + file, b.image.data(), b.image.size());
                rels += "<Relationship Id=\"" + rid +
                        "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" "
                        "Target=\"media/" +
                        file + "\"/>";
                // 96 DPI pixels -> EMU, fitted to the A4 text area.
                double cx = b.w * 9525.0, cy = b.h * 9525.0;
                double maxW = 5731510.0, maxH = 8229600.0;
                double s = std::min({1.0, maxW / cx, maxH / cy});
                std::string ext = "cx=\"" + std::to_string((long long)(cx * s)) + "\" cy=\"" +
                                  std::to_string((long long)(cy * s)) + "\"";
                std::string id = std::to_string(pic);
                body += "<w:p><w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent " +
                        ext + "/><wp:docPr id=\"" + id + "\" name=\"Imagen " + id +
                        "\"/><a:graphic><a:graphicData "
                        "uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:pic><pic:nvPicPr><pic:"
                        "cNvPr id=\"" +
                        id + "\" name=\"" + file + "\"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed=\"" +
                        rid +
                        "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr><a:xfrm><a:off x=\"0\" "
                        "y=\"0\"/><a:ext " +
                        ext +
                        "/></a:xfrm><a:prstGeom "
                        "prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic></"
                        "wp:inline></w:drawing></w:r></w:p>";
                break;
            }
        }
    }
    rels += "</Relationships>";
    std::string document =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:document "
        "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
        "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
        "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><w:body>" +
        body +
        "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" "
        "w:left=\"1440\" w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/></w:sectPr></w:body></w:document>";

    std::string styles =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:styles "
        "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:docDefaults><w:rPrDefault><w:rPr>"
        "<w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\" w:eastAsia=\"Calibri\" w:cs=\"Calibri\"/><w:sz "
        "w:val=\"22\"/><w:szCs w:val=\"22\"/><w:lang w:val=\"es-ES\"/></w:rPr></w:rPrDefault><w:pPrDefault><w:pPr>"
        "<w:spacing w:after=\"160\" w:line=\"276\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault></w:docDefaults>"
        "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name "
        "w:val=\"Normal\"/><w:qFormat/></w:style>";
    const int sizes[6] = {36, 30, 26, 24, 22, 22};
    for (int n = 1; n <= 6; ++n) {
        std::string sz = std::to_string(sizes[n - 1]);
        styles += "<w:style w:type=\"paragraph\" w:styleId=\"Heading" + std::to_string(n) +
                  "\"><w:name w:val=\"heading " + std::to_string(n) +
                  "\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/><w:pPr><w:keepNext/>"
                  "<w:spacing w:before=\"240\" w:after=\"80\"/><w:outlineLvl w:val=\"" +
                  std::to_string(n - 1) + "\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"" + sz + "\"/><w:szCs w:val=\"" + sz +
                  "\"/><w:color w:val=\"1F2937\"/></w:rPr></w:style>";
    }
    styles += "</w:styles>";

    zip.Add("[Content_Types].xml",
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Types "
            "xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"rels\" "
            "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" "
            "ContentType=\"application/xml\"/><Default Extension=\"png\" ContentType=\"image/png\"/><Default "
            "Extension=\"jpeg\" ContentType=\"image/jpeg\"/><Override PartName=\"/word/document.xml\" "
            "ContentType=\"application/"
            "vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/><Override "
            "PartName=\"/word/styles.xml\" "
            "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/></Types>");
    zip.Add("_rels/.rels",
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships "
            "xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" "
            "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
            "Target=\"word/document.xml\"/></Relationships>");
    zip.Add("word/document.xml", document);
    zip.Add("word/styles.xml", styles);
    zip.Add("word/_rels/document.xml.rels", rels);
    WriteFileBytes(path, zip.Finish());
}

// --- text PDF ---------------------------------------------------------------

const short kHelvetica[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556,
    556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778,
    722, 278, 500, 667, 556, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278,
    278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,
    556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};

std::string ToWinAnsi(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(1252, 0, s.data(), (int)s.size(), nullptr, 0, "?", nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(1252, 0, s.data(), (int)s.size(), out.data(), n, "?", nullptr);
    return out;
}

const short kHelveticaBold[95] = {
    278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556,
    556, 556, 556, 556, 556, 556, 333, 333, 584, 584, 584, 611, 975, 722, 722, 722, 722, 667, 611, 778,
    722, 278, 556, 722, 611, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333,
    278, 333, 584, 556, 333, 556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611,
    611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584};

double AnsiWidth(const std::string& s, int font, double size) {
    // Accented Latin-1 letters measure like their base letter.
    static const char kBase[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    const short* table = font == 2 ? kHelveticaBold : kHelvetica;
    double w = 0;
    for (unsigned char c : s) {
        if (c >= 0xC0) c = (unsigned char)kBase[c - 0xC0];
        w += (c >= 32 && c <= 126) ? table[c - 32] : 556;
    }
    return w * size / 1000.0;
}

std::string PdfString(const std::string& s) {
    std::string out = "(";
    for (unsigned char c : s) {
        if (c == '(' || c == ')' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 32 || c >= 128) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\%03o", c);
            out += buf;
        } else {
            out += char(c);
        }
    }
    return out + ")";
}

void WriteTextPdf(const fs::path& path, const Doc& doc) {
    const double pageW = 595.28, pageH = 841.89, margin = 60, usable = pageW - margin * 2;
    PdfWriter pdf;
    int f1 = pdf.AddObject("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
    int f2 = pdf.AddObject("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>");
    int f3 =
        pdf.AddObject("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Oblique /Encoding /WinAnsiEncoding >>");
    std::string fonts = "/Font << /F1 " + std::to_string(f1) + " 0 R /F2 " + std::to_string(f2) + " 0 R /F3 " +
                        std::to_string(f3) + " 0 R >>";

    std::string content, xobjects;
    double y = pageH - margin;
    int imageCount = 0;
    auto newPage = [&] {
        int c = pdf.AddStream("", content);
        pdf.AddPage(pageW, pageH, "<< " + fonts + (xobjects.empty() ? "" : " /XObject << " + xobjects + " >>") + " >>",
                    c);
        content.clear();
        xobjects.clear();
        y = pageH - margin;
    };
    char buf[128];
    for (const auto& b : doc) {
        if (b.kind == DocBlock::PageBreak) {
            if (!content.empty()) newPage();
            continue;
        }
        if (b.kind == DocBlock::Picture) {
            Image img = LoadImageMemory(b.image.data(), b.image.size());
            double s = std::min({1.0, usable / (img.w * 0.75), (pageH - margin * 2) / (img.h * 0.75)});
            double w = img.w * 0.75 * s, h = img.h * 0.75 * s;
            if (y - h < margin) newPage();
            std::vector<uint8_t> jpeg = EncodeImage(img, ImgFmt::Jpg);
            int id = pdf.AddStream("/Type /XObject /Subtype /Image /Width " + std::to_string(img.w) + " /Height " +
                                       std::to_string(img.h) +
                                       " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode",
                                   std::string(jpeg.begin(), jpeg.end()));
            std::string name = "Im" + std::to_string(imageCount++);
            xobjects += "/" + name + " " + std::to_string(id) + " 0 R ";
            y -= h;
            snprintf(buf, sizeof(buf), "q %.2f 0 0 %.2f %.2f %.2f cm /%s Do Q\n", w, h, margin, y, name.c_str());
            content += buf;
            y -= 12;
            continue;
        }
        double size = 11, indent = 0;
        int baseFont = 1;
        if (b.kind == DocBlock::Heading) {
            static const double hs[6] = {20, 16, 14, 12.5, 11.5, 11};
            size = hs[std::clamp(b.level, 1, 6) - 1];
            baseFont = 2;
            y -= size * 0.6;
        }
        if (b.kind == DocBlock::Bullet) indent = 16;
        double lineH = size * 1.4;

        // Tokenise into styled words.
        struct Word {
            std::string text;
            int font;
            bool newline;
            bool spaceBefore;
        };
        std::vector<Word> words;
        bool pendingSpace = false;
        for (const auto& r : ParseInline(b.text, b.markup)) {
            int font = r.bold ? 2 : (r.italic ? 3 : baseFont);
            std::wstring cur;
            auto push = [&](bool nl) {
                if (!cur.empty()) words.push_back({ToWinAnsi(cur), font, false, pendingSpace});
                cur.clear();
                pendingSpace = false;
                if (nl) words.push_back({"", font, true, false});
            };
            for (wchar_t c : r.text) {
                if (c == L'\n') push(true);
                else if (c == L' ' || c == L'\t') {
                    push(false);
                    pendingSpace = true;
                } else cur += c;
            }
            if (!cur.empty()) {  // word continues into the next styled run unless whitespace follows
                words.push_back({ToWinAnsi(cur), font, false, pendingSpace});
                pendingSpace = false;
            }
        }
        if (y - lineH < margin) newPage();
        if (b.kind == DocBlock::Bullet) {
            snprintf(buf, sizeof(buf), "BT /F1 %.1f Tf %.2f %.2f Td (\\225) Tj ET\n", size, margin + 4, y - size);
            content += buf;
        }
        double x = 0, space = AnsiWidth(" ", 1, size);
        auto lineBreak = [&] {
            x = 0;
            y -= lineH;
            if (y - lineH < margin) newPage();
        };
        for (const auto& w : words) {
            if (w.newline) {
                lineBreak();
                continue;
            }
            double ww = AnsiWidth(w.text, w.font, size);
            double gap = (x > 0 && w.spaceBefore) ? space : 0;
            if (x > 0 && x + gap + ww > usable - indent) {
                lineBreak();
                gap = 0;
            }
            x += gap;
            snprintf(buf, sizeof(buf), "BT /F%d %.1f Tf %.2f %.2f Td ", w.font, size, margin + indent + x, y - size);
            content += buf + PdfString(w.text) + " Tj ET\n";
            x += ww;
        }
        y -= lineH + size * (b.kind == DocBlock::Bullet ? 0.1 : 0.55);
    }
    if (!content.empty() || pdf.PageCount() == 0) newPage();
    pdf.Save(path);
}

}  // namespace

bool IsDocumentExt(const std::wstring& ext) {
    return ext == L"txt" || ext == L"md" || ext == L"markdown" || ext == L"html" || ext == L"htm" || ext == L"docx";
}

std::wstring ReadTextFile(const fs::path& path) {
    std::vector<uint8_t> d = ReadFileBytes(path);
    if (d.size() >= 2 && d[0] == 0xFF && d[1] == 0xFE)
        return std::wstring(reinterpret_cast<const wchar_t*>(d.data() + 2), (d.size() - 2) / 2);
    if (d.size() >= 2 && d[0] == 0xFE && d[1] == 0xFF) {
        std::wstring w;
        for (size_t i = 2; i + 1 < d.size(); i += 2) w += wchar_t((d[i] << 8) | d[i + 1]);
        return w;
    }
    size_t skip = (d.size() >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) ? 3 : 0;
    const char* p = reinterpret_cast<const char*>(d.data() + skip);
    int len = int(d.size() - skip);
    if (len == 0) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, len, nullptr, 0);
    UINT cp = n > 0 ? CP_UTF8 : CP_ACP;
    n = MultiByteToWideChar(cp, 0, p, len, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(cp, 0, p, len, w.data(), n);
    return w;
}

void WriteTextFile(const fs::path& path, const std::wstring& text) {
    WriteFileBytes(path, "\xEF\xBB\xBF" + WideToUtf8(text));
}

Doc ReadDocument(const fs::path& path) {
    std::wstring ext = ExtOf(path);
    if (ext == L"docx") return ParseDocx(path);
    if (ext == L"md" || ext == L"markdown") return ParseMarkdown(ReadTextFile(path));
    if (ext == L"html" || ext == L"htm") return ParseHtml(ReadTextFile(path));
    return ParseText(ReadTextFile(path));
}

void WriteDocument(const fs::path& path, const Doc& doc) {
    std::wstring ext = ExtOf(path);
    if (ext == L"docx") WriteDocx(path, doc);
    else if (ext == L"md") WriteTextFile(path, ToMarkdown(doc));
    else if (ext == L"html" || ext == L"htm") WriteTextFile(path, ToHtml(doc, BaseNameOf(path)));
    else if (ext == L"pdf") WriteTextPdf(path, doc);
    else WriteTextFile(path, ToTxt(doc));
}
