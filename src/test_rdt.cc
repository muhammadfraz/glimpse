/*
 * Copyright (C) 2018 Glimp IP Ltd
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Misc notes on how we want to define our error metrics...
 *
 * (XXX: actually we don't currently implement the metrics based on these
 * thoughts but it seemed worth keeping these notes for now)
 *
 * It's important for us to define error metrics that are calculated on a
 * per-image basis, considering our tracking works on a per-image basis.
 *
 * For prectical purposes we need to be able to aggregate the error metrics
 * across a large image set (e.g. > 10k images) (i.e. metrics must be
 * transformed into some image independent units for them to be comparable
 * between images).
 *
 * Varied body sizes e.g. due to varying distances from the camera shouldn't
 * bias the metrics.
 *
 * For deeper insights we want to split out metrics on a per-label basis.
 *
 * It's not critical that we can directly compare the metrics of different
 * labels but it would be nice if there was some intuitive connection.
 *
 * When we consider mis-labelling errors we want to inversly scale the
 * significance of mis-labelling by the size of the part being labelled.  So a
 * small number of mislabelled pixels for the wrist represent a more
 * significant error than a small number of incorrect labels for the hip.
 *
 * It's not obvious what the best way of handling varying occlusion would be.
 * If only a very small part of a hip is currently visible does that now mean
 * that a small number of mis-labelled pixels should be treated like a
 * significant error (similar to the example of mis-labelling a wrist above).
 * It sounds reasonable to only consider the global label-size factors so the
 * significance of errors doesn't depend on the number of labelled pixels seen
 * in the current image, it only depends on the relative size of that label as
 * seen across all images.
 *
 * By measuring the relative sizes of labels within the current image and
 * compare these with our global label size factors we could derive a per-image
 * label occlusion value so we could potentially improve how metrics are
 * normalized.
 *
 * To account for scale differences we could go as far as projecting the area
 * of each pixel to a fixed distance, considering the camera intrinsics used to
 * render the test data and the depth buffers we have.
 */


#include <stdio.h>
#include <getopt.h>

#include <vector>

#include "rdt_tree.h"
#include "infer_labels.h"
#include "image_utils.h"

#include <glimpse_rdt.h>
#include <glimpse_data.h>

#define xsnprintf(dest, size, fmt, ...) do { \
        if (snprintf(dest, size, fmt,  __VA_ARGS__) >= (int)size) \
            exit(1); \
    } while(0)

struct data_loader
{
    struct gm_logger *log;
    uint64_t last_update;
    int width;
    int height;

    std::vector<float> depth_images;
    float bg_depth;

    std::vector<uint8_t> label_images;

    std::vector<char *> frame_paths;
};

static bool threaded_opt = false;
static bool verbose_opt = false;

static int rows_per_label_opt = 2;

/* Label maps... */
static uint8_t rdt_to_test_map[256];
static uint8_t test_to_out_map[256];

static const char *index_output_opt = NULL;
static float index_low_acc_opt = 0.f;

static const char *hbars[] = {
    " ",
    "▏",
    "▎",
    "▍",
    "▌",
    "▋",
    "▊",
    "▉",
    "█"
};

static const char *vbars[] = {
    " ",
    "▁",
    "▂",
    "▃",
    "▄",
    "▅",
    "▆",
    "▇",
    "█"
};


static uint64_t
get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return ((uint64_t)ts.tv_sec) * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static float
get_format_duration(uint64_t duration_ns)
{
    if (duration_ns > 1000000000)
        return duration_ns / 1e9;
    else if (duration_ns > 1000000)
        return duration_ns / 1e6;
    else if (duration_ns > 1000)
        return duration_ns / 1e3;
    else
        return duration_ns;
}

static char *
get_format_duration_suffix(uint64_t duration_ns)
{
    if (duration_ns > 1000000000)
        return (char *)"s";
    else if (duration_ns > 1000000)
        return (char *)"ms";
    else if (duration_ns > 1000)
        return (char *)"us";
    else
        return (char *)"ns";
}

