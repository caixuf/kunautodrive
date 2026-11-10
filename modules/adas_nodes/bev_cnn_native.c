/**
 * bev_cnn_native.c — 纯 C CenterPoint-Lite BEV 卷积检测与解码核实现
 */
#include "bev_cnn_native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

void bev_cnn_free(BevCnnModel* model) {
    if (!model) return;
    for (uint32_t i = 0; i < model->n_layers; ++i) {
        if (model->layers[i].weights) {
            free(model->layers[i].weights);
            model->layers[i].weights = NULL;
        }
        if (model->layers[i].bias) {
            free(model->layers[i].bias);
            model->layers[i].bias = NULL;
        }
    }
    if (model->buf_a) {
        free(model->buf_a);
        model->buf_a = NULL;
    }
    if (model->buf_b) {
        free(model->buf_b);
        model->buf_b = NULL;
    }
    memset(model, 0, sizeof(*model));
}

int bev_cnn_load(BevCnnModel* model, const char* bin_path) {
    if (!model || !bin_path) return -1;
    bev_cnn_free(model);

    FILE* fp = fopen(bin_path, "rb");
    if (!fp) {
        return -1;
    }

    uint32_t magic = 0;
    if (fread(&magic, sizeof(uint32_t), 1, fp) != 1 || magic != BEV_CNN_MAGIC) {
        fclose(fp);
        return -1;
    }

    if (fread(&model->version, sizeof(uint32_t), 1, fp) != 1 ||
        fread(&model->n_layers, sizeof(uint32_t), 1, fp) != 1 ||
        fread(&model->in_c, sizeof(uint32_t), 1, fp) != 1 ||
        fread(&model->in_h, sizeof(uint32_t), 1, fp) != 1 ||
        fread(&model->in_w, sizeof(uint32_t), 1, fp) != 1 ||
        fread(&model->out_c, sizeof(uint32_t), 1, fp) != 1 ||
        fread(&model->out_h, sizeof(uint32_t), 1, fp) != 1 ||
        fread(&model->out_w, sizeof(uint32_t), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }

    if (model->n_layers > BEV_CNN_MAX_LAYERS || model->n_layers == 0) {
        fclose(fp);
        return -1;
    }

    size_t max_channel_map = 0;

    for (uint32_t i = 0; i < model->n_layers; ++i) {
        BevCnnLayer* l = &model->layers[i];
        if (fread(&l->in_channels, sizeof(uint32_t), 1, fp) != 1 ||
            fread(&l->out_channels, sizeof(uint32_t), 1, fp) != 1 ||
            fread(&l->kernel_size, sizeof(uint32_t), 1, fp) != 1 ||
            fread(&l->padding, sizeof(uint32_t), 1, fp) != 1 ||
            fread(&l->stride, sizeof(uint32_t), 1, fp) != 1 ||
            fread(&l->activation, sizeof(uint32_t), 1, fp) != 1) {
            fclose(fp);
            bev_cnn_free(model);
            return -1;
        }

        size_t n_w = (size_t)l->out_channels * l->in_channels * l->kernel_size * l->kernel_size;
        size_t n_b = (size_t)l->out_channels;

        l->weights = (float*)malloc(n_w * sizeof(float));
        l->bias = (float*)malloc(n_b * sizeof(float));
        if (!l->weights || !l->bias) {
            fclose(fp);
            bev_cnn_free(model);
            return -1;
        }

        if (fread(l->weights, sizeof(float), n_w, fp) != n_w ||
            fread(l->bias, sizeof(float), n_b, fp) != n_b) {
            fclose(fp);
            bev_cnn_free(model);
            return -1;
        }

        if (l->out_channels > max_channel_map) {
            max_channel_map = l->out_channels;
        }
    }

    fclose(fp);

    /* 分配中间层缓冲 */
    model->buf_elems = max_channel_map * model->in_h * model->in_w;
    model->buf_a = (float*)malloc(model->buf_elems * sizeof(float));
    model->buf_b = (float*)malloc(model->buf_elems * sizeof(float));
    if (!model->buf_a || !model->buf_b) {
        bev_cnn_free(model);
        return -1;
    }

    model->loaded = 1;
    return 0;
}

static void conv_forward_layer(const BevCnnLayer* l, uint32_t H, uint32_t W,
                               const float* in, float* out) {
    const uint32_t hw = H * W;

    if (l->kernel_size == 1 && l->padding == 0) {
        /* 1x1 卷积快速路径 */
#ifdef _OPENMP
        #pragma omp parallel for schedule(static)
#endif
        for (uint32_t oc = 0; oc < l->out_channels; ++oc) {
            float b = l->bias[oc];
            float* out_plane = out + (size_t)oc * hw;
            const float* w_oc = l->weights + (size_t)oc * l->in_channels;

            for (uint32_t i = 0; i < hw; ++i) {
                out_plane[i] = b;
            }

            for (uint32_t ic = 0; ic < l->in_channels; ++ic) {
                float w = w_oc[ic];
                const float* in_plane = in + (size_t)ic * hw;
                for (uint32_t i = 0; i < hw; ++i) {
                    out_plane[i] += in_plane[i] * w;
                }
            }

            if (l->activation == 1) {
                for (uint32_t i = 0; i < hw; ++i) {
                    if (out_plane[i] < 0.0f) out_plane[i] = 0.0f;
                }
            } else if (l->activation == 2 && oc < 4) {
                for (uint32_t i = 0; i < hw; ++i) {
                    out_plane[i] = 1.0f / (1.0f + expf(-out_plane[i]));
                }
            }
        }
        return;
    }

    /* 3x3 卷积：流式内存访问 + 无内部分支向量化 */
#ifdef _OPENMP
    #pragma omp parallel for schedule(static)
#endif
    for (uint32_t oc = 0; oc < l->out_channels; ++oc) {
        float b = l->bias[oc];
        float* out_plane = out + (size_t)oc * hw;
        const float* w_oc = l->weights + (size_t)oc * l->in_channels * 9;

        for (uint32_t i = 0; i < hw; ++i) {
            out_plane[i] = b;
        }

        for (uint32_t ic = 0; ic < l->in_channels; ++ic) {
            const float* in_plane = in + (size_t)ic * hw;
            const float* w_ic = w_oc + ic * 9;

            for (uint32_t kr = 0; kr < 3; ++kr) {
                int r_start = (kr == 0) ? 1 : 0;
                int r_end   = (kr == 2) ? (int)H - 1 : (int)H;

                for (uint32_t kc = 0; kc < 3; ++kc) {
                    float w = w_ic[kr * 3 + kc];
                    int c_start = (kc == 0) ? 1 : 0;
                    int c_end   = (kc == 2) ? (int)W - 1 : (int)W;

                    for (int r = r_start; r < r_end; ++r) {
                        int in_r = r + (int)kr - 1;
                        const float* in_row = in_plane + (size_t)in_r * W;
                        float* out_row = out_plane + (size_t)r * W;

                        for (int c = c_start; c < c_end; ++c) {
                            int in_c = c + (int)kc - 1;
                            out_row[c] += in_row[in_c] * w;
                        }
                    }
                }
            }
        }

        if (l->activation == 1) {
            for (uint32_t i = 0; i < hw; ++i) {
                if (out_plane[i] < 0.0f) out_plane[i] = 0.0f;
            }
        }
    }
}

int bev_cnn_forward(const BevCnnModel* model, const float* in_feat, float* out_feat) {
    if (!model || !model->loaded || !in_feat || !out_feat) return -1;
    if (model->n_layers < 1) return -1;

    const uint32_t H = model->in_h;
    const uint32_t W = model->in_w;

    const float* cur_in = in_feat;
    float* cur_out = model->buf_a;

    for (uint32_t i = 0; i < model->n_layers; ++i) {
        const BevCnnLayer* l = &model->layers[i];
        if (i == model->n_layers - 1) {
            cur_out = out_feat;
        } else if (i % 2 == 0) {
            cur_out = model->buf_a;
        } else {
            cur_out = model->buf_b;
        }

        conv_forward_layer(l, H, W, cur_in, cur_out);
        cur_in = cur_out;
    }
    return 0;
}

int bev_cnn_decode(const BevCnnModel* model,
                   const float* out_feat,
                   const BevPreConfig* precfg,
                   double ego_heading,
                   double ego_y,
                   BevPostDet* dets,
                   int max_dets) {
    (void)model;
    if (!out_feat || !precfg || !dets || max_dets <= 0) return 0;
    const int H = precfg->h;
    const int W = precfg->w;
    const double res_x = (2.0 * precfg->range_x) / W;
    const double res_y = (2.0 * precfg->range_y_half) / H;
    const double ch = cos(ego_heading);
    const double sh = sin(ego_heading);

    const float conf_thresh = 0.33f;
    int n_dets = 0;

    for (int cls = 0; cls < 4; ++cls) {
        const float* hm = out_feat + (size_t)cls * H * W;
        for (int r = 0; r < H && n_dets < max_dets; ++r) {
            for (int c = 0; c < W && n_dets < max_dets; ++c) {
                float v = hm[r * W + c];
                if (v < conf_thresh) continue;

                /* 3x3 局部极大值抑制 */
                int is_max = 1;
                for (int dr = -1; dr <= 1; ++dr) {
                    int nr = r + dr;
                    if (nr < 0 || nr >= H) continue;
                    for (int dc = -1; dc <= 1; ++dc) {
                        if (dr == 0 && dc == 0) continue;
                        int nc = c + dc;
                        if (nc < 0 || nc >= W) continue;
                        if (hm[nr * W + nc] > v) {
                            is_max = 0;
                            break;
                        }
                    }
                    if (!is_max) break;
                }
                if (!is_max) continue;

                /* 亚像素偏移 (ch4: dx, ch5: dy) */
                float dx = out_feat[(4 * H + r) * W + c];
                float dy = out_feat[(5 * H + r) * W + c];
                if (dx < -0.5f) dx = -0.5f; else if (dx > 0.5f) dx = 0.5f;
                if (dy < -0.5f) dy = -0.5f; else if (dy > 0.5f) dy = 0.5f;

                double xb = -precfg->range_x + ((double)c + 0.5 + (double)dx) * res_x;
                double yb =  precfg->range_y_half - ((double)r + 0.5 + (double)dy) * res_y;

                /* 尺寸 (ch6: width, ch7: length) */
                float w = out_feat[(6 * H + r) * W + c];
                float l = out_feat[(7 * H + r) * W + c];
                if (w < 0.2f) w = 0.2f; else if (w > 8.0f) w = 8.0f;
                if (l < 0.2f) l = 0.2f; else if (l > 15.0f) l = 15.0f;

                /* 车体系速度 (ch8: vx, ch9: vy) */
                float vx = out_feat[(8 * H + r) * W + c];
                float vy = out_feat[(9 * H + r) * W + c];

                BevPostDet* d = &dets[n_dets++];
                d->x = (float)xb;
                d->y = (float)yb;
                d->vx = vx;
                d->vy = vy;
                d->width = w;
                d->length = l;
                d->type = cls;
                d->confidence = v;
                d->world_y = (float)(ego_y + xb * sh + yb * ch);
            }
        }
    }
    return n_dets;
}
