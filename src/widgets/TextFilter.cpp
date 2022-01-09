#include "TextFilter.h"

// A slow version of ImStristr with wildcard matching using dynamic programming
const char * ImStrWildcard(const char *haystack, const char *haystack_end, const char *needle, const char *needle_end) {
    if (!needle_end)
        needle_end = needle + strlen(needle);
    if (!haystack_end)
        haystack_end = haystack + strlen(haystack);

    const size_t haystack_size = std::distance(haystack, haystack_end);
    const size_t needle_size = std::distance(needle, needle_end);

    if (!needle_size)
        return nullptr;
    if (!haystack_size)
        return nullptr;

    // OPTIM: We can use only one line instead of the full table
    bool dpbuf[(haystack_size + 1)][(needle_size + 1)];

    // Init empty character
    dpbuf[0][0] = true;
    for (int j = 1; j <= haystack_size; ++j) {
        dpbuf[j][0] = false;
    }
    for (int i = 1; i <= needle_size; ++i) {
        if (needle[i - 1] == '*')
            dpbuf[0][i] = dpbuf[0][i - 1];
        else
            dpbuf[0][i] = false;
    }

    // Fill dp table
    for (int j = 1; j <= haystack_size; ++j) {
        for (int i = 1; i <= needle_size; ++i) {
            if (haystack[j - 1] == needle[i - 1] || needle[i - 1] == '?') {
                dpbuf[j][i] = dpbuf[j - 1][i - 1];
            } else if (needle[i - 1] == '*') {
                dpbuf[j][i] = dpbuf[j - 1][i] || dpbuf[j][i - 1];
            } else {
                dpbuf[j][i] = false;
            }
        }
    }

    return dpbuf[haystack_size][needle_size] ? haystack : nullptr;
}

TextFilter::TextFilter(const char *default_filter) {
    if (default_filter) {
        ImStrncpy(InputBuf, default_filter, IM_ARRAYSIZE(InputBuf));
        Build();
    } else {
        InputBuf[0] = 0;
        CountGrep = 0;
    }
}

bool TextFilter::Draw(const char *label, float width) {
    if (width != 0.0f)
        ImGui::SetNextItemWidth(width);
    bool value_changed = ImGui::InputTextWithHint(label, "Search", InputBuf, IM_ARRAYSIZE(InputBuf));
    ImGui::SameLine();
    if (ImGui::Checkbox("Use wildcard", &UseWildcard) || value_changed)
        Build();
    return value_changed;
}

void TextFilter::TextRange::split(char separator, ImVector<TextRange> *out) const {
    out->resize(0);
    const char *wb = b;
    const char *we = wb;
    while (we < e) {
        if (*we == separator) {
            out->push_back(TextRange(wb, we));
            wb = we + 1;
        }
        we++;
    }
    if (wb != we)
        out->push_back(TextRange(wb, we));
}

void TextFilter::Build() {
    
    PatternMatchFunc = UseWildcard ? ImStrWildcard : ImStristr;
    
    Filters.resize(0);
    TextRange input_range(InputBuf, InputBuf + strlen(InputBuf));
    input_range.split(',', &Filters);

    CountGrep = 0;
    for (int i = 0; i != Filters.Size; i++) {
        TextRange &f = Filters[i];
        while (f.b < f.e && ImCharIsBlankA(f.b[0]))
            f.b++;
        while (f.e > f.b && ImCharIsBlankA(f.e[-1]))
            f.e--;
        if (f.empty())
            continue;
        if (Filters[i].b[0] != '-')
            CountGrep += 1;
    }
}

bool TextFilter::PassFilter(const char *text, const char *text_end) const {
    if (Filters.empty())
        return true;

    if (text == NULL)
        text = "";

    for (int i = 0; i != Filters.Size; i++) {
        const TextRange &f = Filters[i];
        if (f.empty())
            continue;
        if (f.b[0] == '-') {
            // Subtract
            if (PatternMatchFunc(text, text_end, f.b + 1, f.e) != NULL)
                return false;
        } else {
            // Grep
            if (PatternMatchFunc(text, text_end, f.b, f.e) != NULL)
                return true;
        }
    }

    // Implicit * grep
    if (CountGrep == 0)
        return true;

    return false;
}