static void
logger_cb(struct gm_logger *logger,
          enum gm_log_level level,
          const char *context,
          struct gm_backtrace *backtrace,
          const char *format,
          va_list ap,
          void *user_data)
{
    FILE *log_fp = stderr;
    char *msg = NULL;

    if (verbose_opt == false && level < GM_LOG_ERROR)
        return;

    if (vasprintf(&msg, format, ap) > 0) {
        switch (level) {
        case GM_LOG_ERROR:
            fprintf(log_fp, "%s: ERROR: ", context);
            break;
        case GM_LOG_WARN:
            fprintf(log_fp, "%s: WARN: ", context);
            break;
        default:
            fprintf(log_fp, "%s: ", context);
        }

        fprintf(log_fp, "%s\n", msg);

        if (backtrace) {
            int line_len = 100;
            char *formatted = (char *)alloca(backtrace->n_frames * line_len);

            gm_logger_get_backtrace_strings(logger, backtrace,
                                            line_len, (char *)formatted);
            for (int i = 0; i < backtrace->n_frames; i++) {
                char *line = formatted + line_len * i;
                fprintf(log_fp, "> %s\n", line);
            }
        }

        fflush(log_fp);
        free(msg);
    }
}

static void
logger_abort_cb(struct gm_logger *logger, void *user_data)
{
    FILE *log_fp = stderr;

    fprintf(log_fp, "ABORT\n");
    fflush(log_fp);

    abort();
}

static void
print_label_histogram(struct gm_logger* log,
                      JSON_Array* labels,
                      float* histogram)
{
    int max_bar_width = 30; // measured in terminal columns

    for (int i = 0; i < (int)json_array_get_count(labels); i++) {
        JSON_Object* label = json_array_get_object(labels, i);
        const char *name = json_object_get_string(label, "name");
        int bar_len = max_bar_width * 8 * histogram[i];
        char bar[max_bar_width * 4]; // room for multi-byte utf8 characters
        int bar_pos = 0;

        for (int j = 0; j < max_bar_width; j++) {
            int part;
            if (bar_len > 8) {
                part = 8;
                bar_len -= 8;
            } else {
                part = bar_len;
                bar_len = 0;
            }
            int len = strlen(hbars[part]);
            memcpy(bar + bar_pos, hbars[part], len);
            bar_pos += len;
        }
        bar[bar_pos++] = '\0';
        printf("%2d) %-20s, %-3.0f%%|%s|\n", i, name, histogram[i] * 100.0f, bar);
    }
}

static bool
load_test_data_cb(struct gm_data_index *data_index,
                  int index,
                  const char *frame_path,
                  void *user_data,
                  char **err)
{
    struct data_loader *loader = (struct data_loader *)user_data;
    struct gm_logger *log = loader->log;
    int width = loader->width;
    int height = loader->height;

    const char *top_dir = gm_data_index_get_top_dir(data_index);

    char depth_filename[512];
    xsnprintf(depth_filename, sizeof(depth_filename), "%s/depth/%s.exr",
              top_dir, frame_path);

    int64_t depth_off = (int64_t)index * width * height;
    float *depth_image = &loader->depth_images[depth_off];
    IUImageSpec depth_spec = { width, height, IU_FORMAT_FLOAT };
    if (iu_read_exr_from_file(depth_filename, &depth_spec,
                              (void **)&depth_image) != SUCCESS)
    {
        gm_throw(log, err, "Failed to read image '%s'\n", depth_filename);
        return false;
    }

    /* The decision tree may require a different depth value be used for the
     * background...
     */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int off = width * y + x;

            float depth_m = depth_image[off];
            if (depth_m >= HUGE_DEPTH || std::isnan(depth_m)) {
                depth_image[off] = loader->bg_depth;
            }
        }
    }

    char labels_filename[512];
    xsnprintf(labels_filename, sizeof(labels_filename), "%s/labels/%s.png",
              top_dir, frame_path);

    int64_t labels_off = (int64_t)index * width * height;
    uint8_t *labels_image = &loader->label_images[labels_off];
    IUImageSpec label_spec = { width, height, IU_FORMAT_U8 };
    if (iu_read_png_from_file(labels_filename, &label_spec,
                              &labels_image,
                              NULL, // palette output
                              NULL) // palette size
        != SUCCESS)
    {
        gm_throw(log, err, "Failed to read image '%s'\n", labels_filename);
        return false;
    }

    if (index_output_opt) {
        loader->frame_paths[index] = strdup(frame_path);
    }

    return true;
}

static bool
check_consistent_tree_labels(JSON_Value *expected_labels,
                             JSON_Value *labels)
{
    JSON_Array *exp_labels_array = json_array(expected_labels);
    JSON_Array *labels_array = json_array(labels);
    int n_labels_exp = json_array_get_count(exp_labels_array);
    int n_labels = json_array_get_count(labels_array);
    if (n_labels != n_labels_exp)
        return false;
    for (int i = 0; i < n_labels; i++) {
        JSON_Object* exp_label = json_array_get_object(exp_labels_array, i);
        JSON_Object* label = json_array_get_object(labels_array, i);
        if (strcmp(json_object_get_string(exp_label, "name"),
                   json_object_get_string(label, "name")) != 0)
        {
            return false;
        }
    }

    return true;
}

