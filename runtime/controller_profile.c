#include "controller_profile.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const names[]={"roll","pitch","rudder","throttle","increase_lift","decrease_lift",
  "maximum_lift","half_lift","gear","fire","previous_weapon","next_weapon","chaff","flares",
  "cockpit","chase","toggle_view","pause","confirm","back","menu_up","menu_down","menu_left","menu_right"};
static const char *const labels[]={"Roll","Pitch","Rudder","Throttle / lift 0-100%","Increase lift","Decrease lift",
  "Maximum lift 100%","Set lift 50%","Landing gear","Fire","Previous weapon","Next weapon","Chaff","Flares",
  "Cockpit view","Chase view","Toggle view","Pause","Confirm / next target","Back / Escape","Menu up","Menu down","Menu left","Menu right"};
static const char *const behaviors[]={"analog","direct","relative","increase","decrease","preset","positive","negative","hold","legacy_preset"};
static const char *const behavior_labels[]={"Axis controls signed flight","Position sets lift (0-100%)","Centered axis: increase/decrease","Hold to increase lift",
  "Hold to decrease lift","Set chosen lift percentage","Hold positive flight direction","Hold negative flight direction","Activate beyond threshold / held","Original keyboard preset"};
const char *lhx_usb_behavior_label(int n) { return n>=0 && n<USB_BEHAVIORS?behavior_labels[n]:"Unknown behavior"; }
const char *lhx_usb_action_name(int n) { return n>=0 && n<USB_ACTIONS?names[n]:NULL; }
const char *lhx_usb_action_label(int n) { return n>=0 && n<USB_ACTIONS?labels[n]:"Unknown action"; }
const char *lhx_usb_behavior_name(int n) { return n>=0 && n<USB_BEHAVIORS?behaviors[n]:NULL; }
static int named(const char *s,const char *const *list,int n) {
  for (int i=0;i<n;++i) if (!strcmp(s,list[i])) return i;
  return -1;
}
int lhx_usb_hex(char *out,size_t size,const char *in) {
  static const char hex[]="0123456789abcdef";
  size_t n=in?strlen(in):0;
  if (2*n>=size) { *out=0; return 0; }
  for (size_t i=0;i<n;++i) { unsigned v=(unsigned char)in[i];out[2*i]=hex[v>>4];out[2*i+1]=hex[v&15]; }
  out[2*n]=0;return 1;
}
void lhx_usb_unhex(char *out,size_t size,const char *in) {
  size_t n=strlen(in)/2;
  if (n>=size) n=size-1;
  for (size_t i=0;i<n;++i) { char pair[]={in[2*i],in[2*i+1],0};out[i]=(char)strtol(pair,NULL,16); }
  out[n]=0;
}
static int hex_valid(const char *s,int length,int size) {
  size_t n=strlen(s);
  if (!n || n>=(size_t)size || n%2 || (length && n!=(size_t)length)) return 0;
  for (size_t i=0;i<n;++i) if (!isxdigit((unsigned char)s[i])) return 0;
  return 1;
}
int lhx_calibration_valid(const LhxCalibration *c) {
  return (c->set==0 || c->set==1) && (c->lever==0 || c->lever==1) &&
    (c->invert==0 || c->invert==1) && c->minimum>=-32768 && c->minimum<=32767 && c->center>=-32768 && c->center<=32767 &&
    c->maximum>=-32768 && c->maximum<=32767 &&
    c->maximum-c->minimum>=4096 && c->deadzone>=0 && c->deadzone<=30000 &&
    (c->lever || (c->center-c->minimum>=2048 && c->maximum-c->center>=2048 &&
      c->deadzone<c->center-c->minimum && c->deadzone<c->maximum-c->center));
}
int lhx_calibrated(const LhxCalibration *saved,int raw,int lever) {
  LhxCalibration factory={1,0,-32768,0,32767,0,6000};
  const LhxCalibration *c=saved->set?saved:&factory;
  if (!lhx_calibration_valid(c)) return 0;
  if (c->lever) lever=1;
  if (raw<c->minimum) raw=c->minimum;
  if (raw>c->maximum) raw=c->maximum;
  int value;
  if (lever) value=(int)((long long)(raw-c->minimum)*32767/(c->maximum-c->minimum));
  else if (abs(raw-c->center)<=c->deadzone) value=0;
  else if (raw>c->center) value=(int)((long long)(raw-c->center-c->deadzone)*32767/(c->maximum-c->center-c->deadzone));
  else value=(int)((long long)(raw-c->center+c->deadzone)*32767/(c->center-c->minimum-c->deadzone));
  return c->invert?(lever?32767-value:-value):value;
}
int lhx_usb_behavior_allowed(const LhxUsbBinding *b,int v) {
  if (b->action<0 || b->action>=USB_ACTIONS) return 0;
  if (b->action<=USB_RUDDER) return b->axis?(v==USB_ANALOG || v==USB_POSITIVE || v==USB_NEGATIVE):(v==USB_POSITIVE || v==USB_NEGATIVE);
  if (b->action==USB_THROTTLE) return (b->axis && (v==USB_DIRECT || v==USB_RELATIVE)) || v==USB_INC || v==USB_DEC || v==USB_PRESET;
  return v==USB_HOLD || ((b->action==USB_HALF || b->action==USB_FULL) && v==USB_LEGACY);
}
int lhx_usb_profile_valid(const LhxUsbProfile *p) {
  if (p->version!=0 && p->version!=2) return 0;
  if (p->devices<0 || p->devices>LHX_USB_DEVICES || p->bindings<0 || p->bindings>LHX_USB_BINDINGS) return 0;
  if (!p->version) return !p->devices && !p->bindings;
  for (int i=0;i<p->devices;++i) {
    const LhxUsbDevice *d=&p->device[i];
    if (!hex_valid(d->guid,32,sizeof d->guid) || (*d->serial && !hex_valid(d->serial,0,sizeof d->serial)) ||
        (*d->name && !hex_valid(d->name,0,sizeof d->name)) || (d->enabled!=0 && d->enabled!=1) || (d->mapped!=0 && d->mapped!=1) || (d->session_only!=0 && d->session_only!=1)) return 0;
    for (int a=0;a<LHX_USB_AXES;++a) if (d->calibration[a].set && !lhx_calibration_valid(&d->calibration[a])) return 0;
  }
  for (int i=0;i<p->bindings;++i) {
    const LhxUsbBinding *b=&p->binding[i];
    if (b->device<0 || b->device>=p->devices || (b->axis!=0 && b->axis!=1) || b->input<0 || b->input>=(b->axis?32:80) ||
        (b->direction!=1 && b->direction!=-1) || b->percent<0 || b->percent>100 || b->threshold<0 || b->threshold>32766 ||
        !lhx_usb_behavior_allowed(b,b->behavior)) return 0;
    if (p->device[b->device].mapped && b->input>=(b->axis?SDL_CONTROLLER_AXIS_MAX:SDL_CONTROLLER_BUTTON_MAX)) return 0;
    const LhxCalibration *c=&p->device[b->device].calibration[b->axis?b->input:0];
    if (b->axis && c->set && c->lever && (b->behavior==USB_ANALOG || b->behavior==USB_RELATIVE)) return 0;
  }
  return 1;
}
static int integer(const char *s,int lo,int hi,int *out) {
  if (!*s || (*s!='-' && !isdigit((unsigned char)*s))) return 0;
  char *end;long n=strtol(s,&end,10);
  if (*end || n<lo || n>hi) return 0;
  *out=(int)n;return 1;
}
static int slot(const char *s,const char *prefix,int max) {
  size_t n=strlen(prefix);int v;
  return !strncmp(s,prefix,n) && integer(s+n,1,max,&v)?v-1:-1;
}
int lhx_usb_profile_section(const char *s) {
  return !strcmp(s,"usb_controls") || slot(s,"usb_device",LHX_USB_DEVICES)>=0 || slot(s,"usb_calibration",LHX_USB_DEVICES)>=0;
}
int lhx_usb_profile_parse(LhxUsbProfile *p,const char *section,const char *key,const char *value) {
  if (!strcmp(section,"usb_controls")) {
    if (!strcmp(key,"version")) return integer(value,2,2,&p->version);
    if (!strcmp(key,"devices")) return integer(value,0,LHX_USB_DEVICES,&p->devices);
    if (!strcmp(key,"bindings")) return integer(value,0,LHX_USB_BINDINGS,&p->bindings);
    int i=slot(key,"binding",LHX_USB_BINDINGS);if (i<0) return 0;
    char copy[480]; if (strlen(value)>=sizeof copy) return 0;strcpy(copy,value);
    char *part[8],*cursor=copy;
    for (int n=0;n<8;++n) { part[n]=cursor;char *comma=strchr(cursor,',');if ((n<7)!=!!comma) return 0;if (comma) { *comma=0;cursor=comma+1; } }
    LhxUsbBinding *b=&p->binding[i];
    b->device=slot(part[0],"device",LHX_USB_DEVICES);
    b->axis=!strncmp(part[1],"axis",4);
    b->input=slot(part[1],b->axis?"axis":"button",b->axis?32:80);
    b->action=named(part[2],names,USB_ACTIONS);b->behavior=named(part[3],behaviors,USB_BEHAVIORS);
    return b->device>=0 && b->input>=0 && integer(part[4],-1,1,&b->direction) && b->direction &&
      integer(part[5],0,100,&b->percent) && integer(part[6],0,32766,&b->threshold) && !strcmp(part[7],"v2");
  }
  int i=slot(section,"usb_device",LHX_USB_DEVICES);
  if (i>=0) {
    LhxUsbDevice *d=&p->device[i];
    if (!strcmp(key,"guid")) { if (!hex_valid(value,32,sizeof d->guid)) return 0;strcpy(d->guid,value);for (char *q=d->guid;*q;++q) *q=(char)tolower((unsigned char)*q);return 1; }
    if (!strcmp(key,"serial") || !strcmp(key,"name")) {
      char *out=!strcmp(key,"serial")?d->serial:d->name;
      int size=!strcmp(key,"serial")?sizeof d->serial:sizeof d->name;
      if (!strcmp(value,"none")) { *out=0;return 1; }
      if (!hex_valid(value,0,size)) return 0;
      strcpy(out,value);for (char *q=out;*q;++q) *q=(char)tolower((unsigned char)*q);return 1;
    }
    if (!strcmp(key,"enabled")) return integer(value,0,1,&d->enabled);
    if (!strcmp(key,"mapped")) return integer(value,0,1,&d->mapped);
    if (!strcmp(key,"session_only")) return integer(value,0,1,&d->session_only);
    return 0;
  }
  i=slot(section,"usb_calibration",LHX_USB_DEVICES);
  int a=slot(key,"axis",32);if (i<0 || a<0) return 0;
  LhxCalibration *c=&p->device[i].calibration[a];
  char copy[200];if (strlen(value)>=sizeof copy) return 0;strcpy(copy,value);
  char *parts[6],*cursor=copy;
  for (int n=0;n<6;++n) { parts[n]=cursor;char *comma=strchr(cursor,',');if ((n<5)!=!!comma) return 0;if (comma) { *comma=0;cursor=comma+1; } }
  if (strcmp(parts[0],"centered") && strcmp(parts[0],"lever")) return 0;
  if (!integer(parts[1],-32768,32767,&c->minimum) || !integer(parts[2],-32768,32767,&c->center) ||
      !integer(parts[3],-32768,32767,&c->maximum) || !integer(parts[4],0,1,&c->invert) || !integer(parts[5],0,30000,&c->deadzone)) return 0;
  c->lever=!strcmp(parts[0],"lever");c->set=1;return lhx_calibration_valid(c);
}
static void key(LhxUsbIniKey *k,const char *section,const char *name,const char *value) {
  snprintf(k->section,sizeof k->section,"%s",section);snprintf(k->key,sizeof k->key,"%s",name);snprintf(k->value,sizeof k->value,"%s",value);
}
int lhx_usb_profile_keys(const LhxUsbProfile *p,LhxUsbIniKey *keys,int capacity) {
  if (!p->version) return 0;
  int count=3+p->devices*6+p->bindings;
  for (int d=0;d<p->devices;++d) for (int a=0;a<32;++a) count+=p->device[d].calibration[a].set;
  if (capacity<count || !lhx_usb_profile_valid(p)) return -1;
  int n=0;char v[480],s[40],k[40];
  key(&keys[n++],"usb_controls","version","2");snprintf(v,sizeof v,"%d",p->devices);key(&keys[n++],"usb_controls","devices",v);
  snprintf(v,sizeof v,"%d",p->bindings);key(&keys[n++],"usb_controls","bindings",v);
  for (int d=0;d<p->devices;++d) {
    const LhxUsbDevice *dev=&p->device[d];snprintf(s,sizeof s,"usb_device%d",d+1);
    key(&keys[n++],s,"guid",dev->guid);key(&keys[n++],s,"serial",*dev->serial?dev->serial:"none");
    key(&keys[n++],s,"name",*dev->name?dev->name:"none");key(&keys[n++],s,"enabled",dev->enabled?"1":"0");key(&keys[n++],s,"mapped",dev->mapped?"1":"0");key(&keys[n++],s,"session_only",dev->session_only?"1":"0");
    snprintf(s,sizeof s,"usb_calibration%d",d+1);
    for (int a=0;a<32;++a) if (dev->calibration[a].set) {
      const LhxCalibration *c=&dev->calibration[a];snprintf(k,sizeof k,"axis%d",a+1);
      snprintf(v,sizeof v,"%s,%d,%d,%d,%d,%d",c->lever?"lever":"centered",c->minimum,c->center,c->maximum,c->invert,c->deadzone);key(&keys[n++],s,k,v);
    }
  }
  for (int i=0;i<p->bindings;++i) {
    const LhxUsbBinding *b=&p->binding[i];snprintf(k,sizeof k,"binding%d",i+1);
    snprintf(v,sizeof v,"device%d,%s%d,%s,%s,%d,%d,%d,v2",b->device+1,b->axis?"axis":"button",b->input+1,names[b->action],behaviors[b->behavior],b->direction,b->percent,b->threshold);
    key(&keys[n++],"usb_controls",k,v);
  }
  return n;
}
