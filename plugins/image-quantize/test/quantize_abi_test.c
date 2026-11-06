/* Plain C99 translation unit: proves the engine header leaks no C++ type and
   that every exported symbol links from C. */

#include "seer_quantize.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(expr)                                                        \
    do {                                                                   \
        ++checks;                                                          \
        if (!(expr)) {                                                     \
            ++failures;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        }                                                                  \
    } while (0)

int main(void) {
    sq_params params;
    sq_image_in image;
    sq_result result;
    sq_stats stats;
    uint8_t pixels[4] = {10u, 20u, 30u, 255u};
    uint8_t indices[1] = {0u};

    CHECK(sq_abi_version() == SQ_ABI_VERSION);
    CHECK(sq_params_init(&params) == SQ_OK);
    CHECK(params.struct_size == sizeof(sq_params));
    CHECK(params.version == SQ_ABI_VERSION);
    CHECK(params.max_colors == 256u);
    CHECK(params.kmeans_max_iter == 20u);
    CHECK(params.thread_count == SQ_AUTO_THREADS);
    CHECK(sq_params_validate(&params) == SQ_OK);
    CHECK(sq_workspace_bytes(&params, 1u, 1u) > 0u);
    CHECK(strcmp(sq_status_message(SQ_OK), "ok") == 0);

    memset(&image, 0, sizeof(image));
    image.struct_size = sizeof(sq_image_in);
    image.version = SQ_ABI_VERSION;
    image.pixels = pixels;
    image.width = 1u;
    image.height = 1u;
    image.stride = 4u;

    memset(&result, 0, sizeof(result));
    result.struct_size = sizeof(sq_result);
    result.version = SQ_ABI_VERSION;
    result.indices = indices;
    result.indices_capacity = 1u;

    memset(&stats, 0, sizeof(stats));
    stats.struct_size = sizeof(sq_stats);
    stats.version = SQ_ABI_VERSION;

    CHECK(sq_quantize(&image, &params, NULL, NULL, &result, &stats) == SQ_OK);
    CHECK(result.palette_size == 1u);
    CHECK(result.indices[0] < result.palette_size);
    CHECK(result.transparent_index == SQ_NO_TRANSPARENT_INDEX);

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
