#define _POSIX_C_SOURCE 200809L
#include "controls.h"
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *actions[LHX_ACTION_COUNT] = {
  "none", "confirm", "cancel", "fire", "full_collective", "hover",
  "previous_weapon", "next_weapon", "cockpit", "chase", "toggle_view", "pause",
  "gear", "menu_up", "menu_down", "menu_left", "menu_right",
  "collective_down", "collective_up", "chaff", "flares"
};
static const char *axis_actions[] = {
  "none", "roll", "pitch", "rudder", "collective", "fire", "hover", "chaff", "flares"
};

const char *lhx_control_action_name(LhxControlAction action) {
  return action >= 0 && action < LHX_ACTION_COUNT ? actions[action] : NULL;
}
const char *lhx_axis_action_name(LhxAxisAction action) {
  return action >= LHX_AXIS_NONE && action <= LHX_AXIS_FLARES ? axis_actions[action] : NULL;
}

static void hex_string(char *out, size_t size, const char *in) {
  static const char hex[] = "0123456789abcdef";
  size_t n = in ? strlen(in) : 0;
  /* Never store a truncated identity that could match another device. */
  if (n * 2 >= size) n = 0;
  for (size_t i = 0; i < n; ++i) {
    unsigned c = (unsigned char)in[i];
    out[2*i] = hex[c >> 4]; out[2*i+1] = hex[c & 15];
  }
  out[n*2] = 0;
}

void lhx_controls_select_joystick(LhxControls *c, SDL_Joystick *joy) {
  c->controller_guid[0] = c->controller_serial[0] = 0;
  if (!joy) return;
  SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy), c->controller_guid, sizeof c->controller_guid);
#if SDL_VERSION_ATLEAST(2, 0, 14)
  hex_string(c->controller_serial, sizeof c->controller_serial, SDL_JoystickGetSerial(joy));
#endif
}

void lhx_controls_select_device(LhxControls *c, SDL_GameController *pad) {
  lhx_controls_select_joystick(c,pad?SDL_GameControllerGetJoystick(pad):NULL);
}
int lhx_controls_device_matches(const LhxControls *c, SDL_GameController *pad) {
  return lhx_controls_joystick_matches(c,pad?SDL_GameControllerGetJoystick(pad):NULL);
}
int lhx_controls_joystick_matches(const LhxControls *c, SDL_Joystick *joy) {
  if (!*c->controller_guid) return 1;
  LhxControls identity = {0};
  lhx_controls_select_joystick(&identity, joy);
  return !strcmp(c->controller_guid, identity.controller_guid) &&
    (!*c->controller_serial || !strcmp(c->controller_serial, identity.controller_serial));
}

static int controller_setting(LhxControls *c, const char *key, const char *value) {
  if (!strcmp(key,"input_mode")) {
    if (!strcasecmp(value,"auto")) c->input_mode=LHX_INPUT_AUTO;
    else if (!strcasecmp(value,"gamepad")) c->input_mode=LHX_INPUT_GAMEPAD;
    else if (!strcasecmp(value,"joystick")) c->input_mode=LHX_INPUT_JOYSTICK;
    else return 0;
    return 1;
  }
  if (!strcmp(key, "show_setup")) {
    if (strcasecmp(value,"true") && strcasecmp(value,"false")) return 0;
    c->show_controller_setup = !strcasecmp(value,"true");
    return 1;
  }
  char *target; size_t size;
  if (!strcmp(key,"guid")) { target=c->controller_guid; size=sizeof c->controller_guid; }
  else if (!strcmp(key,"serial")) { target=c->controller_serial; size=sizeof c->controller_serial; }
  else return 0;
  if (!strcasecmp(value,"any")) { *target=0; return 1; }
  size_t n=strlen(value);
  if (!n || n>=size || n%2 || (size==33 && n!=32)) return 0;
  for (size_t i=0;i<n;++i) {
    if (!isxdigit((unsigned char)value[i])) return 0;
    target[i]=(char)tolower((unsigned char)value[i]);
  }
  target[n]=0;
  return 1;
}

/* Each managed key is replaced wherever it occurs, including duplicate sections.
 * Missing keys are appended in small controller-only sections. This retains
 * unrelated bytes, indentation, inline comments, and the file's newline style. */
