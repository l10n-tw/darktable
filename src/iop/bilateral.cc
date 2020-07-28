/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#define __STDC_FORMAT_MACROS

extern "C" {
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
}
#include "iop/Permutohedral.h"
extern "C" {
#include <gtk/gtk.h>
#include <inttypes.h>

/**
 * implementation of the 5d-color bilateral filter using andrew adams et al.'s
 * permutohedral lattice, which they kindly provided online as c++ code, under new bsd license.
 */

DT_MODULE_INTROSPECTION(1, dt_iop_bilateral_params_t)

typedef struct dt_iop_bilateral_params_t
{
  // standard deviations of the gauss to use for blurring in the dimensions x,y,r,g,b (or L*,a*,b*)
  float radius;           // $MIN: 1.0 $MAX: 50.0 $DEFAULT: 15.0
  float reserved;         // $DEFAULT: 15.0
  float red, green, blue; // $MIN: 0.0001 $MAX: 1.0 $DEFAULT: 0.005
} dt_iop_bilateral_params_t; 

typedef struct dt_iop_bilateral_gui_data_t
{
  GtkWidget *radius, *red, *green, *blue;
} dt_iop_bilateral_gui_data_t;

typedef struct dt_iop_bilateral_data_t
{
  float sigma[5];
} dt_iop_bilateral_data_t;

const char *name()
{
  return _("denoise (bilateral filter)");
}

int default_group()
{
  return IOP_GROUP_CORRECT;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "radius"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "red"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "green"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "blue"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_bilateral_gui_data_t *g = (dt_iop_bilateral_gui_data_t *)self->gui_data;
  dt_accel_connect_slider_iop(self, "radius", GTK_WIDGET(g->radius));
  dt_accel_connect_slider_iop(self, "red", GTK_WIDGET(g->red));
  dt_accel_connect_slider_iop(self, "green", GTK_WIDGET(g->green));
  dt_accel_connect_slider_iop(self, "blue", GTK_WIDGET(g->blue));
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_bilateral_data_t *data = (dt_iop_bilateral_data_t *)piece->data;

  const int ch = piece->colors;
  float sigma[5];
  sigma[0] = data->sigma[0] * roi_in->scale / piece->iscale;
  sigma[1] = data->sigma[1] * roi_in->scale / piece->iscale;
  sigma[2] = data->sigma[2];
  sigma[3] = data->sigma[3];
  sigma[4] = data->sigma[4];
  if(fmaxf(sigma[0], sigma[1]) < .1)
  {
    memcpy(ovoid, ivoid, (size_t)sizeof(float) * ch * roi_out->width * roi_out->height);
    return;
  }

  // if rad <= 6 use naive version!
  const int rad = (int)(3.0 * fmaxf(sigma[0], sigma[1]) + 1.0);
  if(rad <= 6 && ((piece->pipe->type & DT_DEV_PIXELPIPE_THUMBNAIL) == DT_DEV_PIXELPIPE_THUMBNAIL))
  {
    // no use denoising the thumbnail. takes ages without permutohedral
    memcpy(ovoid, ivoid, (size_t)sizeof(float) * ch * roi_out->width * roi_out->height);
  }
  else if(rad <= 6)
  {
    static const size_t weights_size = 2 * (6 + 1) * 2 * (6 + 1);
    float mat[weights_size];
    const int wd = 2 * rad + 1;
    float *m = mat + rad * wd + rad;
    float weight = 0.0f;
    const float isig2col[3] = { 1.f / (2.0f * sigma[2] * sigma[2]), 1.f / (2.0f * sigma[3] * sigma[3]),
                                1.f / (2.0f * sigma[4] * sigma[4]) };
    // init gaussian kernel
    for(int l = -rad; l <= rad; l++)
      for(int k = -rad; k <= rad; k++)
        weight += m[l * wd + k] = expf(-(l * l + k * k) / (2.f * sigma[0] * sigma[0]));
    for(int l = -rad; l <= rad; l++)
      for(int k = -rad; k <= rad; k++) m[l * wd + k] /= weight;

    float *const weights_buf = (float *)malloc(weights_size * dt_get_num_threads() * sizeof(float));

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(ch, ivoid, ovoid, rad, roi_in, roi_out, wd, weights_buf) \
    shared(m, mat, isig2col) \
    schedule(static)
#endif
    for(int j = rad; j < roi_out->height - rad; j++)
    {
      const float *in = ((float *)ivoid) + ch * ((size_t)j * roi_in->width + rad);
      float *out = ((float *)ovoid) + ch * ((size_t)j * roi_out->width + rad);
      float *weights = weights_buf + weights_size * dt_get_thread_num();
      float *w = weights + rad * wd + rad;
      float sumw;
      for(int i = rad; i < roi_out->width - rad; i++)
      {
        sumw = 0.0f;
        for(int l = -rad; l <= rad; l++)
          for(int k = -rad; k <= rad; k++)
          {
	    const float *inp = in + ch * (l * roi_in->width + k);
            sumw += w[l * wd + k] = m[l * wd + k]
                                    * expf(-((in[0] - inp[0]) * (in[0] - inp[0]) * isig2col[0]
                                             + (in[1] - inp[1]) * (in[1] - inp[1]) * isig2col[1]
                                             + (in[2] - inp[2]) * (in[2] - inp[2]) * isig2col[2]));
          }
        for(int l = -rad; l <= rad; l++)
          for(int k = -rad; k <= rad; k++) w[l * wd + k] /= sumw;
        for(int c = 0; c < 3; c++) out[c] = 0.0f;
        for(int l = -rad; l <= rad; l++)
          for(int k = -rad; k <= rad; k++)
          {
            const float *inp = in + ch * ((size_t)l * roi_in->width + k);
            float pix_weight = w[(size_t)l * wd + k];
            for(int c = 0; c < 3; c++) out[c] += inp[c] * pix_weight;
          }
        out += ch;
        in += ch;
      }
    }

    free((void *)weights_buf);

    // fill unprocessed border
    for(int j = 0; j < rad; j++)
      memcpy(((float *)ovoid) + (size_t)ch * j * roi_out->width,
             ((float *)ivoid) + (size_t)ch * j * roi_in->width, (size_t)ch * sizeof(float) * roi_out->width);
    for(int j = roi_out->height - rad; j < roi_out->height; j++)
      memcpy(((float *)ovoid) + (size_t)ch * j * roi_out->width,
             ((float *)ivoid) + (size_t)ch * j * roi_in->width, (size_t)ch * sizeof(float) * roi_out->width);
    for(int j = rad; j < roi_out->height - rad; j++)
    {
      const float *in = ((float *)ivoid) + (size_t)ch * roi_out->width * j;
      float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;
      for(int i = 0; i < rad; i++)
        for(int c = 0; c < 3; c++) out[ch * i + c] = in[ch * i + c];
      for(int i = roi_out->width - rad; i < roi_out->width; i++)
        for(int c = 0; c < 3; c++) out[ch * i + c] = in[ch * i + c];
    }
  }
  else
  {
    for(int k = 0; k < 5; k++) sigma[k] = 1.0f / sigma[k];
    PermutohedralLattice<5, 4> lattice((size_t)roi_in->width * roi_in->height, omp_get_max_threads());

// splat into the lattice
#ifdef _OPENMP
#pragma omp parallel for
#endif
    for(int j = 0; j < roi_in->height; j++)
    {
      const float *in = (const float *)ivoid + (size_t)j * roi_in->width * ch;
      const int thread = omp_get_thread_num();
      size_t index = (size_t)j * roi_in->width;
      for(int i = 0; i < roi_in->width; i++, index++)
      {
        float pos[5] = { i * sigma[0], j * sigma[1], in[0] * sigma[2], in[1] * sigma[3], in[2] * sigma[4] };
        float val[4] = { in[0], in[1], in[2], 1.0 };
        lattice.splat(pos, val, index, thread);
        in += ch;
      }
    }

    lattice.merge_splat_threads();

    // blur the lattice
    lattice.blur();

// slice from the lattice
#ifdef _OPENMP
#pragma omp parallel for
#endif
    for(int j = 0; j < roi_in->height; j++)
    {
      float *out = (float *)ovoid + (size_t)j * roi_in->width * ch;
      size_t index = (size_t)j * roi_in->width;
      for(int i = 0; i < roi_in->width; i++, index++)
      {
        float val[4];
        lattice.slice(val, index);
        for(int k = 0; k < 3; k++) out[k] = val[k] / val[3];
        out += ch;
      }
    }
  }

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_bilateral_params_t *p = (dt_iop_bilateral_params_t *)p1;
  dt_iop_bilateral_data_t *d = (dt_iop_bilateral_data_t *)piece->data;
  d->sigma[0] = p->radius;
  d->sigma[1] = p->radius;
  d->sigma[2] = p->red;
  d->sigma[3] = p->green;
  d->sigma[4] = p->blue;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_bilateral_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_bilateral_gui_data_t *g = (dt_iop_bilateral_gui_data_t *)self->gui_data;
  dt_iop_bilateral_params_t *p = (dt_iop_bilateral_params_t *)module->params;
  dt_bauhaus_slider_set_soft(g->radius, p->radius);
  // dt_bauhaus_slider_set(g->scale2, p->sigma[1]);
  dt_bauhaus_slider_set_soft(g->red, p->red);
  dt_bauhaus_slider_set_soft(g->green, p->green);
  dt_bauhaus_slider_set_soft(g->blue, p->blue);
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_bilateral_data_t *data = (dt_iop_bilateral_data_t *)piece->data;
  float sigma[5];
  sigma[0] = data->sigma[0] * roi_in->scale / piece->iscale;
  sigma[1] = data->sigma[1] * roi_in->scale / piece->iscale;
  const int rad = (int)(3.0 * fmaxf(sigma[0], sigma[1]) + 1.0);
  tiling->factor = 2 + 50;
  tiling->overhead = 0;
  tiling->overlap = rad;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = (dt_iop_gui_data_t *)malloc(sizeof(dt_iop_bilateral_gui_data_t));
  dt_iop_bilateral_gui_data_t *g = (dt_iop_bilateral_gui_data_t *)self->gui_data;

  g->radius = dt_bauhaus_slider_from_params(self, N_("radius"));
  gtk_widget_set_tooltip_text(g->radius, _("spatial extent of the gaussian"));
  dt_bauhaus_slider_set_soft_range(g->radius, 1.0, 30.0);
  dt_bauhaus_slider_set_step(g->radius, 1.0);

  g->red = dt_bauhaus_slider_from_params(self, N_("red"));
  gtk_widget_set_tooltip_text(g->red, _("how much to blur red"));
  dt_bauhaus_slider_set_soft_max(g->red, 0.1);
  dt_bauhaus_slider_set_digits(g->red, 4);

  g->green = dt_bauhaus_slider_from_params(self, N_("green"));
  gtk_widget_set_tooltip_text(g->green, _("how much to blur green"));
  dt_bauhaus_slider_set_soft_max(g->green, 0.1);
  dt_bauhaus_slider_set_digits(g->green, 4);

  g->blue = dt_bauhaus_slider_from_params(self, N_("blue"));
  gtk_widget_set_tooltip_text(g->blue, _("how much to blur blue"));
  dt_bauhaus_slider_set_soft_max(g->blue, 0.1);
  dt_bauhaus_slider_set_digits(g->blue, 4);
}
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;