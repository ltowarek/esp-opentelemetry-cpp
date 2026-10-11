#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cJSON.h"

#define NODES 16

void app_main(void)
{
    printf("alignof(max_align_t) = %zu\n", alignof(max_align_t));
    printf("alignof(cJSON)       = %zu\n", alignof(cJSON));
    printf("offsetof(cJSON, valuedouble) = %zu\n\n", offsetof(cJSON, valuedouble));

    // Every node stays alive so each parse gets a fresh heap block; freeing
    // in the loop would hand back the same block and hide the misalignment.
    // Messages differ only in the length of one string, as real payloads do;
    // that is enough to shift the following node by 4 bytes.
    static const char *const messages[] = {
        "{\"cmd\":\"go\",\"speed\":1.5}",
        "{\"cmd\":\"stop\",\"speed\":1.5}",
        "{\"cmd\":\"left\",\"speed\":1.5}",
        "{\"cmd\":\"right\",\"speed\":1.5}",
    };
    static cJSON *nodes[NODES];
    int misaligned = 0;
    for (int i = 0; i < NODES; ++i) {
        nodes[i] = cJSON_Parse(messages[i % 4]);
        const cJSON *speed = cJSON_GetObjectItem(nodes[i], "speed");
        uintptr_t low = (uintptr_t)speed & (alignof(cJSON) - 1);
        printf("  [%2d] node %p  low3=%u  &valuedouble=%p  speed=%f%s\n", i, (void *)speed,
               (unsigned)low, (void *)&speed->valuedouble, speed->valuedouble,
               low ? "  <-- MISALIGNED" : "");
        if (low) {
            ++misaligned;
        }
    }
    printf("Misaligned cJSON nodes: %d / %d\n", misaligned, NODES);

    for (int i = 0; i < NODES; ++i) {
        cJSON_Delete(nodes[i]);
    }
}