typedef struct { const char *section; char key[48], value[520]; int seen; } SaveKey;
static char *trim(char *text);

static int same_file(const struct stat *a, const struct stat *b) {
  return a->st_dev==b->st_dev && a->st_ino==b->st_ino && a->st_size==b->st_size &&
    a->st_mtim.tv_sec==b->st_mtim.tv_sec && a->st_mtim.tv_nsec==b->st_mtim.tv_nsec;
}

int lhx_controls_save(const LhxControls *c, const char *path, char *error, size_t error_size) {
  SaveKey keys[960]={0};
  LhxUsbIniKey usb_keys[800];
  int usb_count=lhx_usb_profile_keys(&c->usb,usb_keys,800);
  if (!lhx_usb_profile_valid(&c->usb) || usb_count<0) {
    if (error_size) snprintf(error,error_size,"Invalid USB profile or profile limit exceeded");
    return 0;
  }
  unsigned count=0;
  const char *why="Cannot save controller settings";
  int saved_errno=0, exists=0, good=0;
  struct stat original, current;
  FILE *input=NULL, *output=NULL;
  char *temporary=NULL;
  if (error_size) *error=0;
  if (lstat(path,&original)==0) {
    exists=1;
    if (!S_ISREG(original.st_mode)) { why="Controls path must be a regular file (no symlinks)"; goto done; }
  } else if (errno!=ENOENT) { saved_errno=errno; goto done; }
  LhxControls checked;
  if (!lhx_controls_load(&checked,path,exists)) { why="Existing controls file is invalid or unreadable; left unchanged"; goto done; }
  if (!c->usb.version) {
  for (int i=0;i<SDL_CONTROLLER_BUTTON_MAX;++i) {
    const char *name=SDL_GameControllerGetStringForButton((SDL_GameControllerButton)i);
    const char *action=lhx_control_action_name(c->buttons[i]);
    if (!name || !action) { why="Invalid button binding"; goto done; }
    keys[count].section="buttons";
    snprintf(keys[count].key,sizeof keys[count].key,"%s",name);
    snprintf(keys[count++].value,sizeof keys[0].value,"%s",action);
  }
  for (int i=0;i<SDL_CONTROLLER_AXIS_MAX;++i) {
    const char *name=SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)i);
    const char *action=lhx_axis_action_name(c->axes[i].action);
    if (!name || !action || (c->axes[i].direction!=1 && c->axes[i].direction!=-1)) {
      why="Invalid axis binding"; goto done;
    }
    keys[count].section="axes";
    snprintf(keys[count].key,sizeof keys[count].key,"%s",name);
    snprintf(keys[count++].value,sizeof keys[0].value,"%s%s",c->axes[i].direction<0?"-":"",action);
  }
  for (int i=0;i<LHX_INPUT_BUTTONS;++i) {
    const char *action=lhx_control_action_name(c->raw_buttons[i]);
    if (!action) { why="Invalid raw button binding"; goto done; }
    keys[count].section="joystick_buttons";
    keys[count].seen=c->raw_buttons[i]==LHX_NONE; /* Omit missing, empty raw slots. */
    if (i<LHX_RAW_BUTTONS) snprintf(keys[count].key,sizeof keys[count].key,"button%d",i+1);
    else {
      const char *directions[]={"up","right","down","left"};
      snprintf(keys[count].key,sizeof keys[count].key,"hat%d_%s",(i-LHX_RAW_BUTTONS)/4+1,directions[(i-LHX_RAW_BUTTONS)%4]);
    }
    snprintf(keys[count++].value,sizeof keys[0].value,"%s",action);
  }
  for (int i=0;i<LHX_RAW_AXES;++i) {
    const char *action=lhx_axis_action_name(c->raw_axes[i].action);
    if (!action || (c->raw_axes[i].direction!=1 && c->raw_axes[i].direction!=-1)) {
      why="Invalid raw axis binding"; goto done;
    }
    keys[count].section="joystick_axes";
    keys[count].seen=c->raw_axes[i].action==LHX_AXIS_NONE && c->raw_axes[i].direction==1;
    snprintf(keys[count].key,sizeof keys[count].key,"axis%d",i+1);
    snprintf(keys[count++].value,sizeof keys[0].value,"%s%s",c->raw_axes[i].direction<0?"-":"",action);
  }
  const char *setting_names[]={"deadzone","trigger_threshold","axis_button_threshold",
    "repeat_delay_ms","repeat_interval_ms","throttle_repeat_ms","fire_repeat_ms"};
  const unsigned values[]={c->deadzone,c->trigger_threshold,c->axis_button_threshold,
    c->repeat_delay_ms,c->repeat_interval_ms,c->throttle_repeat_ms,c->fire_repeat_ms};
  for (unsigned i=0;i<7;++i) {
    keys[count].section="settings";
    snprintf(keys[count].key,sizeof keys[count].key,"%s",setting_names[i]);
    snprintf(keys[count++].value,sizeof keys[0].value,"%u",values[i]);
  }
  const char *device_names[]={"guid","serial","show_setup","input_mode"};
  const char *modes[]={"auto","gamepad","joystick"};
  if (c->input_mode<LHX_INPUT_AUTO || c->input_mode>LHX_INPUT_JOYSTICK) { why="Invalid input mode"; goto done; }
  const char *device_values[]={c->controller_guid,c->controller_serial,
    c->show_controller_setup?"true":"false",modes[c->input_mode]};
  for (unsigned i=0;i<4;++i) {
    keys[count].section="controller";
    snprintf(keys[count].key,sizeof keys[count].key,"%s",device_names[i]);
    snprintf(keys[count++].value,sizeof keys[0].value,"%s",*device_values[i]?device_values[i]:"any");
  }
  }
  for (int i=0;i<usb_count;++i) {
    keys[count].section=usb_keys[i].section;
    snprintf(keys[count].key,sizeof keys[count].key,"%s",usb_keys[i].key);
    snprintf(keys[count++].value,sizeof keys[0].value,"%s",usb_keys[i].value);
  }
  temporary=malloc(strlen(path)+16);
  if (!temporary) { saved_errno=ENOMEM; goto done; }
  sprintf(temporary,"%s.tmp.XXXXXX",path);
  int fd=mkstemp(temporary);
  if (fd<0) { saved_errno=errno; goto done; }
  if (exists && fchmod(fd,original.st_mode & 0777)) {
    saved_errno=errno; close(fd); goto done;
  }
  output=fdopen(fd,"w");
  if (!output) { saved_errno=errno; close(fd); goto done; }
  if (exists) {
    input=fopen(path,"r");
    if (!input) { saved_errno=errno; goto done; }
  }
  char line[512], parsed[512], section[64]="";
  const char *newline="\n";
  int last_newline=1;
  while (input && fgets(line,sizeof line,input)) {
    if (strstr(line,"\r\n")) newline="\r\n";
    strcpy(parsed,line);
    char *comment=strpbrk(parsed,"#;"); if (comment) *comment=0;
    char *key=trim(parsed), *equal=strchr(key,'=');
    if (*key=='[') {
      snprintf(section,sizeof section,"%s",key+1);
      char *end=strchr(section,']'); if (end) *end=0;
    }
    SaveKey *replacement=NULL;
    if (equal) {
      *equal=0; key=trim(key);
      for (unsigned i=0;i<count;++i)
        if (!strcasecmp(section,keys[i].section) && !strcasecmp(key,keys[i].key)) {
          replacement=&keys[i]; break;
        }
    }
    if (replacement) {
      /* Keep everything outside the value itself, including an empty value's
       * surrounding space. Parsing above has already validated the source. */
      char *begin=strchr(line,'=')+1;
      while (*begin==' ' || *begin=='\t') ++begin;
      char *end=strpbrk(begin,"#;\r\n"); if (!end) end=begin+strlen(begin);
      while (end>begin && (end[-1]==' ' || end[-1]=='\t')) --end;
      fwrite(line,1,(size_t)(begin-line),output);
      fputs(replacement->value,output); fputs(end,output);
      replacement->seen=1;
    } else if (!(c->usb.version && equal && !strncasecmp(section,"usb_",4))) fputs(line,output);
    last_newline=strchr(line,'\n')!=NULL;
  }
  if (input && ferror(input)) { saved_errno=errno?errno:EIO; goto done; }
  if (!last_newline) fputs(newline,output);
  const char *appended="";
  for (unsigned i=0;i<count;++i) if (!keys[i].seen) {
    if (strcmp(appended,keys[i].section)) {
      fprintf(output,"%s[%s]%s",newline,keys[i].section,newline); appended=keys[i].section;
    }
    fprintf(output,"%s = %s%s",keys[i].key,keys[i].value,newline);
  }
  if (fflush(output) || ferror(output) || fsync(fileno(output))) {
    saved_errno=errno?errno:EIO; goto done;
  }
  if (fclose(output)) { output=NULL; saved_errno=errno; goto done; }
  output=NULL;
  if (!lhx_controls_load(&checked,temporary,1)) { why="Invalid controller settings; original left unchanged"; goto done; }
  if ((exists && (lstat(path,&current) || !same_file(&original,&current))) ||
      (!exists && (lstat(path,&current)==0 || errno!=ENOENT))) {
    why="Controls file changed while saving; retry"; goto done;
  }
  if (rename(temporary,path)) { saved_errno=errno; goto done; }
  good=1;
