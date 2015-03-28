/*
 * render.c - Image calculation and rendering
 *
 * de Jong Explorer - interactive exploration of the Peter de Jong attractor
 * Copyright (C) 2004 David Trowbridge and Micah Dowty
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "de-jong.h"

struct vector2 point;

static float get_pixel_scale();
static void update_color_table();


void resize(int w, int h, int oversample) {
  render.width = w;
  render.height = h;
  render.oversample = oversample;

  if (render.counts)
    g_free(render.counts);
  render.counts = g_malloc(sizeof(render.counts[0]) * w * h * oversample * oversample);

  if (render.pixels)
    g_free(render.pixels);
  render.pixels = g_malloc(4 * render.width * render.height);

  clear();
}

void clear() {
  memset(render.counts, 0, render.width * render.height *
	 render.oversample * render.oversample * sizeof(int));
  render.current_density = 0;
  render.iterations = 0;
  point.x = uniform_variate();
  point.y = uniform_variate();
}

static float get_pixel_scale() {
  /* Calculate the scale factor for converting count[] values to luminance
   * values between 0 and 1.
   */
  float density, fscale;

  /* Scale our counts to a luminance between 0 and 1 that gets fed through our
   * colormap[] to generate an actual gdk color. 'p' contains the number of
   * times our point has passed the current pixel.
   *
   * iterations / (width * height) gives us the average density of counts[].
   */
  density = render.iterations / (render.width * render.height);

  /* fscale is a floating point number that, when multiplied by a raw
   * counts[] value, gives values between 0 and 1 corresponding to full
   * white and full black.
   */
  fscale = (render.exposure * params.zoom) / density;

  /* The very first frame we render will often be very underexposed.
   * If fscale > 0.5, this makes countsclamp negative and we get incorrect
   * results. The lowest usable value of countsclamp is 1.
   */
  if (fscale > 0.5)
    fscale = 0.5;

  return fscale;
}

static void update_color_table() {
  /* Reallocate the color table if necessary, then regenerate its contents
   * to match the current_density, exposure, gamma, and other rendering parameters.
   * The color tabls is what maps counts[] values to pixels[] values quickly.
   *
   * Note that the target_density doesn't include oversampling. Since the
   * final accumulated count value can range to the current density times
   * oversample^2, we need that much of a larger array. Yay. We don't however
   * have to account for this when computing fscale, since it uses the
   * non-oversampled size in calculating image density.
   */
  guint oversampled_density = render.current_density * render.oversample * render.oversample;
  guint required_size = oversampled_density + 1;
  guint count;
  int r, g, b;
  float pixel_scale = get_pixel_scale();
  float luma;
  double one_over_gamma = 1/render.gamma;

  /* Current table too small? */
  if (render.color_table_size < required_size) {
    if (render.color_table)
      g_free(render.color_table);

    /* Allocate it to double the size we need now, as we expect our needs to grow. */
    render.color_table_size = required_size * 2;
    render.color_table = g_malloc(render.color_table_size * sizeof(render.color_table[0]));
  }

  /* Generate one color for every currently-possible count value... */
  for (count=0; count<=oversampled_density; count++) {

    /* Scale and gamma-correct */
    luma = count * pixel_scale;
    luma = pow(luma, one_over_gamma);

    /* Optionally clamp before interpolating */
    if (render.clamped && luma > 1)
      luma = 1;

    /* Linearly interpolate between fgcolor and bgcolor */
    r = ((int)(render.bgcolor.red   * (1-luma) + render.fgcolor.red   * luma)) >> 8;
    g = ((int)(render.bgcolor.green * (1-luma) + render.fgcolor.green * luma)) >> 8;
    b = ((int)(render.bgcolor.blue  * (1-luma) + render.fgcolor.blue  * luma)) >> 8;

    /* Always clamp color components */
    if (r<0) r = 0;  if (r>255) r = 255;
    if (g<0) g = 0;  if (g>255) g = 255;
    if (b<0) b = 0;  if (b>255) b = 255;

    /* Colors are always ARGB order in little endian */
    render.color_table[count] = GUINT32_TO_LE( 0xFF000000UL | (b<<16) | (g<<8) | r );
  }
}

