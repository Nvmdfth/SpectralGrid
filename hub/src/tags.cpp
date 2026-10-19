#include <string.h>
#include "tags.h"

static const char *KINDS[] = {"hvac", "phone", "appliance", "person", "pet", "other", "note"};
void tags_init(TagSet *s) { memset(s, 0, sizeof(*s)); }
bool tag_kind_valid(const char *k) { if (!k) return false; for (auto *x : KINDS) if (!strcmp(k, x)) return true; return false; }

bool tags_add(TagSet *s, const char *kind, const char *note, uint32_t dur_s, uint32_t now_ms) {
    if (!tag_kind_valid(kind)) return false;
    if (dur_s == 0) return true;                          // instant note: the caller logs it, nothing stays active
    if (dur_s > TAG_MAX_DUR_S) dur_s = TAG_MAX_DUR_S;
    Tag *slot = nullptr;
    for (auto &t : s->t) if (t.active && !strcmp(t.kind, kind)) { slot = &t; break; }
    if (!slot) for (auto &t : s->t) if (!t.active) { slot = &t; break; }
    if (!slot) return false;
    memset(slot, 0, sizeof(*slot));
    slot->active = true; strncpy(slot->kind, kind, sizeof(slot->kind) - 1);
    if (note) strncpy(slot->note, note, sizeof(slot->note) - 1);
    slot->start_ms = now_ms; slot->dur_ms = dur_s * 1000u;
    return true;
}

int tags_active(TagSet *s, uint32_t now_ms, Tag *out, uint32_t *left_s, int max) {
    int n = 0;
    for (auto &t : s->t) {
        if (!t.active) continue;
        uint32_t el = now_ms - t.start_ms;
        if (el >= t.dur_ms) { t.active = false; continue; }
        if (n < max) { out[n] = t; if (left_s) left_s[n] = (t.dur_ms - el + 999) / 1000; n++; }
    }
    return n;
}

bool tags_clear(TagSet *s, const char *kind) {
    for (auto &t : s->t) if (t.active && !strcmp(t.kind, kind)) { t.active = false; return true; }
    return false;
}
