#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPORT "/run/capture/report"

struct target {
    int id;
    char name[128];
};

struct frame {
    int id;
    char tag[64];
    double latency;
    int colour;
};

struct line {
    char text[8192];
};

static struct target *targets;
static int target_count;
static struct frame *frames;
static int frame_count;
static struct line *ended;
static int ended_count;

static void grow(void **array, int *capacity, int need, size_t size) {
    if (need < *capacity) return;
    while (*capacity <= need) *capacity = *capacity ? *capacity * 2 : 64;
    void *grown = realloc(*array, (size_t)*capacity * size);
    if (!grown) exit(1);
    *array = grown;
}

static char **split(char *text, int *count) {
    static char *parts[16];
    int used = 0;
    char *at = text;
    for (;;) {
        while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') at++;
        if (!*at) break;
        if (used == 16) break;
        parts[used++] = at;
        while (*at && *at != ' ' && *at != '\t' && *at != '\n' && *at != '\r') at++;
        if (*at) *at++ = '\0';
    }
    *count = used;
    return parts;
}

int main(void) {
    FILE *file = fopen(REPORT, "r");
    if (!file) {
        fprintf(stderr, "capture-check: cannot open %s\n", REPORT);
        return 1;
    }
    int target_capacity = 0, frame_capacity = 0, ended_capacity = 0;
    char line[8192];
    int lines = 0;
    char head[8192], tail[8192];
    head[0] = tail[0] = '\0';
    while (fgets(line, sizeof line, file)) {
        size_t length = strlen(line);
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = '\0';
        if (lines == 0) snprintf(head, sizeof head, "%s", line);
        snprintf(tail, sizeof tail, "%s", line);
        lines++;
        char copy[8192];
        snprintf(copy, sizeof copy, "%s", line);
        int count = 0;
        char **parts = split(copy, &count);
        if (count == 0) continue;
        if (strcmp(parts[0], "target") == 0 && count >= 5) {
            int id = (int)strtol(parts[1], NULL, 10);
            grow((void **)&targets, &target_capacity, target_count, sizeof targets[0]);
            targets[target_count].id = id;
            snprintf(targets[target_count].name, sizeof targets[target_count].name, "%s", parts[4]);
            target_count++;
        } else if (strcmp(parts[0], "frame") == 0 && count >= 6) {
            grow((void **)&frames, &frame_capacity, frame_count, sizeof frames[0]);
            frames[frame_count].id = (int)strtol(parts[1], NULL, 10);
            snprintf(frames[frame_count].tag, sizeof frames[frame_count].tag, "%s", parts[3]);
            frames[frame_count].latency = strtod(parts[4], NULL);
            frames[frame_count].colour = (int)(strtol(parts[5], NULL, 16) & 0xFFFFFF);
            frame_count++;
        } else if (strcmp(parts[0], "end") == 0) {
            for (int index = 1; index < count; index++) {
                grow((void **)&ended, &ended_capacity, ended_count, sizeof ended[0]);
                snprintf(ended[ended_count].text, sizeof ended[ended_count].text, "%s",
                         parts[index]);
                ended_count++;
            }
        }
    }
    fclose(file);
    int ready = 0, failed = 0, wrong = 0;
    for (int index = 0; index < frame_count; index++)
        if (strcmp(frames[index].tag, "ready") == 0) ready++;
        else if (strcmp(frames[index].tag, "failed") == 0) failed++;
    for (int index = 0; index < frame_count; index++) {
        if (strcmp(frames[index].tag, "ready") != 0) continue;
        char wanted[64];
        snprintf(wanted, sizeof wanted, "c%06x", frames[index].colour);
        const char *seen = NULL;
        for (int other = target_count - 1; other >= 0; other--)
            if (targets[other].id == frames[index].id) {
                seen = targets[other].name;
                break;
            }
        if (!seen || strcmp(seen, wanted) != 0) wrong++;
    }
    printf("ready %d\n", ready);
    printf("failed %d\n", failed);
    printf("wrong %d\n", wrong);
    struct target unique[64];
    int unique_count = 0;
    for (int index = 0; index < target_count; index++) {
        int known = -1;
        for (int other = 0; other < unique_count; other++)
            if (unique[other].id == targets[index].id) known = other;
        if (known < 0) {
            known = unique_count++;
            memset(&unique[known], 0, sizeof unique[known]);
            unique[known].id = targets[index].id;
        }
        snprintf(unique[known].name, sizeof unique[known].name, "%s", targets[index].name);
    }
    for (int left = 0; left < unique_count; left++)
        for (int right = left + 1; right < unique_count; right++)
            if (unique[right].id < unique[left].id) {
                struct target swap = unique[left];
                unique[left] = unique[right];
                unique[right] = swap;
            }
    printf("targets");
    for (int index = 0; index < unique_count; index++)
        printf(" %d:%s", unique[index].id, unique[index].name);
    printf("\n");
    printf("first");
    for (int index = 0; index < frame_count; index++) {
        int position = 0;
        for (int other = 0; other < index; other++)
            if (strcmp(frames[other].tag, "ready") == 0) position++;
        if (strcmp(frames[index].tag, "ready") != 0 || position >= 4) continue;
        printf(" %d:%s:%06x", frames[index].id, frames[index].tag, frames[index].colour);
    }
    printf("\n");
    double latency = 0.0;
    for (int index = 0; index < frame_count; index++)
        if (strcmp(frames[index].tag, "ready") == 0) {
            latency = frames[index].latency;
            break;
        }
    printf("ready-latency %.3f\n", latency);
    printf("end");
    for (int index = 0; index < ended_count; index++) printf(" %s", ended[index].text);
    printf("\n");
    printf("lines %d\n", lines);
    printf("head %s\n", head);
    printf("tail %s\n", tail);
    return 0;
}