void update_pixels() {
  /* Convert counts[] to colored 8-bit ARGB image data using our color lookup table,
   * downsampling by combining all count buckets that represent each of our output pixels.
   */
  guint32 *pixel_p;
  guint *count_p, *sample_p;
  const int oversample = render.oversample;
  const int sample_stride = (render.width * oversample) - oversample;
  const int sample_y_stride = (render.width * oversample) * (oversample - 1);
  int x, y, sample_x, sample_y;
  guint sample;

  update_color_table();

  pixel_p = render.pixels;
  count_p = render.counts;

  if (oversample > 1) {
    /* Nice ugly loop that downsamples from counts[] to pixels[] */

    for (y=render.height; y; y--) {
      for (x=render.width; x; x--) {

	/* For each output pixel, accumulate an oversample^2 region of point counts */
	sample = 0;
	sample_p = count_p;
	for (sample_y=oversample; sample_y; sample_y--) {
	  for (sample_x=oversample; sample_x; sample_x--) {
	    sample += *(sample_p++);
	  }
	  sample_p += sample_stride;
	}

	count_p += oversample;
	*(pixel_p++) = render.color_table[sample];
      }
      count_p += sample_y_stride;
    }
  }
  else {
    /* A much simpler and faster loop to use when oversampling is disabled */

    for (y=render.height; y; y--)
      for (x=render.width; x; x--)
	*(pixel_p++) = render.color_table[*(count_p++)];
  }

  render.dirty_flag = FALSE;
}

float uniform_variate() {
  /* A uniform random variate between 0 and 1 */
  return ((float) rand()) / RAND_MAX;
}

float normal_variate() {
  /* A unit-normal random variate, implemented with the Box-Muller method */
  return sqrt(-2*log(uniform_variate())) * cos(uniform_variate() * (2*M_PI));
}

void run_iterations(int count) {
  const int count_width = render.width * render.oversample;
  const int count_height = render.height * render.oversample;
  const double xcenter = count_width / 2.0;
  const double ycenter = count_height / 2.0;
  const double scale = xcenter / 2.5 * params.zoom;
  const gboolean rotation_enabled = params.rotation > 0.0001 || params.rotation < -0.0001;
  const gboolean blur_enabled = params.blur_ratio > 0.0001 && params.blur_radius > 0.00001;
  const int blur_table_size = 1024; /* Must be a power of two */

  double x, y, sine_rotation, cosine_rotation;
  unsigned int i, ix, iy;
  guint *p;
  guint d;
  int blur_index;
  float blur_table[blur_table_size];

  /* Precalculate the sine and cosine of the rotation angle, if we'll need it */
  if (rotation_enabled) {
    sine_rotation = sin(params.rotation);
    cosine_rotation = cos(params.rotation);
  }

  /* Initialize the blur table with a set of precalculated normally distributed
   * random numbers. Larger blur tables just increase the independence between
   * blocks of iterations. Since a new blur table is calculated at each run_iterations,
   * at infinity the image still has the same effect, but each iteration runs much faster.
   */
  if (blur_enabled) {
    for (i=0; i<blur_table_size; i++)
      blur_table[i] = normal_variate() * params.blur_radius;
    blur_index = 0;
  }

  for(i=count; i; --i) {
    /* These are the actual Peter de Jong map equations. The new point value
     * gets stored into 'point', then we go on and mess with x and y before plotting.
     */
    x = sin(params.a * point.y) - cos(params.b * point.x);
    y = sin(params.c * point.x) - cos(params.d * point.y);
    point.x = x;
    point.y = y;

    /* If rotation is enabled, rotate each point around
     * the origin by params.rotation radians.
     */
    if (rotation_enabled) {
      x =  cosine_rotation * point.x + sine_rotation   * point.y;
      y = -sine_rotation   * point.x + cosine_rotation * point.y;
    }

    /* If blurring is enabled, use blur_ratio to decide how often to perturb
     * the apparent point position, and blur_radius to determine how much.
     * By perturbing the point using a normal variate, we create a true gaussian
     * blur as the number of iterations approaches infinity.
     */
    if (blur_enabled) {
      if (uniform_variate() < params.blur_ratio) {
	x += blur_table[blur_index];
	blur_index = (blur_index+1) & (blur_table_size-1);
	y += blur_table[blur_index];
	blur_index = (blur_index+1) & (blur_table_size-1);
      }
    }

    /* Scale and translate our (x,y) coordinates into pixel coordinates */
    ix = (int)((x + params.xoffset) * scale + xcenter);
    iy = (int)((y + params.yoffset) * scale + ycenter);

    /* Clip to the size of our image. Note that ix and iy are
     * unsigned, so we only have to make one comparison each.
     */
    if (ix < count_width && iy < count_height) {
      p = render.counts + ix + count_width * iy;
      d = *p = *p + 1;
      if (d > render.current_density)
	render.current_density = d;
    }
  }
  render.iterations += count;
}

/* The End */