done:
  if (input) fclose(input);
  if (output) fclose(output);
  if (temporary) { if (!good) unlink(temporary); free(temporary); }
  if (!good && error_size) snprintf(error,error_size,"%s%s%s",why,saved_errno?": ":"",saved_errno?strerror(saved_errno):"");
  return good;
}

void lhx_controls_defaults(LhxControls *controls) {
  *controls = (LhxControls){
    .buttons = {
      [SDL_CONTROLLER_BUTTON_A] = LHX_CONFIRM,
      [SDL_CONTROLLER_BUTTON_B] = LHX_VIEW,
      [SDL_CONTROLLER_BUTTON_X] = LHX_CHAFF,
      [SDL_CONTROLLER_BUTTON_Y] = LHX_FLARES,
      [SDL_CONTROLLER_BUTTON_BACK] = LHX_CANCEL,
      [SDL_CONTROLLER_BUTTON_START] = LHX_PAUSE,
      [SDL_CONTROLLER_BUTTON_LEFTSTICK] = LHX_GEAR,
      [SDL_CONTROLLER_BUTTON_RIGHTSTICK] = LHX_NEXT,
      [SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = LHX_DECREASE,
      [SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = LHX_INCREASE,
      [SDL_CONTROLLER_BUTTON_DPAD_UP] = LHX_UP,
      [SDL_CONTROLLER_BUTTON_DPAD_DOWN] = LHX_DOWN,
      [SDL_CONTROLLER_BUTTON_DPAD_LEFT] = LHX_LEFT,
      [SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = LHX_RIGHT
    },
    .axes = {
      [SDL_CONTROLLER_AXIS_LEFTX] = {LHX_ROLL, 1},
      [SDL_CONTROLLER_AXIS_LEFTY] = {LHX_PITCH, 1},
      [SDL_CONTROLLER_AXIS_RIGHTX] = {LHX_RUDDER, 1},
      [SDL_CONTROLLER_AXIS_RIGHTY] = {LHX_COLLECTIVE, 1},
      [SDL_CONTROLLER_AXIS_TRIGGERLEFT] = {LHX_AXIS_HOVER, 1},
      [SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = {LHX_AXIS_FIRE, 1}
    },
    .deadzone = 6000, .trigger_threshold = 16000, .axis_button_threshold = 12000,
    .repeat_delay_ms = 350, .repeat_interval_ms = 120,
    .throttle_repeat_ms = 200, .fire_repeat_ms = 100,
    .rendering = {.extended_visibility = 1,.terrain_dir="bin/enhanced/terrain",
                  .models_dir="bin/enhanced/models",.sky_dir="bin/enhanced/sky"}
  };
  for (int i=0;i<LHX_RAW_AXES;++i) controls->raw_axes[i].direction=1;
}

static char *trim(char *text) {
  while (isspace((unsigned char)*text))
    text++;
  char *end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1]))
    *--end = 0;
  return text;
}

static int named(const char *name, const char *const *names, unsigned count) {
  for (unsigned i = 0; i < count; ++i)
    if (!strcasecmp(name, names[i]))
      return (int)i;
  return -1;
}

static int raw_index(const char *key,int axis) {
  const char *prefix=axis?"axis":"button";
  size_t n=strlen(prefix);
  if (!strncmp(key,prefix,n) && key[n]>='1' && key[n]<='9') {
    char *end; long value=strtol(key+n,&end,10);
    if (!*end && value<=(axis?LHX_RAW_AXES:LHX_RAW_BUTTONS)) return (int)value-1;
  }
  if (!axis) {
    const char *dirs[]={"up","right","down","left"};
    char name[32];
    for (int h=0;h<LHX_RAW_HATS;++h) for (int d=0;d<4;++d) {
      snprintf(name,sizeof name,"hat%d_%s",h+1,dirs[d]);
      if (!strcmp(key,name)) return LHX_RAW_BUTTONS+h*4+d;
    }
  }
  return -1;
}

static int setting(LhxControls *controls, const char *key, const char *value) {
  static const struct {
    const char *name;
    size_t offset;
    unsigned minimum, maximum;
  } settings[] = {
    {"deadzone", offsetof(LhxControls, deadzone), 0, 30000},
    {"trigger_threshold", offsetof(LhxControls, trigger_threshold), 0, 32766},
    {"axis_button_threshold", offsetof(LhxControls, axis_button_threshold), 0, 32766},
    {"repeat_delay_ms", offsetof(LhxControls, repeat_delay_ms), 0, 2000},
    {"repeat_interval_ms", offsetof(LhxControls, repeat_interval_ms), 30, 2000},
    {"throttle_repeat_ms", offsetof(LhxControls, throttle_repeat_ms), 30, 2000},
    {"fire_repeat_ms", offsetof(LhxControls, fire_repeat_ms), 30, 2000}
  };
  for (unsigned i = 0; i < sizeof(settings) / sizeof(*settings); ++i) {
    if (strcasecmp(key, settings[i].name))
      continue;
    char *end;
    errno = 0;
    unsigned long number = strtoul(value, &end, 10);
    if (errno || !*value || *end || number < settings[i].minimum ||
        number > settings[i].maximum)
      return 0;
    *(unsigned *)((char *)controls + settings[i].offset) = (unsigned)number;
    return 1;
  }
  return 0;
}

static int rendering(LhxRendering *options, const char *key, const char *value) {
  if (!strcmp(key, "renderer")) {
    const char *const names[] = {"native", "original"};
    int mode = named(value, names, 2);
    if (mode < 0) return 0;
    options->renderer = mode == 0 ? LHX_RENDER_NATIVE : LHX_RENDER_ORIGINAL;
  } else if (!strcmp(key,"backend")) {
    const char *const names[]={"cpu","gpu"};
    int mode=named(value,names,2);
    if (mode<0) return 0;
    options->gpu=mode;
  } else if (!strcmp(key, "render_scale")) {
    char *end;
    errno = 0;
    unsigned long scale = strtoul(value, &end, 10);
    if (errno || !*value || *end || scale < 1 || scale > 8) return 0;
    options->render_scale = (unsigned)scale;
  } else if (!strcmp(key, "visibility")) {
    const char *const names[] = {"original", "extended"};
    int mode = named(value, names, 2);
    if (mode < 0) return 0;
    options->extended_visibility = mode;
  } else if (!strcmp(key,"sky_dir")) {
    if (!*value || strlen(value)>=sizeof options->sky_dir) return 0;
    strcpy(options->sky_dir,value);
  } else if (!strcmp(key,"models_dir")) {
    if (!*value || strlen(value)>=sizeof options->models_dir) return 0;
    strcpy(options->models_dir,value);
  } else if (!strcmp(key,"terrain_dir")) {
    if (!*value || strlen(value)>=sizeof options->terrain_dir) return 0;
    strcpy(options->terrain_dir,value);
  } else if (!strcmp(key, "show_fps") || !strcmp(key,"lighting") ||
             !strcmp(key,"runway_textures") || !strcmp(key,"terrain_textures") ||
             !strcmp(key,"antialiasing") || !strcmp(key,"sphere_depth") ||
             !strcmp(key,"hide_ground_dots") || !strcmp(key,"smooth_distant_aircraft") ||
             !strcmp(key,"atmosphere") || !strcmp(key,"replacement_models") ||
             !strcmp(key,"photo_skies") || !strcmp(key,"terrain_relief") ||
             !strcmp(key,"shadows") || !strcmp(key,"military_paint") ||
             !strcmp(key,"surface_detail") || !strcmp(key,"rotor_blades") || !strcmp(key,"runway_detail")) {
    const char *const names[] = {"false", "true"};
    int mode = named(value, names, 2);
    if (mode < 0) return 0;
    if (!strcmp(key,"show_fps")) options->show_fps=mode;
    else if (!strcmp(key,"lighting")) options->lighting=mode;
    else if (!strcmp(key,"runway_textures")) options->runway_textures=mode;
    else if (!strcmp(key,"terrain_textures")) options->terrain_textures=mode;
    else if (!strcmp(key,"antialiasing")) options->antialiasing=mode;
    else if (!strcmp(key,"sphere_depth")) options->sphere_depth=mode;
    else if (!strcmp(key,"hide_ground_dots")) options->hide_ground_dots=mode;
    else if (!strcmp(key,"smooth_distant_aircraft")) options->smooth_distant_aircraft=mode;
    else if (!strcmp(key,"atmosphere")) options->atmosphere=mode;
    else if (!strcmp(key,"replacement_models")) options->replacement_models=mode;
    else if (!strcmp(key,"photo_skies")) options->photo_skies=mode;
    else if (!strcmp(key,"terrain_relief")) options->terrain_relief=mode;
    else if (!strcmp(key,"shadows")) options->shadows=mode;
    else if (!strcmp(key,"military_paint")) options->military_paint=mode;
    else if (!strcmp(key,"surface_detail")) options->surface_detail=mode;
    else if (!strcmp(key,"rotor_blades")) options->rotor_blades=mode;
    else options->runway_detail=mode;
  } else return 0;
  return 1;
}

int lhx_controls_load(LhxControls *controls, const char *path, int required) {
  LhxControls candidate;
  lhx_controls_defaults(&candidate);
  FILE *file = fopen(path, "r");
  if (!file) {
    if (!required && errno == ENOENT) {
      *controls = candidate;
      return 1;
    }
    fprintf(stderr, "[controls] %s: %s\n", path, strerror(errno));
    return 0;
  }
  enum { UNSET, BUTTONS, AXES, SETTINGS, AUDIO, RENDERING, CONTROLLER, RAW_BUTTONS, RAW_AXES, USB } section = UNSET;
  char line[512], usb_section[64]="";
  LhxUsbIniKey usb_seen[800], usb_expected[800];
  int usb_seen_count=0,usb_sections=0;
  for (int i=0;i<LHX_USB_DEVICES;++i) candidate.usb.device[i].occurrence=-1;
  unsigned line_number = 0;
  const char *error = NULL;
  while (fgets(line, sizeof(line), file)) {
    line_number++;
    if (!strchr(line, '\n') && !feof(file)) {
      error = "line too long";
      break;
    }
    char *comment = strpbrk(line, "#;");
    if (comment)
      *comment = 0;
    char *key = trim(line);
    if (!*key)
      continue;
    if (*key == '[') {
      if (!strcasecmp(key, "[buttons]")) section = BUTTONS;
      else if (!strcasecmp(key, "[axes]")) section = AXES;
      else if (!strcasecmp(key, "[settings]")) section = SETTINGS;
      else if (!strcasecmp(key, "[audio]")) section = AUDIO;
      else if (!strcasecmp(key, "[rendering]")) section = RENDERING;
      else if (!strcasecmp(key, "[controller]")) section = CONTROLLER;
      else if (!strcasecmp(key,"[joystick_buttons]")) section=RAW_BUTTONS;
      else if (!strcasecmp(key,"[joystick_axes]")) section=RAW_AXES;
      else if (!strncasecmp(key,"[usb_",5) && strlen(key)<sizeof usb_section && key[strlen(key)-1]==']') {
        strcpy(usb_section,key+1);usb_section[strlen(usb_section)-1]=0;
        for (char *p=usb_section;*p;++p) *p=(char)tolower((unsigned char)*p);
        if (!lhx_usb_profile_section(usb_section)) { error="Unknown USB profile section";break; }
        section=USB;++usb_sections;
      }
      else { error = "unknown section"; break; }
      continue;
    }
    char *value = strchr(key, '=');
    if (!value) { error = "expected input = action"; break; }
    *value++ = 0;
    key = trim(key);
    value = trim(value);
    for (char *p = key; *p; ++p)
      *p = (char)tolower((unsigned char)*p);
    if (section==USB) {
      int duplicate=0;
      for (int i=0;i<usb_seen_count;++i) if (!strcmp(usb_seen[i].section,usb_section) && !strcmp(usb_seen[i].key,key)) duplicate=1;
      if (duplicate || usb_seen_count>=800 || strlen(key)>=40 || !lhx_usb_profile_parse(&candidate.usb,usb_section,key,value)) { error="Invalid, duplicate or out-of-range USB profile field";break; }
      snprintf(usb_seen[usb_seen_count].section,40,"%s",usb_section);
      snprintf(usb_seen[usb_seen_count++].key,40,"%s",key);
    } else if (section==RAW_BUTTONS || section==RAW_AXES) {
      int index=raw_index(key,section==RAW_AXES), direction=*value=='-'?-1:1;
      int action=section==RAW_AXES?named(value+(direction<0),axis_actions,sizeof axis_actions/sizeof *axis_actions):named(value,actions,LHX_ACTION_COUNT);
      if (index<0 || action<0) { error="unknown native joystick input or action"; break; }
      if (section==RAW_AXES) candidate.raw_axes[index]=(LhxAxisBinding){(LhxAxisAction)action,direction};
      else candidate.raw_buttons[index]=(LhxControlAction)action;
    } else if (section == BUTTONS) {
      SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(key);
      int action = named(value, actions, LHX_ACTION_COUNT);
      if (button == SDL_CONTROLLER_BUTTON_INVALID || action < 0) {
        error = "unknown button or action"; break;
      }
      candidate.buttons[button] = (LhxControlAction)action;
    } else if (section == AXES) {
      SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(key);
      int direction = *value == '-' ? -1 : 1;
      int action = named(value + (direction < 0), axis_actions,
                         sizeof(axis_actions) / sizeof(*axis_actions));
      if (axis == SDL_CONTROLLER_AXIS_INVALID || action < 0) {
        error = "unknown axis or action"; break;
      }
      candidate.axes[axis] = (LhxAxisBinding){(LhxAxisAction)action, direction};
    } else if (section == SETTINGS) {
      if (!setting(&candidate, key, value)) {
        error = "unknown setting or value outside its allowed range"; break;
      }
    } else if (section == CONTROLLER) {
      if (!controller_setting(&candidate,key,value)) {
        error = "unknown controller option or invalid value"; break;
      }
    } else if (section == AUDIO) {
      static const char *const values[] = {"false", "true"};
      int value_index = named(value, values, 2);
      if (strcmp(key, "improved_sounds") || value_index < 0) {
        error = "unknown audio option or invalid value"; break;
      }
      candidate.improved_sounds = value_index;
    } else if (section == RENDERING) {
      if (!rendering(&candidate.rendering, key, value)) {
        error = "unknown rendering option or invalid value"; break;
      }
    } else { error = "expected [buttons], [axes], [settings], [audio], or [rendering]"; break; }
  }
  if (!error && ferror(file))
    error = "read failed";
  if (!error && *candidate.controller_serial && !*candidate.controller_guid)
    error = "controller serial requires a GUID";
  if (!error && usb_sections && candidate.usb.version!=2) error="Incomplete USB profile";
  if (!error) {
    int n=lhx_usb_profile_keys(&candidate.usb,usb_expected,800);
    if (!lhx_usb_profile_valid(&candidate.usb) || n!=usb_seen_count) error="Incomplete USB profile";
    for (int i=0;!error && i<n;++i) {
      int found=0;
      for (int j=0;j<usb_seen_count;++j) found |= !strcmp(usb_seen[j].section,usb_expected[i].section) && !strcmp(usb_seen[j].key,usb_expected[i].key);
      if (!found) error="Missing USB profile field or records outside declared counts";
    }
  }
  fclose(file);
  if (error) {
    fprintf(stderr, "[controls] %s:%u: %s\n", path, line_number, error);
    return 0;
  }
  *controls = candidate;
  fprintf(stderr, "[controls] %s\n", path);
  return 1;
}