/*
 * Use this inference implementation with legacy decision trees that
 * were trained using floor() rounding when normalizing uv offsets
 * and measured gradients in floating point.
 *
 * Note: don't call for background pixels
 */
static void
infer_pixel_label_probs(struct gm_logger *log,
                        RDTree **trees,
                        int n_trees,
                        float *depth_image,
                        int width,
                        int height,
                        int x,
                        int y,
                        float *pr_table_out)
{
    int n_rdt_labels = trees[0]->header.n_labels;
    int off = y * width + x;
    float depth = depth_image[off];

    float bg_depth = trees[0]->header.bg_depth;

    memset(pr_table_out, 0, sizeof(float) * n_rdt_labels);

    for (int i = 0; i < n_trees; i++) {
        RDTree *tree = trees[i];

        int id = 0;
        Node node = tree->nodes[0];
        while (node.label_pr_idx == 0)
        {
            int32_t u[2] = { (int32_t)(x + node.uv[0] / depth),
                             (int32_t)(y + node.uv[1] / depth) };
            int32_t v[2] = { (int32_t)(x + node.uv[2] / depth),
                             (int32_t)(y + node.uv[3] / depth) };

            float upixel = (u[0] >= 0 && u[0] < (int32_t)width &&
                            u[1] >= 0 && u[1] < (int32_t)height) ?
                (float)depth_image[((u[1] * width) + u[0])] : bg_depth;
            float vpixel = (v[0] >= 0 && v[0] < (int32_t)width &&
                            v[1] >= 0 && v[1] < (int32_t)height) ?
                (float)depth_image[((v[1] * width) + v[0])] : bg_depth;

            float gradient = upixel - vpixel;

            /* NB: The nodes are arranged in breadth-first, left then
             * right child order with the root node at index zero.
             *
             * In this case if you have an index for any particular node
             * ('id' here) then 2 * id + 1 is the index for the left
             * child and 2 * id + 2 is the index for the right child...
             */
            id = (gradient < node.t) ? 2 * id + 1 : 2 * id + 2;

            node = tree->nodes[id];
        }

        /* NB: node->label_pr_idx is a base-one index since index zero
         * is reserved to indicate that the node is not a leaf node
         */
        float *pr_table =
            &tree->label_pr_tables[(node.label_pr_idx - 1) * n_rdt_labels];
        for (int n = 0; n < n_rdt_labels; n++)
            pr_table_out[n] += pr_table[n];
    }

    for (int i = 0; i < n_rdt_labels; i++)
        pr_table_out[i] /= (float)n_trees;
}

static void
usage(void)
{
    fprintf(stderr,
"Usage: test_rdt [OPTIONS] <data dir> <index name> <tree0> [tree1...]\n"
"\n"
"Measures the performance of one or more randomized decision trees across a\n"
"given index of test images.\n"
"\n"
"Using label maps test_rdt allows testing a decision tree whose labels don't\n"
"necessarily match the labels of the test set, and allows you to aggregate\n"
"multiple labels when reporting results, so for example you could measure the\n"
"accuracy of labelling the left vs right of the body or arms (normally comprised\n"
"of multiple labels).\n"
"\n"
"  --rdt-to-test-map=JSON  A label map from decision tree labels to test set\n"
"                          labels (E.g. for testing older trees with newer\n"
"                          test sets)\n"
"\n"
"  --test-to-out-map=JSON  A label map from test set labels to aggregated labels\n"
"                          (E.g. to group labels and see accuracy for the\n"
"                          left/right side of the body or whole limbs)\n"
"\n"
"  -o, --output=FILE       Output results in JSON format to this file\n"
"\n"
"  --index-output=FILE     Output filtered index of test images\n"
"  --index-low-accuracy=A  Index test images with an inference accuracy < A.\n"
"                          Range = (0,1]\n"
"\n"
"  -p, --pretty            Write pretty JSON output\n"
"  -r, --row-height=N      Number of rows per-label for confusion matrix bars\n"
"                          (default 2)\n"
"\n"
"  -f, --flip              Enable horizontal mirroring for enhanced inference\n"
"  -t, --threaded          Use multi-threaded inference.\n"
"\n"
"  -v, --verbose           Verbose output.\n"
"  -h, --help              Display this message.\n"
    );
    exit(1);
}

