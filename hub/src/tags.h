// Cause tags: the operator marks a time window as likely explained (HVAC cycling, a phone near a node, ...).
// Tags annotate; they never suppress the score or the LED. Arduino-free, host-tested.
#pragma once
#include <stdint.h>

#define TAG_MAX 6
#define TAG_MAX_DUR_S (4 * 3600)
typedef struct { bool active; char kind[12]; char note[24]; uint32_t start_ms, dur_ms; } Tag;
typedef struct { Tag t[TAG_MAX]; } TagSet;

void tags_init(TagSet *s);
bool tag_kind_valid(const char *kind);                    // hvac phone appliance person pet other note
// dur_s == 0 is an instant note (not kept active). A second tag of the same kind replaces the first. False if the kind
// is invalid or the set is full.
bool tags_add(TagSet *s, const char *kind, const char *note, uint32_t dur_s, uint32_t now_ms);
int tags_active(TagSet *s, uint32_t now_ms, Tag *out, uint32_t *left_s, int max);   // expires finished tags
bool tags_clear(TagSet *s, const char *kind);
