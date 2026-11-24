// The problem report's texts (report_text.hpp)
#include "report_text.hpp"

#include <algorithm>
#include <cctype>

namespace report {
namespace {

bool word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool same_letters(std::string_view text, std::size_t at, std::string_view word) {
    if (at + word.size() > text.size())
        return false;
    for (std::size_t k = 0; k < word.size(); k++) {
        if (std::tolower(static_cast<unsigned char>(text[at + k])) != std::tolower(static_cast<unsigned char>(word[k])))
            return false;
    }
    return true;
}

// The redaction that starts at `at`, if any (the longest first)
const Redaction *match_at(std::string_view text, std::size_t at, const std::vector<const Redaction *> &words) {
    for (const Redaction *r : words) {
        if (!same_letters(text, at, r->word))
            continue;
        const std::size_t end = at + r->word.size();
        if (r->whole_word && ((at > 0 && word_char(text[at - 1])) || (end < text.size() && word_char(text[end]))))
            continue;
        return r;
    }
    return nullptr;
}

bool mail_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-' || c == '+';
}

// An e-mail address around the '@' at `at`: its start and end, or false
bool mail_around(std::string_view text, std::size_t at, std::size_t *start, std::size_t *end) {
    std::size_t s = at, e = at + 1;
    while (s > 0 && mail_char(text[s - 1]))
        s--;
    bool dot = false;
    while (e < text.size() && mail_char(text[e])) {
        dot = dot || text[e] == '.';
        e++;
    }
    while (e > at + 1 && text[e - 1] == '.') // (a sentence's full stop)
        e--;
    if (s == at || e == at + 1 || !dot || text[e - 1] == '.')
        return false;
    *start = s;
    *end = e;
    return true;
}

std::string without_mail(std::string text) {
    std::string out;
    out.reserve(text.size());
    std::size_t copied = 0;
    for (std::size_t at = text.find('@'); at != std::string::npos; at = text.find('@', at + 1)) {
        std::size_t start = 0, end = 0;
        if (at < copied || !mail_around(text, at, &start, &end) || start < copied)
            continue;
        out.append(text, copied, start - copied);
        out += "<e-mail>";
        copied = end;
        at = end - 1;
    }
    out.append(text, copied, std::string::npos);
    return out;
}

} // namespace

std::string redact(std::string_view text, const std::vector<Redaction> &words) {
    std::vector<const Redaction *> order;
    for (const Redaction &r : words) {
        if (!r.word.empty())
            order.push_back(&r);
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const Redaction *a, const Redaction *b) { return a->word.size() > b->word.size(); });
    std::string out;
    out.reserve(text.size());
    for (std::size_t at = 0; at < text.size();) {
        if (const Redaction *r = match_at(text, at, order)) {
            out += r->stand_in;
            at += r->word.size();
        } else {
            out += text[at++];
        }
    }
    return without_mail(std::move(out));
}

std::string url_encode(std::string_view text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3 / 2);
    for (char c : text) {
        const auto b = static_cast<unsigned char>(c);
        if (std::isalnum(b) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += hex[b >> 4];
            out += hex[b & 15];
        }
    }
    return out;
}

std::string file_link(std::string_view generic_path) {
    std::string link = !generic_path.empty() && generic_path.front() == '/' ? "file://" : "file:///";
    for (char c : generic_path) {
        const auto b = static_cast<unsigned char>(c);
        if (c == '/' || c == ':' || b >= 0x80)
            link += c;
        else
            link += url_encode(std::string_view(&c, 1));
    }
    return link;
}

namespace {

// The parts of the link that can be cut, as they are now
struct LinkParts {
    std::string what, where, details, body;
};

std::string form_url(const IssueForm &form, const LinkParts &parts) {
    std::string url = "https://github.com/";
    url += form.repository;
    url += "/issues/new?template=report.yml";
    const std::pair<const char *, std::string_view> fields[] = {
        {"title", form.title}, {"what", parts.what}, {"where", parts.where}, {"details", parts.details},
        {"body", parts.body}};
    for (const auto &[id, text] : fields) {
        if (!text.empty())
            url += std::string("&") + id + "=" + url_encode(text);
    }
    return url;
}

// A plain issue's text, for when the form is not there (GitHub reads forms from the default branch only): the
// boxes' texts in one. The form ignores it.
std::string plain_body(const IssueForm &form) {
    std::string body;
    if (!form.what.empty())
        body += std::string(form.what) + "\n\n";
    if (!form.where.empty())
        body += "Where: " + std::string(form.where) + "\n\n";
    body += "Please drag the report file (.zip) and a screenshot of the problem in here.\n";
    if (!form.details.empty())
        body += "\n```\n" + std::string(form.details) + "\n```\n";
    return body;
}

// The text cut to its first `bytes` at a line's end, with a note
std::string cut(std::string_view text, std::size_t bytes) {
    static constexpr std::string_view note = "\n(cut short: the rest is in the report's zip)";
    if (bytes >= text.size())
        return std::string(text);
    std::size_t end = text.rfind('\n', bytes);
    if (end == std::string_view::npos)
        return std::string(note.substr(1));
    return std::string(text.substr(0, end)) + std::string(note);
}

} // namespace

namespace {

// Cuts `text` (one of parts') until the link fits or it is gone, 64 bytes at a time at a line's end
void cut_to_fit(const IssueForm &form, LinkParts &parts, std::string &text, std::string_view whole,
                std::size_t max_bytes, std::string &url) {
    for (std::size_t keep = whole.size(); url.size() > max_bytes && keep > 0;) {
        keep = keep > 64 ? keep - 64 : 0;
        text = keep > 0 ? cut(whole, keep) : std::string();
        url = form_url(form, parts);
    }
}

} // namespace

std::string issue_url(const IssueForm &form, std::size_t max_bytes) {
    const std::string body = plain_body(form);
    LinkParts parts{std::string(form.what), std::string(form.where), std::string(form.details), body};
    std::string url = form_url(form, parts);
    // the plain issue's text first (the form, once on the default branch, ignores it), then Details, then Where and
    // What happened
    cut_to_fit(form, parts, parts.body, body, max_bytes, url);
    cut_to_fit(form, parts, parts.details, form.details, max_bytes, url);
    cut_to_fit(form, parts, parts.where, form.where, max_bytes, url);
    cut_to_fit(form, parts, parts.what, form.what, max_bytes, url);
    return url;
}

std::string head_and_tail(std::string_view text, std::size_t head_lines, std::size_t tail_bytes) {
    std::size_t head_end = 0;
    for (std::size_t line = 0; line < head_lines && head_end < text.size(); line++) {
        std::size_t nl = text.find('\n', head_end);
        head_end = nl == std::string_view::npos ? text.size() : nl + 1;
    }
    if (head_end + tail_bytes >= text.size())
        return std::string(text);
    std::size_t tail_start = text.size() - tail_bytes;
    std::size_t nl = text.find('\n', tail_start);
    if (nl != std::string_view::npos && nl + 1 < text.size())
        tail_start = nl + 1;
    std::string out(text.substr(0, head_end));
    out += "\n... (" + std::to_string(tail_start - head_end) + " bytes left out) ...\n\n";
    out += text.substr(tail_start);
    return out;
}

} // namespace report
