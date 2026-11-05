#pragma once
#include <math.h>
#include <stdint.h>

/* Reference Enhanced-mode haze: web/gpu.js PhotoSky and HAZE_DENSITY.
 * Distances use the existing scene's common camera units. */
#define LHX_HAZE_DENSITY 0.000004f
static inline uint32_t lhx_haze_color(uint32_t sky)
{
    unsigned sum=((sky>>16)&255)+((sky>>8)&255)+(sky&255);
    return sum<115?0xff0d0f17u:0xffa3b0bfu;
}
static inline uint32_t lhx_atmosphere_mix(uint32_t color,uint32_t haze,double visibility)
{
    visibility=fmax(0,fmin(1,visibility));
    uint32_t result=0xff000000u;
    for(unsigned shift=0;shift<=16;shift+=8)
        result|=(uint32_t)((((color>>shift)&255)*visibility+
                           ((haze>>shift)&255)*(1-visibility))+0.5)<<shift;
    return result;
}
static inline double lhx_haze_visibility(double forward)
{
    double d=fmax(0,forward)*LHX_HAZE_DENSITY;
    return exp(-d*d);
}
