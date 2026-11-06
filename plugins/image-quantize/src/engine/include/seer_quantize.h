#ifndef SEER_QUANTIZE_H
#define SEER_QUANTIZE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SQ_ABI_VERSION              1u
#define SQ_MAX_PALETTE_ENTRIES      256u
#define SQ_NO_TRANSPARENT_INDEX     0xFFFFu
#define SQ_AUTO_THREADS             0u
#define SQ_MAX_DIMENSION            65535u      /* per side                        */
#define SQ_MAX_PIXELS               268435456u  /* 2^28 = 16384 x 16384 equivalent */

typedef enum sq_status {
    SQ_OK                        = 0,
    SQ_ERR_INVALID_ARG           = 1,
    SQ_ERR_UNSUPPORTED_VERSION   = 2,
    SQ_ERR_BUFFER_TOO_SMALL      = 3,
    SQ_ERR_CANCELLED             = 4,
    SQ_ERR_OUT_OF_MEMORY         = 5,
    SQ_ERR_INTERNAL              = 6
} sq_status;

typedef enum sq_alpha_mode {
    SQ_ALPHA_STRAIGHT            = 0,
    SQ_ALPHA_PREMULTIPLIED       = 1
} sq_alpha_mode;

typedef enum sq_color_space {
    SQ_SPACE_SRGB                = 0,
    SQ_SPACE_OKLAB               = 1
} sq_color_space;

typedef enum sq_dither_mode {
    SQ_DITHER_NONE               = 0,
    SQ_DITHER_FLOYD_STEINBERG    = 1
} sq_dither_mode;

/* Reported in sq_stats::path. */
typedef enum sq_path {
    SQ_PATH_UNSET                = 0,
    SQ_PATH_EXACT                = 1,
    SQ_PATH_DEGENERATE           = 2,
    SQ_PATH_GRID                 = 3
} sq_path;

/* Explicit pipeline override. AUTO derives the pipeline from color_space. */
typedef enum sq_pipeline {
    SQ_PIPELINE_AUTO             = 0,   /* sRGB -> Wu + sRGB refinement
                                           oklab -> Approach A                  */
    SQ_PIPELINE_SRGB             = 1,   /* Wu + k-means, both in sRGB          */
    SQ_PIPELINE_OKLAB_A          = 2,   /* Wu binning in sRGB, refinement and
                                           nearest search in Oklab             */
    SQ_PIPELINE_OKLAB_B          = 3    /* Wu-style splitting inside an Oklab
                                           grid                                 */
} sq_pipeline;

typedef struct sq_params {
    uint32_t struct_size;        /* set to sizeof(sq_params)                    */
    uint32_t version;            /* set to SQ_ABI_VERSION                       */
    uint32_t max_colors;         /* [2, 256]                                    */
    uint32_t alpha_mode;         /* sq_alpha_mode                               */
    uint32_t color_space;        /* sq_color_space                              */
    uint32_t dither_mode;        /* sq_dither_mode                              */
    float    dither_strength;    /* [0.0f, 1.0f]; ignored when dither is NONE   */
    uint32_t kmeans_max_iter;    /* [0, 1000]; 0 disables refinement            */
    float    kmeans_tolerance;   /* relative objective delta, default 1e-4f     */
    uint32_t thread_count;       /* 0 = auto; must not affect output            */
    uint32_t pipeline;           /* sq_pipeline, 0 = AUTO                       */
    uint32_t reserved0[3];       /* must be zero                                 */
} sq_params;

typedef struct sq_image_in {
    uint32_t       struct_size;  /* in: sizeof(sq_image_in)                      */
    uint32_t       version;      /* in: SQ_ABI_VERSION                           */
    const uint8_t *pixels;       /* RGBA8888, top-down, tightly described by stride */
    uint32_t       width;        /* [1, SQ_MAX_DIMENSION]                        */
    uint32_t       height;       /* [1, SQ_MAX_DIMENSION]                        */
    size_t         stride;       /* bytes per row, >= width * 4                  */
    uint32_t       reserved0[2]; /* must be zero                                 */
} sq_image_in;

typedef struct sq_result {
    uint32_t struct_size;        /* in: sizeof(sq_result)                        */
    uint32_t version;            /* in: SQ_ABI_VERSION                           */
    uint8_t  palette_rgba[SQ_MAX_PALETTE_ENTRIES * 4]; /* out: straight alpha  */
    uint16_t palette_size;       /* out: [1, max_colors]                         */
    uint16_t transparent_index;  /* out: SQ_NO_TRANSPARENT_INDEX when unused     */
    uint8_t *indices;            /* in: caller buffer, width * height            */
    size_t   indices_capacity;   /* in: must be >= width * height                */
} sq_result;

typedef struct sq_stats {
    uint32_t struct_size;
    uint32_t version;
    uint32_t path;               /* sq_path: 1 exact, 2 degenerate, 3 grid       */
    uint32_t probe_state;        /* 0 complete, 1 overflow (5.3)                 */
    uint32_t unique_colors;      /* distinct RGBA found, saturated at 4097/32769 */
    uint32_t occupied_cells;     /* non-empty 5-bit cells                        */
    uint32_t palette_size;
    uint32_t kmeans_iterations;
    uint32_t thread_count_used;
    double   stage_ms[6];        /* scan, split, refine, palette, map_or_dither, total */
    size_t   workspace_bytes;    /* peak library-owned allocation                */
} sq_stats;

/* progress: return non-zero to cancel. Called from the calling thread only. */
typedef int (*sq_progress_fn)(uint32_t percent, void *user);

uint32_t    sq_abi_version(void);
sq_status   sq_params_init(sq_params *params);
sq_status   sq_params_validate(const sq_params *params);
size_t      sq_workspace_bytes(const sq_params *params, uint32_t width, uint32_t height);
const char *sq_status_message(sq_status status);

sq_status   sq_quantize(const sq_image_in *input,
                        const sq_params   *params,
                        sq_progress_fn     progress,
                        void              *progress_user,
                        sq_result         *result,
                        sq_stats          *stats /* optional, may be NULL */);

#ifdef __cplusplus
}
#endif

#endif /* SEER_QUANTIZE_H */