int
main(int argc, char **argv)
{
    uint64_t start, end;

    struct gm_logger *log = gm_logger_new(logger_cb, NULL);
    gm_logger_set_abort_callback(log, logger_abort_cb, NULL);

#define RDT_TO_TEST_MAP_OPT                 (CHAR_MAX + 1)
#define TEST_TO_OUT_MAP_OPT                 (CHAR_MAX + 2)
#define INDEX_OUTPUT_OPT                    (CHAR_MAX + 3)
#define INDEX_LOW_ACC_OPT                   (CHAR_MAX + 4)

    const char *short_options = "oprftvh";
    const struct option long_options[] = {
        {"rdt-to-test-map",  required_argument,  0, RDT_TO_TEST_MAP_OPT},
        {"test-to-out-map",  required_argument,  0, TEST_TO_OUT_MAP_OPT},
        {"output",           required_argument,  0, 'o'},
        {"index-output",      required_argument,  0, INDEX_OUTPUT_OPT},
        {"index-low-accuracy",required_argument,  0, INDEX_LOW_ACC_OPT},
        {"pretty",           no_argument,        0, 'p'},
        {"row-height",       required_argument,  0, 'r'},
        {"flip",             no_argument,        0, 'f'},
        {"threaded",         no_argument,        0, 't'},
        {"verbose",          no_argument,        0, 'v'},
        {"help",             no_argument,        0, 'h'},
        {0, 0, 0, 0}
    };

    JSON_Value *rdt_to_test_map_js = NULL;
    int n_test_labels = 0;
    JSON_Value *test_to_out_map_js = NULL;
    int n_out_labels = 0;
    JSON_Value *out_label_names = NULL;

    // Default options
    bool flip = false;
    bool pretty = false;
    const char *output = NULL;

    int opt;
    while ((opt = getopt_long(argc, argv, short_options, long_options, NULL))
           != -1)
    {
        switch (opt) {
        case RDT_TO_TEST_MAP_OPT:
            rdt_to_test_map_js =
                gm_data_load_label_map_from_json(log, optarg, rdt_to_test_map,
                                                 NULL);
            n_test_labels =
                json_array_get_count(json_array(rdt_to_test_map_js));
            break;
        case TEST_TO_OUT_MAP_OPT:
            test_to_out_map_js =
                gm_data_load_label_map_from_json(log, optarg, test_to_out_map,
                                                 NULL);
            n_out_labels =
                json_array_get_count(json_array(test_to_out_map_js));
            break;
        case 'o':
            output = optarg;
            break;
        case INDEX_OUTPUT_OPT:
            index_output_opt = optarg;
            break;
        case INDEX_LOW_ACC_OPT:
            {
                char *end = NULL;
                index_low_acc_opt = strtod(optarg, &end);
                gm_assert(log, end && *end == '\0',
                          "Failed to parse low accuracy threshold");
                gm_assert(log, index_low_acc_opt > 0 && index_low_acc_opt <= 1.0,
                          "Low accuracy threshold should be between 0 and 1");
            }
            break;
        case 'p':
            pretty = true;
            break;
        case 'r':
            rows_per_label_opt = atoi(optarg);
            gm_assert(log, rows_per_label_opt >= 1 && rows_per_label_opt <= 10,
                      "-r,--row-height value should be between 1 and 10 (inclusive)");
            break;
        case 'f':
            flip = true;
            break;
        case 't':
            threaded_opt = true;
            break;
        case 'v':
            verbose_opt = true;
            break;
        case 'h':
            usage();
            break;
        default:
            usage();
            break;
        }
    }

    if (argc - optind < 3)
        usage();

    const char *data_dir = argv[optind];
    const char *index_name = argv[optind + 1];

    struct gm_data_index *data_index =
        gm_data_index_open(log,
                           data_dir,
                           index_name,
                           NULL); // abort on error
    if (!data_index)
        return 1;

    JSON_Value *meta = gm_data_index_get_meta(data_index);
    int n_images = gm_data_index_get_len(data_index);
    int width = gm_data_index_get_width(data_index);
    int height = gm_data_index_get_height(data_index);

    JSON_Object* meta_camera =
        json_object_get_object(json_object(meta), "camera");
    float fov_rad = json_object_get_number(meta_camera, "vertical_fov");
    fov_rad *= (M_PI / 180.0);

    if (test_to_out_map_js) {
        out_label_names = json_value_deep_copy(test_to_out_map_js);
    } else {
        out_label_names = json_value_deep_copy(
            json_object_get_value(json_object(meta), "labels"));
    }

    if (n_test_labels) {
        gm_assert(log,
                  (n_test_labels ==
                   json_object_get_number(json_object(meta), "n_labels")),
                  "rdt-to-test label map size doesn't match n_labels for test set");
    } else {
        n_test_labels = json_object_get_number(json_object(meta), "n_labels");
    }

    if (n_out_labels) {
        gm_assert(log, n_out_labels <= n_test_labels,
                  "test-to-out label mapping defines more labels than in the test set");
    } else {
        n_out_labels = n_test_labels;
    }

    int n_trees = argc - optind - 2;
    RDTree *forest[n_trees];
    JSON_Value *forest_js[n_trees];

    start = get_time();
    for (int i = 0; i < n_trees; i++) {
        char *tree_path = argv[optind + 2 + i];

        forest_js[i] = json_parse_file(tree_path);
        gm_assert(log, forest_js[i] != NULL, "Failed to parse %s as JSON", tree_path);

        forest[i] = rdt_tree_load_from_json(log, forest_js[i],
                                            false, // don't load incomplete trees
                                            NULL); // abort on error
    }
    end = get_time();
    uint64_t load_forest_duration = end - start;

    printf("Decision trees expect background depth of %fm\n",
           forest[0]->header.bg_depth);

    JSON_Value *expected_labels =
        json_object_get_value(json_object(forest_js[0]), "labels");
    for (int i = 1; i < n_trees; i++) {
        char *tree_path = argv[optind + 2 + i];

        JSON_Value *tree_labels = json_object_get_value(json_object(forest_js[i]),
                                                        "labels");
        gm_assert(log,
                  check_consistent_tree_labels(expected_labels,
                                               tree_labels),
                  "Decision tree %s labels not consistent with %s",
                  tree_path,
                  argv[optind + 2]);
    }

    if (!rdt_to_test_map_js) {
        JSON_Value *test_data_labels = json_object_get_value(json_object(meta),
                                                             "labels");
        gm_assert(log,
                  check_consistent_tree_labels(expected_labels,
                                               test_data_labels),
                  "Test data labels expected to match decision tree if not using a mapping");
    }

    int n_rdt_labels = forest[0]->header.n_labels;
    if (!rdt_to_test_map_js) {
        for (int i = 0; i < n_rdt_labels; i++)
            rdt_to_test_map[i] = i;
    }
    if (!test_to_out_map_js) {
        for (int i = 0; i < n_test_labels; i++)
            test_to_out_map[i] = i;
    }

    /* test_rdt maintains support for older decision trees by maintaining
     * it's own implementations of the label inference code that are
     * selected based on stipulated sampling requirements in the loaded
     * decision tree.
     *
     * This lets us draw comparisons between old and new decision trees where
     * details for sampling UV pairs while training don't necessarily match
     * what the latest training code does.
     */

    /* NB: parson returns -1 for missing boolean values */
    int sample_uv_offsets_nearest =
        json_object_get_boolean(json_object(forest_js[0]), "sample_uv_offsets_nearest");
    if (sample_uv_offsets_nearest < 0)
        sample_uv_offsets_nearest = 0;

    /* NB: parson returns -1 for missing boolean values */
    int sample_uv_z_in_mm =
        json_object_get_boolean(json_object(forest_js[0]), "sample_uv_z_in_mm");
    if (sample_uv_z_in_mm < 0)
        sample_uv_z_in_mm = 0;

    if (sample_uv_offsets_nearest) {
        gm_assert(log, sample_uv_z_in_mm == true,
                  "Unsupported tree sampling requirements");
    }

    start = get_time();

    struct data_loader loader;
    loader.log = log;
    loader.last_update = get_time();
    loader.width = width;
    loader.height = height;
    loader.bg_depth = forest[0]->header.bg_depth;
    loader.depth_images = std::vector<float>((int64_t)width *
                                             height *
                                             n_images);
    loader.label_images = std::vector<uint8_t>((int64_t)width *
                                               height *
                                               n_images);
    if (index_output_opt) {
        loader.frame_paths = std::vector<char *>(n_images);
    }

    printf("Loading test data...\n");
    if (!gm_data_index_foreach(data_index,
                               load_test_data_cb,
                               &loader,
                               NULL)) // abort on error
    {
        return 1;
    }

    FILE *index_output_fd = NULL;
    if (index_output_opt) {
        index_output_fd = fopen(index_output_opt, "w");
        gm_assert(log, index_output_fd != NULL,
                  "Failed to open %s", index_output_opt);
    }

    float *depth_images = loader.depth_images.data();
    uint8_t *label_images = loader.label_images.data();
    char **file_paths = loader.frame_paths.data();

    end = get_time();
    uint64_t load_data_duration = end - start;

    std::vector<float> all_accuracies;
    all_accuracies.reserve(n_images);

    std::vector<int64_t> overall_confusion_matrix;
    overall_confusion_matrix.resize(n_out_labels * n_out_labels);
    int64_t overall_label_incidence[n_out_labels];
    memset(overall_label_incidence, 0, sizeof(overall_label_incidence));

    int64_t full_set_histogram[n_out_labels];
    int64_t full_set_n_pixels = 0; // (non-background, for normalizing histogram)

    memset(full_set_histogram, 0, sizeof(full_set_histogram));
    for (int i = 0; i < n_images; i++) {
        int64_t image_off = (int64_t)i * width * height;
        uint8_t *labels = &label_images[image_off];

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int off = y * width + x;

                uint8_t test_label = (int)labels[off];
                uint8_t out_label = test_to_out_map[test_label];

                // Ignore background pixels
                if (out_label == 0)
                    continue;

                full_set_histogram[out_label]++;
                full_set_n_pixels++;
            }
        }
    }

    float full_set_nhistogram[n_out_labels];
    for (int i = 0; i < n_out_labels; i++) {
        full_set_nhistogram[i] =
            (float)full_set_histogram[i] / full_set_n_pixels;
    }

    printf("Histogram of [mapped] test-set labels:\n");
    print_label_histogram(log,
                          json_array(out_label_names),
                          full_set_nhistogram);


    float *rdt_probs = (float*)xmalloc(width * height *
                                       sizeof(float) * n_rdt_labels);

    for (int i = 0; i < n_images; i++) {

        int64_t image_off = (int64_t)i * width * height;

        float *depth_image = &depth_images[image_off];
        uint8_t *labels = &label_images[image_off];

        int image_label_incidence[n_out_labels];
        memset(image_label_incidence, 0, sizeof(image_label_incidence));
        int image_best_label_matches[n_out_labels];
        memset(image_best_label_matches, 0, sizeof(image_best_label_matches));

        infer_labels(log,
                     forest,
                     n_trees,
                     depth_image,
                     width,
                     height,
                     rdt_probs,
                     threaded_opt,
                     flip);

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int off = y * width + x;

                uint8_t test_label = (int)labels[off];

                // Ignore background pixels
                if (test_label == 0)
                    continue;

                uint8_t out_label = test_to_out_map[test_label];

                float *rdt_pr_table = &rdt_probs[off * n_rdt_labels];
#if 0
                infer_pixel_label_probs(log,
                                        forest,
                                        n_trees,
                                        depth_image,
                                        width,
                                        height,
                                        x, y,
                                        rdt_pr_table);
#endif

                float test_pr_table[n_test_labels];
                memset(test_pr_table, 0, sizeof(test_pr_table));
                float out_pr_table[n_out_labels];
                memset(out_pr_table, 0, sizeof(out_pr_table));

                for (int l = 0; l < n_rdt_labels; l++) {
                    int mapped = rdt_to_test_map[l];
                    test_pr_table[mapped] += rdt_pr_table[l];
                }
                for (int l = 0; l < n_test_labels; l++) {
                    int mapped = test_to_out_map[l];
                    out_pr_table[mapped] += test_pr_table[l];
                }

                image_label_incidence[out_label]++;
                overall_label_incidence[out_label]++;

                /* XXX: the only reason we collect statistics about the average
                 * correctness of the best label is because Microsoft's paper
                 * used this metric so it's interesting to compare with their
                 * results.
                 *
                 * XXX: Looking at the best labels only disregards the heat-map
                 * nature of the labelling and the fact that tracking never
                 * cares which label is best - it's mainly just used to easily
                 * visualize labelling results.
                 *
                 * XXX: False positives that show up in larger labels (e.g. 5%
                 * of hip pixels being incorrectly labelled as the wrist) won't
                 * have a big impact on the accuracy for the larger labels but
                 * might hide a significant mis-labelling problem for smaller
                 * joints.
                 */
                uint8_t best_label = 0;
                float pr = -1.0;
                for (int l = 0; l < n_out_labels; l++) {
                    if (out_pr_table[l] > pr) {
                        best_label = l;
                        pr = out_pr_table[l];
                    }
                }

                if (out_label == best_label)
                    image_best_label_matches[best_label]++;
                overall_confusion_matrix[out_label * n_out_labels + best_label]++;
            }
        }

        int present_labels = 0;
        float accuracy = 0.f;
        for (int l = 0; l < n_out_labels; l++) {
            if (image_label_incidence[l] > 0) {
                accuracy += (image_best_label_matches[l] /
                             (float)image_label_incidence[l]);
                present_labels++;
            }
        }
        accuracy /= (float)present_labels;

        all_accuracies.push_back(accuracy);

        if (index_output_fd) {
            if (accuracy < index_low_acc_opt) {
                char *file_path = file_paths[i];
                fwrite(file_path, strlen(file_path), 1, index_output_fd);
                fputc('\n', index_output_fd);
            }
        }
    }


    /*
     * Post-processing of metrics...
     */

    std::sort(all_accuracies.begin(), all_accuracies.end());
    float best_accuracy = 0;
    float worst_accuracy = 1.0;
    float average_accuracy = 0;

    int histogram_len = 50; // height in terminal rows
    int histogram[histogram_len];
    int max_entries = 0; // which bucket has the most entries
    memset(histogram, 0, sizeof(histogram));

    for (int i = 0; i < (int)all_accuracies.size(); i++) {
        float accuracy = all_accuracies[i];

        if (accuracy < worst_accuracy)
            worst_accuracy = accuracy;
        if (accuracy > best_accuracy)
            best_accuracy = accuracy;

        average_accuracy += accuracy;

        int bucket = std::min((int)(accuracy * histogram_len), histogram_len - 1);
        histogram[bucket]++;
        if (histogram[bucket] > max_entries) {
            max_entries = histogram[bucket];
        }
    }
    gm_assert(log, max_entries > 0, "No accuracy histogram entries");

    average_accuracy /= n_images;

    gm_assert(log, (int)all_accuracies.size() == n_images,
              "Number of accuracy values (%d) doesn't match number of images (%d)",
              (int)all_accuracies.size(),
              n_images);

    /*
     * Reporting of metrics...
     */

    printf("Loaded %d decision trees in %.2f%s\n",
           n_trees,
           get_format_duration(load_forest_duration),
           get_format_duration_suffix(load_forest_duration));

    printf("Loaded %d images from '%s' index in %.2f%s\n",
           n_images,
           index_name,
           get_format_duration(load_data_duration),
           get_format_duration_suffix(load_data_duration));

    printf("Accuracy across all images:\n");
    printf("  • Average: %.2f\n", average_accuracy);
    printf("  • Median:  %.2f\n", all_accuracies[all_accuracies.size() / 2]);
    printf("  • Worst:   %.2f\n", worst_accuracy);
    printf("  • Best:    %.2f\n", best_accuracy);

    printf("Histogram of accuracies:\n");

    int max_bar_width = 30; // measured in terminal columns

    for (int i = 0; i < histogram_len; i++) {
        int bar_len = max_bar_width * 8 * histogram[i] / max_entries;
        printf("%-3d%%|", (int)(((float)i/histogram_len) * 100.0f));
        for (int j = 0; j < max_bar_width; j++) {
            if (bar_len > 8) {
                printf("%s", hbars[8]);
                bar_len -= 8;
            } else {
                printf("%s", hbars[bar_len]);
                bar_len = 0;
            }
        }
        printf("| %d\n", histogram[i]);
    }

    printf("\nConfusion matrix:\n");
    char line[1024];

    printf("%-15s     |", "");
    for (int x = 0; x < n_out_labels; x++) {
        if (x < 10)
            printf("  ");
        else
            printf("%d ", (int)(x / 10));
    }
    printf("\n");

    printf("%-15s     |", "");
    for (int x = 0; x < n_out_labels; x++)
        printf("%d ", x % 10);
    printf("\n");

    memset(line, '-', sizeof(line));
    line[std::min(n_out_labels * 2 + 27, (int)sizeof(line) - 1)] = '\0';
    printf("%s\n", line);

    for (int y_ = 0; y_ < n_out_labels * rows_per_label_opt; y_++) {
        int y = y_ / rows_per_label_opt;
        JSON_Object *label = json_array_get_object(json_array(out_label_names), y);
        const char *name = json_object_get_string(label, "name");
        int line_pos = 0;

        for (int x = 0; x < n_out_labels; x++) {
            char column[16];
            double f = 0;
            if (overall_label_incidence[y]) {
                f = (double)overall_confusion_matrix[y * n_out_labels + x] /
                    overall_label_incidence[y];
            }

            gm_assert(log, f <= 1.0, "Out of range confusion factor");

            int bar_len;
            if (f > 0.005)
                bar_len = std::max(1.0, round(rows_per_label_opt * 8.0 * f));
            else
                bar_len = 0;
            //bar_len = std::min(x, rows_per_label_opt * 8);

            int bar_index;
            int bar_part = (rows_per_label_opt-1) - (y_ % rows_per_label_opt);
            bar_len -= 8 * bar_part;
            if (bar_len < 0)
                bar_len = 0;
            if (bar_len < 8)
                bar_index = bar_len;
            else
                bar_index = 8;

            xsnprintf(column, sizeof(column), "%s ", vbars[bar_index]);
            //xsnprintf(column, sizeof(column), "%6.3f ", f * 100.0);
            int len = strlen(column);
            if ((int)sizeof(line) - line_pos > len + 1) {
                memcpy(line + line_pos, column, len + 1);
                line_pos += len;
            }
        }
        if (y_ % rows_per_label_opt != (rows_per_label_opt - 1))
            printf("%-15s     |%s|\n", "", line);
        else
            printf("%-15s  %2d |%s|\n", name, y, line);
    }
    memset(line, '-', sizeof(line));
    line[std::min(n_out_labels * 2 + 27, (int)sizeof(line) - 1)] = '\0';
    printf("%s\n", line);

    printf("%-15s     |", "");
    for (int x = 0; x < n_out_labels; x++) {
        if (x < 10)
            printf("  ");
        else
            printf("%d ", (int)(x / 10));
    }
    printf("\n");

    printf("%-15s     |", "");
    for (int x = 0; x < n_out_labels; x++)
        printf("%d ", x % 10);
    printf("\n");

    // Write output to JSON file
    if (output) {
        JSON_Value *json_output_root = json_value_init_object();

        // Output details about testing parameters
        JSON_Value *json_params = json_value_init_object();
        json_object_set_string(json_object(json_params), "data_dir", data_dir);
        json_object_set_string(json_object(json_params), "index", index_name);
        JSON_Value *tree_array = json_value_init_array();
        for (int i = 0; i < n_trees; ++i) {
            json_array_append_string(json_array(tree_array),
                                     argv[optind + 2 + i]);
        }
        json_object_set_value(json_object(json_params), "trees", tree_array);
        if (rdt_to_test_map_js) {
            json_object_set_value(json_object(json_params), "rdt_to_test_map",
                                  rdt_to_test_map_js);
        }
        if (test_to_out_map_js) {
            json_object_set_value(json_object(json_params), "test_to_out_map",
                                  test_to_out_map_js);
        }
        if (out_label_names) {
            json_object_set_value(json_object(json_params), "out_label_names",
                                  out_label_names);
        }
        json_object_set_boolean(json_object(json_params), "flip", flip);
        json_object_set_value(json_object(json_output_root), "params",
                              json_params);

        // Output accuracy summary
        JSON_Value *json_accuracy = json_value_init_object();
        json_object_set_number(json_object(json_accuracy),
                               "average", average_accuracy);
        json_object_set_number(json_object(json_accuracy),
                               "median",
                               all_accuracies[all_accuracies.size() / 2]);
        json_object_set_number(json_object(json_accuracy),
                               "worst", worst_accuracy);
        json_object_set_number(json_object(json_accuracy),
                               "best", best_accuracy);

        json_object_set_value(json_object(json_output_root), "accuracy",
                              json_accuracy);

        JSON_Value *results = json_value_init_object();
        json_object_set_number(json_object(results), "n_labels", n_out_labels);

        // Output confusion matrix and label incidence array
        JSON_Value *json_confusion_matrix = json_value_init_array();
        for (int i = 0; i < n_out_labels * n_out_labels; ++i) {
            json_array_append_number(json_array(json_confusion_matrix),
                                     overall_confusion_matrix[i]);
        }
        json_object_set_value(json_object(results), "confusion_matrix",
                              json_confusion_matrix);

        JSON_Value *json_label_incidence = json_value_init_array();
        for (int i = 0; i < n_out_labels; ++i) {
            json_array_append_number(json_array(json_label_incidence),
                                     overall_label_incidence[i]);
        }
        json_object_set_value(json_object(results), "label_incidence",
                              json_label_incidence);

        json_object_set_value(json_object(json_output_root), "results", results);

        if (pretty) {
            json_serialize_to_file_pretty(json_output_root, output);
        } else {
            json_serialize_to_file(json_output_root, output);
        }
        json_value_free(json_output_root);
    }

    if (index_output_fd) {
        fclose(index_output_fd);
    }

    // Clean up and quit
    for (int i = 0; i < n_trees; i++) {
        rdt_tree_destroy(forest[i]);
    }

    gm_data_index_destroy(data_index);
    data_index = NULL;

    return 0;
}
