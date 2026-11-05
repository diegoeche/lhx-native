#pragma once
#include <math.h>
#include <stdint.h>
/* Reference service paint and model-space panel/camouflage patterns. */
static inline double lhx_surface_clamp(double v){return fmax(0,fmin(1,v));}
static inline double lhx_surface_smooth(double a,double b,double v){double t=lhx_surface_clamp((v-a)/(b-a));return t*t*(3-2*t);}
static inline double lhx_surface_hash(double x,double y){double v=sin(x*127.1+y*311.7)*43758.5453;return v-floor(v);}
static inline double lhx_surface_noise(double x,double y){double i=floor(x),j=floor(y),u=x-i,v=y-j;
 u=u*u*(3-2*u);v=v*v*(3-2*v);
 return (1-v)*((1-u)*lhx_surface_hash(i,j)+u*lhx_surface_hash(i+1,j))+
        v*((1-u)*lhx_surface_hash(i,j+1)+u*lhx_surface_hash(i+1,j+1));}
static inline double lhx_surface_detail(int kind,double u,double v,double du,double dv){
 double k=(.93+.14*lhx_surface_noise(u*.3,v*.3))*(.97+.06*lhx_surface_noise(u*1.7,v*1.7));
 if(kind==2){double c=lhx_surface_noise(u*.05+7.3,v*.05+7.3)+.5*lhx_surface_noise(u*.13+2.1,v*.13+2.1);
  return k*(1-.32*lhx_surface_smooth(.88,.95,c));}
 double x=u/9,y=v/6,wx=fmax(du/9,1e-8),wy=fmax(dv/6,1e-8);
 double lx=1-lhx_surface_smooth(0,wx*1.2,fabs((x-.5-floor(x-.5))-.5));
 double ly=1-lhx_surface_smooth(0,wy*1.2,fabs((y-.5-floor(y-.5))-.5));
 return k*(1-.2*fmax(lx,ly)*lhx_surface_clamp(1-fmax(wx,wy)*5));
}
static inline uint32_t lhx_military_paint(uint32_t color){
 double rgb[3]={((color>>16)&255)/255.,((color>>8)&255)/255.,(color&255)/255.};
 const double warm[]={.05,.03,-.03},minimum[]={.13,.14,.11};
 double l=.3*rgb[0]+.59*rgb[1]+.11*rgb[2];uint32_t result=0xff000000;
 for(unsigned i=0;i<3;i++){double c=fmax(minimum[i],(l+(rgb[i]-l)*.35)*.75+warm[i]);
  result|=(uint32_t)(fmin(255,c*255)+.5)<<(16-i*8);}
 return result;
}
