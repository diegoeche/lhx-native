#define _POSIX_C_SOURCE 200809L
#include "audio_driver.h"
#include "audio_synth.h"
#include "lifted.h"
#include "mission_module.h"
#include "cockpit_module.h"
#include "model_transform.h"
#include "frame_clock.h"
#include "fps.h"
#include "gamepad.h"
#include "controller_setup.h"
#include "scene_capture.h"
#include "scene_composite.h"
#include "scene_renderer.h"
#include "scene_gpu.h"
#include "scene_props.h"
#include "scene_sky.h"
#include "scene_metrics.h"
#include "scene_assets.h"
#include <SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The guest CPU has already been translated to C. This module supplies file,
 * video, keyboard and timer services; it never decodes guest instructions. */
static CPU machine;
static jmp_buf exit_jmp;
static int exit_ready, result, headless, in_irq, polling;
static int sound_enabled = 1;
static int controller_enabled = 1, list_controllers;
static const char *controls_path = "controls.ini";
static int controls_explicit;
static SDL_AudioDeviceID audio_device;
static atomic_uint audio_divisor = 65536;
static atomic_int audio_gate;
static int audio_rate = 48000;
static int improved_sounds, audio_channels = 1;
static LhxSound enhanced_audio;
static const char *data_dir = "bin/dos", *save_dir = "saves", *shot_path;
static const char *memory_path;
static double limit_seconds;
static uint64_t start_us, virtual_us, next_irq = 65536, next_frame;
static struct {
  uint64_t polls,last_poll,max_service_gap_us,draws,max_draw_us,slow_draws;
  uint64_t repeat_draws,last_serial,timer_resets,max_irq_lateness_us;
} service_metrics;
static atomic_uint_fast64_t audio_callbacks,audio_max_callback_gap;
static uint64_t audio_last_callback;
static uint64_t host_us(void);
static LhxFrameClock flight_clock = {.rate = 60};
static uint64_t flight_frames, first_flight_us, last_flight_us;
static int show_fps;
static LhxFpsCounter fps_counter;
int g_lhx_scene_enabled;
int g_lhx_scene_world_enabled = 1;
static int native_renderer = 1, renderer_selected, scale_selected;
static unsigned render_scale = 3, render_width = 320, render_height = 200;
static const char *render_stats_path;
static LhxSceneRenderOptions render_options;
static LhxGpu *gpu;
static LhxProps *scenery;
static LhxSky *sky;
static int gpu_requested,backend_selected;
static int terrain_textures;
static unsigned video_mode = 3, cursor_x, cursor_y;
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static uint32_t pixels[320 * 200];
static uint32_t *native_pixels, *screen_pixels = pixels;
static uint8_t palette[768], pic_mask = 0xfc, speaker_gate, key_port;
static uint8_t seq[8], gc[16], crtc[32], seq_index, gc_index, crtc_index;
static unsigned dac_read, dac_write, dac_sub;
static FILE *handles[64];
static uint16_t dta_seg, dta_off;
static char found[256][256];
static unsigned find_count, find_pos;
static uint8_t keyq[256];
static unsigned key_head, key_tail;
static uint8_t input_sources[256];
static int extended_key;
int g_recomp_tick_budget = 1;

typedef struct {
  uint32_t divisor;
  uint16_t value, latch;
  uint8_t access, flip, latched;
} PIT;
static PIT pit[3] = {
    {65536, 0, 0, 3, 0, 0}, {65536, 0, 0, 3, 0, 0}, {65536, 0, 0, 3, 0, 0}};
typedef struct {
  double at;
  unsigned key;
  int release, done;
} ScriptKey;
static ScriptKey script[256];
static unsigned script_count;

static void speaker_audio(void *unused, Uint8 *stream, int len) {
  (void)unused;
  if(render_stats_path){
    uint64_t now=host_us(),gap=audio_last_callback?now-audio_last_callback:0;
    audio_last_callback=now;atomic_fetch_add_explicit(&audio_callbacks,1,memory_order_relaxed);
    if(gap>atomic_load_explicit(&audio_max_callback_gap,memory_order_relaxed))
      atomic_store_explicit(&audio_max_callback_gap,gap,memory_order_relaxed);
  }
  if (improved_sounds && atomic_load_explicit(&enhanced_audio.received, memory_order_acquire)) {
    lhx_sound_render(&enhanced_audio, (int16_t *)stream,
                     len / (sizeof(int16_t) * audio_channels), audio_channels);
    return;
  }
  static double phase;
  int16_t *out = (int16_t *)stream;
  unsigned divisor = atomic_load_explicit(&audio_divisor, memory_order_relaxed);
  int gate = atomic_load_explicit(&audio_gate, memory_order_relaxed);
  double step = 1193182.0 / divisor / audio_rate;
  for (int i = 0; i < len / (int)sizeof(*out) / audio_channels; ++i) {
    for (int c = 0; c < audio_channels; ++c)
      out[i*audio_channels+c] = gate ? (phase < .5 ? 1800 : -1800) : 0;
    phase += step;
    if (phase >= 1)
      phase -= (unsigned)phase;
  }
}

static uint64_t host_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}
static uint64_t now_us(void) {
  return headless ? virtual_us : host_us() - start_us;
}
static uint64_t pit_ticks(void) { return now_us() * 1193182 / 1000000; }
static void finish(int code) {
  result = code;
  if (exit_ready)
    longjmp(exit_jmp, 1);
  exit(code);
}
static void fail_api(CPU *c, unsigned code) {
  c->ax = code;
  c->flags |= FLAG_CF;
}
static void ok(CPU *c) { c->flags &= ~FLAG_CF; }
static void flag_zf(CPU *c, int on) {
  c->flags = on ? c->flags | FLAG_ZF : c->flags & ~FLAG_ZF;
}
static void asciiz(CPU *c, uint16_t seg, uint16_t off, char *s, size_t n) {
  size_t i;
  for (i = 0; i + 1 < n; ++i) {
    s[i] = (char)mem_read8(c, seg, (uint16_t)(off + i));
    if (!s[i])
      break;
  }
  s[i] = 0;
}
static const char *basename_dos(const char *s) {
  const char *b = s;
  for (const char *p = s; *p; ++p)
    if (*p == '/' || *p == '\\' || *p == ':')
      b = p + 1;
  return b;
}
static int resolve_file(const char *dir, const char *name, char *out,
                        size_t cap) {
  DIR *d = opendir(dir);
  if (!d)
    return 0;
  struct dirent *e;
  int found_it = 0;
  while ((e = readdir(d)))
    if (!strcasecmp(name, e->d_name)) {
      snprintf(out, cap, "%s/%s", dir, e->d_name);
      found_it = 1;
      break;
    }
  closedir(d);
  if (!found_it)
    errno = ENOENT;
  return found_it;
}
static FILE *open_file(const char *dosname, unsigned mode, int create) {
  const char *name = basename_dos(dosname);
  char path[4096], source[4096];
  if (!*name || !strcmp(name, ".") || !strcmp(name, ".."))
    return NULL;
  if (create) {
    snprintf(path, sizeof path, "%s/%s", save_dir, name);
    return fopen(path, "w+b");
  }
  int saved = resolve_file(save_dir, name, path, sizeof path);
  if (mode == 0) {
    if (saved)
      return fopen(path, "rb");
    return resolve_file(data_dir, name, path, sizeof path) ? fopen(path, "rb")
                                                           : NULL;
  }
  if (!saved) {
    if (!resolve_file(data_dir, name, source, sizeof source))
      return NULL;
    snprintf(path, sizeof path, "%s/%s", save_dir, name);
    FILE *a = fopen(source, "rb"), *b = a ? fopen(path, "wb") : NULL;
    if (!a || !b) {
      if (a)
        fclose(a);
      return NULL;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, a)))
      if (fwrite(buf, 1, n, b) != n)
        break;
    int bad = ferror(a) || ferror(b);
    fclose(a);
    fclose(b);
    if (bad)
      return NULL;
  }
  return fopen(path, "r+b");
}
static int new_handle(FILE *f) {
  for (int i = 5; i < 64; i++)
    if (!handles[i]) {
      handles[i] = f;
      return i;
    }
  fclose(f);
  return -1;
}

/* A DOS arena is retained as data because the game inspects its MCBs. */
static void mcb(CPU *c, uint16_t seg, uint8_t type, uint16_t owner,
                uint16_t size) {
  mem_write8(c, seg, 0, type);
  mem_write16(c, seg, 1, owner);
  mem_write16(c, seg, 3, size);
}
static void coalesce(CPU *c) {
  uint32_t seg = g_psp_seg - 1;
  for (unsigned guard = 0; guard < 4096 && seg < 0xa000; guard++) {
    unsigned size = mem_read16(c, seg, 3), type = mem_read8(c, seg, 0),
             next = seg + size + 1;
    if (type == 0x5a)
      return;
    if (type != 0x4d || next >= 0xa000) {
      fprintf(stderr, "Invalid DOS arena at %04x\n", (unsigned)seg);
      finish(2);
    }
    if (mem_read16(c, seg, 1) == 0 && mem_read16(c, next, 1) == 0) {
      mcb(c, seg, mem_read8(c, next, 0), 0, size + 1 + mem_read16(c, next, 3));
      continue;
    }
    seg = next;
  }
}
static unsigned allocate(CPU *c, unsigned paragraphs, unsigned *largest) {
  coalesce(c);
  *largest = 0;
  for (unsigned seg = g_psp_seg - 1, guard = 0;
       seg < 0xa000 && guard++ < 4096;) {
    unsigned size = mem_read16(c, seg, 3), type = mem_read8(c, seg, 0);
    if (!mem_read16(c, seg, 1)) {
      if (size > *largest)
        *largest = size;
      if (size >= paragraphs) {
        if (size > paragraphs) {
          mcb(c, seg + paragraphs + 1, type, 0, size - paragraphs - 1);
          type = 0x4d;
        }
        mcb(c, seg, type, g_psp_seg, paragraphs);
        return seg + 1;
      }
    }
    if (type == 0x5a)
      break;
    seg += size + 1;
  }
  return 0;
}
static int resize_block(CPU *c, unsigned seg, unsigned size,
                        unsigned *largest) {
  if (seg < g_psp_seg || seg >= 0xa000 ||
      mem_read16(c, seg - 1, 1) != g_psp_seg)
    return 9;
  coalesce(c);
  unsigned m = seg - 1, avail = mem_read16(c, m, 3), type = mem_read8(c, m, 0),
           endtype = type;
  unsigned next = seg + avail;
  if (type != 0x5a && !mem_read16(c, next, 1)) {
    avail += 1 + mem_read16(c, next, 3);
    endtype = mem_read8(c, next, 0);
  }
  *largest = avail;
  if (size > avail)
    return 8;
  if (size < avail) {
    mcb(c, seg + size, endtype, 0, avail - size - 1);
    mcb(c, m, 0x4d, g_psp_seg, size);
  } else
    mcb(c, m, endtype, g_psp_seg, size);
  return 0;
}

static void tty(CPU *c, uint8_t ch) {
  fputc(ch, stdout);
  fflush(stdout);
  if (ch == '\r')
    cursor_x = 0;
  else if (ch == '\n')
    ++cursor_y;
  else if (ch == 8) {
    if (cursor_x)
      --cursor_x;
  } else if (ch >= 32) {
    mem_write16(c, 0xb800, (cursor_y * 80 + cursor_x) * 2, 0x0700 | ch);
    if (++cursor_x == 80) {
      cursor_x = 0;
      cursor_y++;
    }
  }
  if (cursor_y >= 25) {
    memmove(c->mem + 0xb8000, c->mem + 0xb80a0, 3840);
    for (unsigned i = 0; i < 80; i++)
      mem_write16(c, 0xb800, (24 * 80 + i) * 2, 0x0720);
    cursor_y = 24;
  }
}
static int pop_bios_key(CPU *c, int remove) {
  unsigned h = mem_read16(c, 0x40, 0x1a), t = mem_read16(c, 0x40, 0x1c);
  if (h == t)
    return -1;
  unsigned word = mem_read16(c, 0x40, h);
  if (remove)
    mem_write16(c, 0x40, 0x1a, h + 2 >= 0x3e ? 0x1e : h + 2);
  return word;
}
static void push_bios_key(CPU *c, unsigned word) {
  unsigned h = mem_read16(c, 0x40, 0x1a), t = mem_read16(c, 0x40, 0x1c),
           next = t + 2 >= 0x3e ? 0x1e : t + 2;
  if (next != h) {
    mem_write16(c, 0x40, t, word);
    mem_write16(c, 0x40, 0x1c, next);
  }
}
static void built_in(CPU *, unsigned);
static void call_vector(CPU *c, unsigned n) {
  uint16_t off = mem_read16(c, 0, n * 4), seg = mem_read16(c, 0, n * 4 + 2);
  if (seg == 0xf000) {
    built_in(c, n);
    return;
  }
  uint16_t old_cs = c->cs, old_ip = c->ip;
  push16(c, c->flags);
  push16(c, old_cs);
  push16(c, 0xffff);
  c->flags &= ~(FLAG_IF | FLAG_TF);
  recomp_dispatch(c, seg, off);
  c->cs = old_cs;
  c->ip = old_ip;
}
static uint8_t ascii_for(unsigned sc, unsigned flags) {
  static const char row1[] = "1234567890-=", row1s[] = "!@#$%^&*()_+",
                    row2[] = "qwertyuiop[]", row3[] = "asdfghjkl;'`",
                    row4[] = "\\zxcvbnm,./";
  char a = 0;
  if (sc >= 2 && sc < 14)
    a = (flags & 3 ? row1s : row1)[sc - 2];
  else if (sc >= 0x10 && sc <= 0x1b)
    a = row2[sc - 0x10];
  else if (sc >= 0x1e && sc <= 0x29)
    a = row3[sc - 0x1e];
  else if (sc >= 0x2b && sc <= 0x35)
    a = row4[sc - 0x2b];
  else
    switch (sc) {
    case 1:
      a = 27;
      break;
    case 0xe:
      a = 8;
      break;
    case 0xf:
      a = 9;
      break;
    case 0x1c:
      a = 13;
      break;
    case 0x39:
      a = 32;
      break;
    }
  if ((flags & 3) && a >= 'a' && a <= 'z')
    a = toupper(a);
  if ((flags & 4) && isalpha((unsigned char)a))
    a = toupper(a) - 'A' + 1;
  return a;
}
static void keyboard_irq(CPU *c) {
  unsigned sc = key_port;
  if (sc == 0xe0) {
    extended_key = 1;
    return;
  }
  unsigned code = sc & 127, flags = mem_read8(c, 0x40, 0x17),
           bit = code == 0x2a   ? 2
                 : code == 0x36 ? 1
                 : code == 0x1d ? 4
                 : code == 0x38 ? 8
                                : 0;
  if (bit) {
    flags = sc & 128 ? flags & ~bit : flags | bit;
    mem_write8(c, 0x40, 0x17, flags);
  } else if (!(sc & 128))
    push_bios_key(c, (code << 8) | ascii_for(code, flags));
  extended_key = 0;
}

static void find_next(CPU *c) {
  if (find_pos >= find_count) {
    fail_api(c, 18);
    return;
  }
  const char *name = found[find_pos++];
  char p[4096];
  struct stat st = {0};
  if (resolve_file(save_dir, name, p, sizeof p) ||
      resolve_file(data_dir, name, p, sizeof p))
    stat(p, &st);
  mem_write8(c, dta_seg, dta_off + 0x15, 0x20);
  mem_write16(c, dta_seg, dta_off + 0x16, 0x6000);
  mem_write16(c, dta_seg, dta_off + 0x18, 0x1ee1);
  mem_write32(c, dta_seg, dta_off + 0x1a, (uint32_t)st.st_size);
  for (unsigned i = 0; i < 13; i++)
    mem_write8(c, dta_seg, dta_off + 0x1e + i,
               i < strlen(name) ? toupper((unsigned char)name[i]) : 0);
  ok(c);
}
static void find_first(CPU *c) {
  char pattern[256];
  asciiz(c, c->ds, c->dx, pattern, sizeof pattern);
  char *base = (char *)basename_dos(pattern);
  for (char *p = base; *p; p++)
    *p = toupper((unsigned char)*p);
  if (!strcmp(base, "*.*"))
    strcpy(base, "*");
  find_count = find_pos = 0;
  const char *dirs[2] = {save_dir, data_dir};
  for (unsigned k = 0; k < 2; k++) {
    DIR *d = opendir(dirs[k]);
    if (!d)
      continue;
    struct dirent *e;
    while ((e = readdir(d)) && find_count < 256) {
      char up[256];
      snprintf(up, sizeof up, "%s", e->d_name);
      for (char *p = up; *p; p++)
        *p = toupper((unsigned char)*p);
      if (up[0] == '.' || fnmatch(base, up, 0))
        continue;
      int dup = 0;
      for (unsigned i = 0; i < find_count; i++)
        if (!strcasecmp(found[i], up))
          dup = 1;
      if (!dup)
        snprintf(found[find_count++], 256, "%s", up);
    }
    closedir(d);
  }
  find_next(c);
}

void dos_int21(CPU *c) {
  unsigned ah = c->ah, al = c->al;
  char name[1024], path[4096];
  time_t current = time(NULL);
  struct tm tm;
  localtime_r(&current, &tm);
  if (getenv("LHX_TRACE"))
    fprintf(stderr, "[dos] AX=%04x BX=%04x CX=%04x DX=%04x DS=%04x ES=%04x\n",
            c->ax, c->bx, c->cx, c->dx, c->ds, c->es);
  switch (ah) {
  case 0:
  case 0x4c:
    finish(al);
    break;
  case 1:
  case 7:
  case 8: {
    int k;
    while ((k = pop_bios_key(c, 1)) < 0) {
      c->flags |= FLAG_IF;
      recomp_tick(c);
    }
    c->al = k;
    if (ah == 1)
      tty(c, c->al);
    break;
  }
  case 2:
    tty(c, c->dl);
    c->al = c->dl;
    break;
  case 6:
    if (c->dl == 255) {
      int k = pop_bios_key(c, 1);
      flag_zf(c, k < 0);
      c->al = k < 0 ? 0 : k;
    } else
      tty(c, c->dl);
    break;
  case 9:
    for (unsigned i = 0; i < 65536; i++) {
      uint8_t ch = mem_read8(c, c->ds, c->dx + i);
      if (ch == '$')
        break;
      tty(c, ch);
    }
    c->al = '$';
    break;
  case 0xb:
    c->al = pop_bios_key(c, 0) < 0 ? 0 : 255;
    break;
  case 0xc:
    while (pop_bios_key(c, 1) >= 0)
      ;
    c->ah = al;
    dos_int21(c);
    break;
  case 0xd:
    break;
  case 0xe:
    c->al = 3;
    break;
  case 0x19:
    c->al = 2;
    break;
  case 0x1a:
    dta_seg = c->ds;
    dta_off = c->dx;
    break;
  case 0x25:
    mem_write16(c, 0, al * 4, c->dx);
    mem_write16(c, 0, al * 4 + 2, c->ds);
    break;
  case 0x2a:
    c->cx = tm.tm_year + 1900;
    c->dh = tm.tm_mon + 1;
    c->dl = tm.tm_mday;
    c->al = tm.tm_wday;
    break;
  case 0x2c:
    c->ch = tm.tm_hour;
    c->cl = tm.tm_min;
    c->dh = tm.tm_sec;
    c->dl = (now_us() / 10000) % 100;
    break;
  case 0x2f:
    c->es = dta_seg;
    c->bx = dta_off;
    break;
  case 0x30:
    c->ax = 5;
    c->bx = 0xff00;
    c->cx = 0;
    break;
  case 0x33:
    if (al == 0)
      c->dl = 0;
    break;
  case 0x35:
    c->bx = mem_read16(c, 0, al * 4);
    c->es = mem_read16(c, 0, al * 4 + 2);
    break;
  case 0x36:
    c->ax = 64;
    c->bx = 0x7fff;
    c->cx = 512;
    c->dx = 0xffff;
    break;
  case 0x3b:
    ok(c);
    break;
  case 0x3c:
  case 0x3d: {
    asciiz(c, c->ds, c->dx, name, sizeof name);
    if (getenv("LHX_TRACE"))
      fprintf(stderr, "[open] %s mode=%u\n", name, ah == 0x3c ? 2 : al & 3);
    FILE *f = open_file(name, ah == 0x3c ? 2 : al & 3, ah == 0x3c);
    if (!f) {
      const char *base = basename_dos(name), *extension = strrchr(base, '.');
      int optional_probe = !strcasecmp(base, "lhx.cfg") ||
                           (extension && !strcasecmp(extension, ".fmd"));
      if (!optional_probe || getenv("LHX_TRACE"))
        fprintf(stderr, "[file] %s: %s\n", name, strerror(errno));
      fail_api(c, 2);
    } else {
      int h = new_handle(f);
      if (h < 0)
        fail_api(c, 4);
      else {
        const char *extension=strrchr(basename_dos(name),'.');
        if (terrain_textures && extension && !strcasecmp(extension,".LIB"))
          lhx_terrain_file_open((unsigned)h,f);
        c->ax = h;
        ok(c);
      }
    }
    break;
  }
  case 0x3e:
    if (c->bx < 5)
      ok(c);
    else if (c->bx < 64 && handles[c->bx]) {
      lhx_terrain_file_close(c->bx);
      fclose(handles[c->bx]);
      handles[c->bx] = NULL;
      ok(c);
    } else
      fail_api(c, 6);
    break;
  case 0x3f: {
    if (c->bx == 0) {
      c->ax = 0;
      ok(c);
      break;
    }
    if (c->bx >= 64 || !handles[c->bx]) {
      fail_api(c, 6);
      break;
    }
    long read_position=terrain_textures ? ftell(handles[c->bx]) : -1;
    unsigned n = 0;
    while (n < c->cx) {
      int ch = fgetc(handles[c->bx]);
      if (ch == EOF)
        break;
      mem_write8(c, c->ds, c->dx + n++, ch);
    }
    lhx_terrain_file_read(c->bx,read_position,n);
    c->ax = n;
    ok(c);
    break;
  }
  case 0x40: {
    if (c->bx == 0) {
      fail_api(c, 5);
      break;
    }
    if (c->bx >= 64 || (c->bx > 2 && !handles[c->bx])) {
      fail_api(c, 6);
      break;
    }
    unsigned n = 0;
    for (; n < c->cx; n++) {
      uint8_t ch = mem_read8(c, c->ds, c->dx + n);
      if (c->bx == 1 || c->bx == 2)
        tty(c, ch);
      else if (fputc(ch, handles[c->bx]) == EOF)
        break;
    }
    c->ax = n;
    ok(c);
    break;
  }
  case 0x41:
    asciiz(c, c->ds, c->dx, name, sizeof name);
    if (resolve_file(save_dir, basename_dos(name), path, sizeof path) &&
        !unlink(path))
      ok(c);
    else
      fail_api(c, 2);
    break;
  case 0x42: {
    if (c->bx >= 64 || !handles[c->bx] || al > 2) {
      fail_api(c, 6);
      break;
    }
    int32_t off = (int32_t)(((uint32_t)c->cx << 16) | c->dx);
    if (fseek(handles[c->bx], off,
              al == 0   ? SEEK_SET
              : al == 1 ? SEEK_CUR
                        : SEEK_END)) {
      fail_api(c, 1);
      break;
    }
    long p = ftell(handles[c->bx]);
    c->ax = p;
    c->dx = (unsigned long)p >> 16;
    ok(c);
    break;
  }
  case 0x43:
    asciiz(c, c->ds, c->dx, name, sizeof name);
    if (resolve_file(save_dir, basename_dos(name), path, sizeof path) ||
        resolve_file(data_dir, basename_dos(name), path, sizeof path)) {
      c->cx = 0x20;
      ok(c);
    } else
      fail_api(c, 2);
    break;
  case 0x44:
    if (al == 0) {
      c->dx = c->bx < 5 ? 0x80d3 : 2;
      ok(c);
    } else if (al == 1)
      ok(c);
    else if (al == 8) {
      c->ax = 1;
      ok(c);
    } else if (al == 0xe) {
      c->al = 0;
      ok(c);
    } else
      fail_api(c, 1);
    break;
  case 0x47:
    mem_write8(c, c->ds, c->si, 0);
    ok(c);
    break;
  case 0x48: {
    unsigned largest, seg = allocate(c, c->bx, &largest);
    if (seg) {
      c->ax = seg;
      ok(c);
    } else {
      c->bx = largest;
      fail_api(c, 8);
    }
    break;
  }
  case 0x49:
    if (c->es >= g_psp_seg && c->es < 0xa000) {
      mem_write16(c, c->es - 1, 1, 0);
      coalesce(c);
      ok(c);
    } else
      fail_api(c, 9);
    break;
  case 0x4a: {
    unsigned largest = 0;
    int err = resize_block(c, c->es, c->bx, &largest);
    if (err) {
      c->bx = largest;
      fail_api(c, err);
    } else
      ok(c);
    break;
  }
  case 0x4b:
    fail_api(c, 2);
    break;
  case 0x4d:
    c->ax = 0;
    break;
  case 0x4e:
    find_first(c);
    break;
  case 0x4f:
    find_next(c);
    break;
  case 0x51:
  case 0x62:
    c->bx = g_psp_seg;
    break;
  case 0x52:
    c->es = 0xf000;
    c->bx = 0x3002;
    break;
  case 0x57:
    if (!al) {
      c->cx = 0x6000;
      c->dx = 0x1ee1;
    }
    ok(c);
    break;
  case 0x58:
    c->ax = 0;
    ok(c);
    break;
  case 0x59:
    c->ax = 2;
    c->bx = 0x101;
    c->cx = 0;
    break;
  default:
    fprintf(stderr, "Unhandled DOS service AX=%04x\n", c->ax);
    fail_api(c, 1);
    break;
  }
}

void bios_int10(CPU *c) {
  unsigned ah = c->ah, al = c->al;
  switch (ah) {
  case 0:
    video_mode = al & 127;
    mem_write8(c, 0x40, 0x49, video_mode);
    mem_write16(c, 0x40, 0x4a, video_mode == 0x13 ? 40 : 80);
    if (!(al & 128)) {
      memset(c->mem + 0xa0000, 0, 65536);
      memset(c->mem + 0xb8000, 0, 4000);
    }
    cursor_x = cursor_y = 0;
    fprintf(stderr, "[video] mode %02x\n", video_mode);
    break;
  case 2:
    cursor_x = c->dl;
    cursor_y = c->dh;
    break;
  case 3:
    c->dl = cursor_x;
    c->dh = cursor_y;
    c->cx = 0x607;
    break;
  case 6:
  case 7:
  case 0xb:
    break;
  case 8:
    c->ax = mem_read16(c, 0xb800, (cursor_y * 80 + cursor_x) * 2);
    break;
  case 9:
  case 0xa:
    for (unsigned i = 0; i < c->cx; i++) {
      mem_write8(c, 0xb800, ((cursor_y * 80 + cursor_x + i) % 2000) * 2, al);
      if (ah == 9)
        mem_write8(c, 0xb800, ((cursor_y * 80 + cursor_x + i) % 2000) * 2 + 1,
                   c->bl);
    }
    break;
  case 0xc:
    if (video_mode == 0x13 && c->cx < 320 && c->dx < 200)
      c->mem[0xa0000 + c->dx * 320 + c->cx] = al;
    break;
  case 0xd:
    c->al = video_mode == 0x13 && c->cx < 320 && c->dx < 200
                ? c->mem[0xa0000 + c->dx * 320 + c->cx]
                : 0;
    break;
  case 0xe:
    tty(c, al);
    break;
  case 0xf:
    c->al = video_mode;
    c->ah = mem_read16(c, 0x40, 0x4a);
    c->bh = 0;
    break;
  case 0x10:
    if (al == 0x10) {
      unsigned i = (c->bx & 255) * 3;
      palette[i] = c->dh & 63;
      palette[i + 1] = c->ch & 63;
      palette[i + 2] = c->cl & 63;
    } else if (al == 0x12 || al == 0x17)
      for (unsigned i = 0; i < (unsigned)c->cx * 3; i++) {
        unsigned p = (c->bx * 3 + i) % 768;
        if (al == 0x12)
          palette[p] = mem_read8(c, c->es, c->dx + i) & 63;
        else
          mem_write8(c, c->es, c->dx + i, palette[p]);
      }
    else if (al == 0x15) {
      unsigned i = (c->bx & 255) * 3;
      c->dh = palette[i];
      c->ch = palette[i + 1];
      c->cl = palette[i + 2];
    } else if (al == 0x1a)
      c->bx = 0;
    break;
  case 0x11:
    if (al == 0x30) {
      c->es = 0xf000;
      c->bp = 0x4000;
      c->cx = 16;
      c->dl = 24;
    }
    break;
  case 0x12:
    if (c->bl == 0x10) {
      c->bx = 3;
      c->cx = 9;
    }
    break;
  case 0x1a:
    if (!al) {
      c->al = 0x1a;
      c->bx = 8;
    }
    break;
  default:
    fprintf(stderr, "Unhandled video BIOS service AX=%04x\n", c->ax);
    break;
  }
}
void bios_int16(CPU *c) {
  switch (c->ah) {
  case 0:
  case 0x10: {
    int k;
    while ((k = pop_bios_key(c, 1)) < 0) {
      c->flags |= FLAG_IF;
      recomp_tick(c);
    }
    c->ax = k;
    break;
  }
  case 1:
  case 0x11: {
    int k = pop_bios_key(c, 0);
    flag_zf(c, k < 0);
    if (k >= 0)
      c->ax = k;
    break;
  }
  case 2:
  case 0x12:
    c->al = mem_read8(c, 0x40, 0x17);
    break;
  case 5:
    push_bios_key(c, c->cx);
    c->al = 0;
    break;
  default:
    fprintf(stderr, "Unhandled keyboard BIOS AX=%04x\n", c->ax);
    break;
  }
}
void mouse_int33(CPU *c) {
  if (!c->ax)
    c->ax = c->bx = 0;
}
static unsigned bcd(unsigned v) { return (v / 10) * 16 + v % 10; }
static void built_in(CPU *c, unsigned n) {
  switch (n) {
  case 0:
    recomp_unhandled(c, c->cs, c->ip, "divide error");
    break;
  case 8: {
    unsigned ticks = mem_read32(c, 0x40, 0x6c) + 1;
    mem_write32(c, 0x40, 0x6c, ticks);
    call_vector(c, 0x1c);
    break;
  }
  case 9:
    keyboard_irq(c);
    break;
  case 0x10:
    bios_int10(c);
    break;
  case 0x11:
    c->ax = 0x61;
    break;
  case 0x12:
    c->ax = 640;
    break;
  case 0x15:
    c->ah = 0x86;
    c->flags |= FLAG_CF;
    break;
  case 0x16:
    bios_int16(c);
    break;
  case 0x1a: {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (c->ah == 0) {
      c->cx = mem_read16(c, 0x40, 0x6e);
      c->dx = mem_read16(c, 0x40, 0x6c);
      c->al = 0;
    } else if (c->ah == 2) {
      c->ch = bcd(tm.tm_hour);
      c->cl = bcd(tm.tm_min);
      c->dh = bcd(tm.tm_sec);
      c->dl = 0;
      ok(c);
    } else if (c->ah == 4) {
      c->ch = bcd((tm.tm_year + 1900) / 100);
      c->cl = bcd((tm.tm_year + 1900) % 100);
      c->dh = bcd(tm.tm_mon + 1);
      c->dl = bcd(tm.tm_mday);
      ok(c);
    }
    break;
  }
  case 0x20:
    finish(0);
    break;
  case 0x21:
    dos_int21(c);
    break;
  case 0x2f:
    c->al = 0;
    break;
  case 0x33:
    mouse_int33(c);
    break;
  case 0x1b:
  case 0x1c:
  case 0x23:
  case 0x24:
  case 0x28:
    break;
  default:
    fprintf(stderr, "Unhandled interrupt %02x AX=%04x\n", n, c->ax);
    break;
  }
}
void int_handler(CPU *c, unsigned n) { call_vector(c, n); }

/* MSC's int86 helper constructs `INT n; RETF` on its stack. Its register
 * structure is a documented API boundary, so replace the helper directly
 * instead of trying to execute a dynamically written trampoline. */
void lhx_int86(CPU *c) {
  uint16_t bp = c->bp, si = c->si, di = c->di;
  uint16_t sp = c->sp, ds = c->ds, ss = c->ss;
  uint16_t n = mem_read16(c, ss, sp + 4), in = mem_read16(c, ss, sp + 6),
           out = mem_read16(c, ss, sp + 8);
  c->ax = mem_read16(c, ds, in);
  c->bx = mem_read16(c, ds, in + 2);
  c->cx = mem_read16(c, ds, in + 4);
  c->dx = mem_read16(c, ds, in + 6);
  c->si = mem_read16(c, ds, in + 8);
  c->di = mem_read16(c, ds, in + 10);
  int_handler(c, n & 255);
  c->flags &= ~FLAG_DF;
  mem_write16(c, ds, out, c->ax);
  mem_write16(c, ds, out + 2, c->bx);
  mem_write16(c, ds, out + 4, c->cx);
  mem_write16(c, ds, out + 6, c->dx);
  mem_write16(c, ds, out + 8, c->si);
  mem_write16(c, ds, out + 10, c->di);
  mem_write16(c, ds, out + 12, cf(c) ? 1 : 0);
  c->bp = bp;
  c->si = si;
  c->di = di;
  c->sp = sp + 4;
}

static uint16_t pit_count(unsigned channel) {
  return (uint16_t)(pit[channel].divisor - pit_ticks() % pit[channel].divisor);
}
uint8_t port_in8(CPU *c, uint16_t p) {
  (void)c;
  switch (p) {
  case 0x20:
    return 0;
  case 0x21:
    return pic_mask;
  case 0x40:
  case 0x41:
  case 0x42: {
    PIT *t = &pit[p - 0x40];
    unsigned v = t->latched ? t->latch : pit_count(p - 0x40);
    if (t->access == 1)
      return v;
    if (t->access == 2)
      return v >> 8;
    unsigned r = t->flip ? v >> 8 : v;
    t->flip ^= 1;
    if (!t->flip)
      t->latched = 0;
    return r;
  }
  case 0x60:
    return key_port;
  case 0x61:
    speaker_gate ^= 0x10;
    return speaker_gate;
  case 0x64:
    return key_head != key_tail ? 1 : 0;
  case 0x201:
    return 255;
  case 0x3c4:
    return seq_index;
  case 0x3c5:
    return seq[seq_index & 7];
  case 0x3c7:
    return 3;
  case 0x3c8:
    return dac_write;
  case 0x3c9: {
    uint8_t v = palette[dac_read * 3 + dac_sub];
    if (++dac_sub == 3) {
      dac_sub = 0;
      dac_read = (dac_read + 1) & 255;
    }
    return v;
  }
  case 0x3cc:
    return 0x63;
  case 0x3ce:
    return gc_index;
  case 0x3cf:
    return gc[gc_index & 15];
  case 0x3d4:
    return crtc_index;
  case 0x3d5:
    return crtc[crtc_index & 31];
  case 0x3da: {
    static unsigned v;
    return (++v & 1) ? 9 : 0;
  }
  default:
    return 255;
  }
}
void port_out8(CPU *c, uint16_t p, uint8_t v) {
  (void)c;
  switch (p) {
  case 0x20:
    break;
  case 0x21:
    pic_mask = v;
    break;
  case 0x40:
  case 0x41:
  case 0x42: {
    PIT *t = &pit[p - 0x40];
    if (t->access == 1)
      t->value = v;
    else if (t->access == 2)
      t->value = v << 8;
    else if (!t->flip) {
      t->value = v;
      t->flip = 1;
      break;
    } else {
      t->value |= v << 8;
      t->flip = 0;
    }
    t->divisor = t->value ? t->value : 65536;
    if (p == 0x40)
      next_irq = pit_ticks() + t->divisor;
    else if (p == 0x42)
      atomic_store_explicit(&audio_divisor, t->divisor, memory_order_relaxed);
    break;
  }
  case 0x43: {
    unsigned n = v >> 6;
    if (n == 3)
      break;
    PIT *t = &pit[n];
    if (!(v & 0x30)) {
      t->latch = pit_count(n);
      t->latched = 1;
      t->flip = 0;
    } else {
      t->access = (v >> 4) & 3;
      t->flip = 0;
    }
    break;
  }
  case 0x61:
    speaker_gate = v;
    atomic_store_explicit(&audio_gate, (v & 3) == 3, memory_order_relaxed);
    break;
  case 0x3c4:
    seq_index = v;
    break;
  case 0x3c5:
    seq[seq_index & 7] = v;
    break;
  case 0x3c7:
    dac_read = v;
    dac_sub = 0;
    break;
  case 0x3c8:
    dac_write = v;
    dac_sub = 0;
    break;
  case 0x3c9:
    palette[dac_write * 3 + dac_sub] = v & 63;
    if (++dac_sub == 3) {
      dac_sub = 0;
      dac_write = (dac_write + 1) & 255;
    }
    break;
  case 0x3ce:
    gc_index = v;
    break;
  case 0x3cf:
    gc[gc_index & 15] = v;
    break;
  case 0x3d4:
    crtc_index = v;
    break;
  case 0x3d5:
    crtc[crtc_index & 31] = v;
    break;
  }
}
uint16_t port_in16(CPU *c, uint16_t p) {
  return port_in8(c, p) | (uint16_t)port_in8(c, p + 1) << 8;
}
void port_out16(CPU *c, uint16_t p, uint16_t v) {
  port_out8(c, p, v);
  port_out8(c, p + 1, v >> 8);
}

static void (*lookup(uint16_t seg, uint16_t off))(CPU *) {
  int32_t at = ((int32_t)seg - g_load_seg) * 16 + off;
  unsigned lo = 0, hi = g_func_count;
  while (lo < hi) {
    unsigned m = (lo + hi) / 2;
    if ((int32_t)g_funcs[m].address < at)
      lo = m + 1;
    else
      hi = m;
  }
  return lo < g_func_count && (int32_t)g_funcs[lo].address == at
             ? g_funcs[lo].func
             : NULL;
}
void recomp_unhandled(CPU *c, uint16_t seg, uint16_t off, const char *why) {
  fprintf(stderr, "[miss] %04X:%04X image=%05X: %s (SS:SP=%04X:%04X)\n", seg,
          off, (unsigned)(((int)seg - g_load_seg) * 16 + off), why, c->ss,
          c->sp);
  FILE *f = fopen("build/misses.txt", "a");
  if (f) {
    fprintf(f, "%04X:%04X\n", seg, off);
    fclose(f);
  }
  finish(2);
}
static void stub_iret(CPU *c, uint16_t off) {
  built_in(c, off & 255);
  c->sp += 4;
  uint16_t flags = pop16(c);
  if (off == 8 || off == 9 || off == 0x1c)
    c->flags = flags;
  else
    c->flags =
        (flags & ~(FLAG_CF | FLAG_ZF)) | (c->flags & (FLAG_CF | FLAG_ZF));
}
void recomp_dispatch(CPU *c, uint16_t seg, uint16_t off) {
  if (off == 0xffff)
    return;
  if (seg == 0xf000) {
    stub_iret(c, off);
    return;
  }
  if (lhx_audio_dispatch(c, seg, off))
    return;
  if (lhx_model_dispatch(c, seg, off))
    return;
  if (lhx_mission_dispatch(c, seg, off))
    return;
  if (lhx_cockpit_dispatch(c, seg, off))
    return;
  void (*fn)(CPU *) = lookup(seg, off);
  if (!fn)
    recomp_unhandled(c, seg, off, "untranslated control-flow target");
  uint16_t old_cs = c->cs, old_ip = c->ip;
  c->cs = seg;
  c->ip = off;
  fn(c);
  c->cs = old_cs;
  c->ip = old_ip;
}
void dispatch_near(CPU *c, uint16_t seg, uint16_t off) {
  recomp_dispatch(c, seg, off);
}
void dispatch_far(CPU *c, uint16_t seg, uint16_t off) {
  recomp_dispatch(c, seg, off);
}
int recomp_div0(const char *why, uint16_t expected_next_ip) {
  CPU *c = &machine;
  uint16_t off = mem_read16(c, 0, 0), seg = mem_read16(c, 0, 2);
  if (seg == 0xf000) {
    recomp_unhandled(c, c->cs, c->ip, why);
    return 1;
  }
  /* LHX's original INT 0 handler saturates overflowing projections and
   * advances the saved IP past the divide. A fault frame contains the actual
   * instruction address, unlike the synthetic return sentinels used for calls.
   */
  uint16_t fault_cs = c->cs;
  push16(c, c->flags);
  push16(c, fault_cs);
  push16(c, c->ip);
  uint16_t frame_ss = c->ss, frame_sp = c->sp;
  c->flags &= ~(FLAG_IF | FLAG_TF);
  recomp_dispatch(c, seg, off);
  uint16_t resume_ip = mem_read16(c, frame_ss, frame_sp);
  uint16_t resume_cs = mem_read16(c, frame_ss, frame_sp + 2);
  if (resume_cs == fault_cs && resume_ip == expected_next_ip)
    return 0;
  recomp_dispatch(c, resume_cs, resume_ip);
  return 1;
}
int cpu_lar(CPU *c, uint16_t sel, uint16_t *out) {
  (void)c;
  (void)sel;
  (void)out;
  return 0;
}
int cpu_lsl(CPU *c, uint16_t sel, uint16_t *out) {
  (void)c;
  (void)sel;
  (void)out;
  return 0;
}

static unsigned scan_for(SDL_Scancode s) {
  static const SDL_Scancode order[] = {SDL_SCANCODE_ESCAPE,
                                       SDL_SCANCODE_1,
                                       SDL_SCANCODE_2,
                                       SDL_SCANCODE_3,
                                       SDL_SCANCODE_4,
                                       SDL_SCANCODE_5,
                                       SDL_SCANCODE_6,
                                       SDL_SCANCODE_7,
                                       SDL_SCANCODE_8,
                                       SDL_SCANCODE_9,
                                       SDL_SCANCODE_0,
                                       SDL_SCANCODE_MINUS,
                                       SDL_SCANCODE_EQUALS,
                                       SDL_SCANCODE_BACKSPACE,
                                       SDL_SCANCODE_TAB,
                                       SDL_SCANCODE_Q,
                                       SDL_SCANCODE_W,
                                       SDL_SCANCODE_E,
                                       SDL_SCANCODE_R,
                                       SDL_SCANCODE_T,
                                       SDL_SCANCODE_Y,
                                       SDL_SCANCODE_U,
                                       SDL_SCANCODE_I,
                                       SDL_SCANCODE_O,
                                       SDL_SCANCODE_P,
                                       SDL_SCANCODE_LEFTBRACKET,
                                       SDL_SCANCODE_RIGHTBRACKET,
                                       SDL_SCANCODE_RETURN,
                                       SDL_SCANCODE_LCTRL,
                                       SDL_SCANCODE_A,
                                       SDL_SCANCODE_S,
                                       SDL_SCANCODE_D,
                                       SDL_SCANCODE_F,
                                       SDL_SCANCODE_G,
                                       SDL_SCANCODE_H,
                                       SDL_SCANCODE_J,
                                       SDL_SCANCODE_K,
                                       SDL_SCANCODE_L,
                                       SDL_SCANCODE_SEMICOLON,
                                       SDL_SCANCODE_APOSTROPHE,
                                       SDL_SCANCODE_GRAVE,
                                       SDL_SCANCODE_LSHIFT,
                                       SDL_SCANCODE_BACKSLASH,
                                       SDL_SCANCODE_Z,
                                       SDL_SCANCODE_X,
                                       SDL_SCANCODE_C,
                                       SDL_SCANCODE_V,
                                       SDL_SCANCODE_B,
                                       SDL_SCANCODE_N,
                                       SDL_SCANCODE_M,
                                       SDL_SCANCODE_COMMA,
                                       SDL_SCANCODE_PERIOD,
                                       SDL_SCANCODE_SLASH,
                                       SDL_SCANCODE_RSHIFT,
                                       SDL_SCANCODE_KP_MULTIPLY,
                                       SDL_SCANCODE_LALT,
                                       SDL_SCANCODE_SPACE,
                                       SDL_SCANCODE_CAPSLOCK,
                                       SDL_SCANCODE_F1,
                                       SDL_SCANCODE_F2,
                                       SDL_SCANCODE_F3,
                                       SDL_SCANCODE_F4,
                                       SDL_SCANCODE_F5,
                                       SDL_SCANCODE_F6,
                                       SDL_SCANCODE_F7,
                                       SDL_SCANCODE_F8,
                                       SDL_SCANCODE_F9,
                                       SDL_SCANCODE_F10,
                                       SDL_SCANCODE_NUMLOCKCLEAR,
                                       SDL_SCANCODE_SCROLLLOCK,
                                       SDL_SCANCODE_KP_7,
                                       SDL_SCANCODE_KP_8,
                                       SDL_SCANCODE_KP_9,
                                       SDL_SCANCODE_KP_MINUS,
                                       SDL_SCANCODE_KP_4,
                                       SDL_SCANCODE_KP_5,
                                       SDL_SCANCODE_KP_6,
                                       SDL_SCANCODE_KP_PLUS,
                                       SDL_SCANCODE_KP_1,
                                       SDL_SCANCODE_KP_2,
                                       SDL_SCANCODE_KP_3,
                                       SDL_SCANCODE_KP_0,
                                       SDL_SCANCODE_KP_PERIOD};
  for (unsigned i = 0; i < sizeof order / sizeof order[0]; i++)
    if (order[i] == s)
      return i + 1;
  switch (s) {
  case SDL_SCANCODE_UP:
    return 0xe048;
  case SDL_SCANCODE_DOWN:
    return 0xe050;
  case SDL_SCANCODE_LEFT:
    return 0xe04b;
  case SDL_SCANCODE_RIGHT:
    return 0xe04d;
  case SDL_SCANCODE_RCTRL:
    return 0xe01d;
  case SDL_SCANCODE_RALT:
    return 0xe038;
  case SDL_SCANCODE_KP_ENTER:
    return 0xe01c;
  default:
    return 0;
  }
}
static void queue_key(unsigned sc, int release) {
  if (!sc)
    return;
  if (sc > 255) {
    keyq[key_tail++ & 255] = 0xe0;
  }
  keyq[key_tail++ & 255] = (sc & 127) | (release ? 128 : 0);
}
/* Keep physical, scripted and controller keys independent when they overlap. */
static void input_key(unsigned source, unsigned sc, int release) {
  if (!sc)
    return;
  unsigned slot = (sc & 127) | (sc > 255 ? 128 : 0);
  unsigned before = input_sources[slot];
  if (release)
    input_sources[slot] &= ~source;
  else
    input_sources[slot] |= source;
  unsigned after = input_sources[slot];
  if ((!before && after) || (!release && (before & source)))
    queue_key(sc, 0);
  else if (before && !after)
    queue_key(sc, 1);
}
static void controller_key(unsigned scan, int release) {
  input_key(2, scan, release);
}
static void release_physical_keys(void) {
  for (unsigned i = 0; i < 256; ++i)
    if (input_sources[i] & 1)
      input_key(1, (i & 127) | (i >= 128 ? 0xe000 : 0), 1);
}
static void draw_frame(CPU *c) {
  if (video_mode != 0x13)
    return;
  lhx_fps_update(&fps_counter, now_us(), flight_frames);
  if (gpu) {
    const LhxSceneFrame *scene=lhx_scene_frame();
    uint64_t composite_start=render_stats_path?host_us():0;
    lhx_scene_overlay(scene,c->mem+0xa0000,palette,pixels);
    if (show_fps) lhx_fps_overlay(&fps_counter,pixels,320,200);
    uint64_t render_start=render_stats_path?host_us():0;
    render_options.terrain=lhx_terrain_current();
    render_options.time_seconds=now_us()/1000000.0;
    if (lhx_gpu_draw(gpu,scene,palette,pixels,&render_options)<0) {
      fprintf(stderr,"GPU scene rendering failed\n");finish(2);
    }
    if (render_stats_path && scene->view_count)
      lhx_scene_metrics_record_frame(scene,host_us()-render_start,render_start-composite_start);
    if (!headless) lhx_gpu_present(gpu);
    screen_pixels=native_pixels;
    return;
  }
  for (unsigned i = 0; i < 64000; i++) {
    unsigned p = c->mem[0xa0000 + i] * 3;
    pixels[i] = 0xff000000 | ((palette[p] * 255 / 63) << 16) |
                ((palette[p + 1] * 255 / 63) << 8) |
                (palette[p + 2] * 255 / 63);
  }
  if (native_renderer) {
    const LhxSceneFrame *scene = lhx_scene_frame();
    uint64_t render_start = render_stats_path ? host_us() : 0;
    render_options.terrain=lhx_terrain_current();
    render_options.time_seconds=now_us()/1000000.0;
    if (lhx_scene_render_with_options(scene, palette, native_pixels,
                                      render_width, render_height,&render_options) < 0) {
      fprintf(stderr, "High-resolution scene rendering failed\n");
      finish(2);
    }
    uint64_t composite_start = render_stats_path ? host_us() : 0;
    lhx_scene_composite(scene, c->mem + 0xa0000, palette, native_pixels,
                        render_width, render_height);
    if (render_stats_path && scene->view_count)
      lhx_scene_metrics_record_frame(scene, composite_start - render_start,
                                     host_us() - composite_start);
    if (show_fps)
      lhx_fps_overlay_scaled(&fps_counter, native_pixels, render_width,
                              render_height, render_scale);
    screen_pixels = native_pixels;
  } else {
    if (show_fps)
      lhx_fps_overlay(&fps_counter, pixels, 320, 200);
    screen_pixels = pixels;
  }
  if (renderer) {
    SDL_UpdateTexture(texture, NULL, screen_pixels, render_width * 4);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
  }
}
static void screenshot(CPU *c) {
  if (!shot_path)
    return;
  draw_frame(c);
  if (gpu && !lhx_gpu_read_pixels(gpu,native_pixels)) {
    fprintf(stderr,"GPU screenshot readback failed\n");result=2;return;
  }
  FILE *f = fopen(shot_path, "wb");
  if (!f) {
    perror(shot_path);
    return;
  }
  fprintf(f, "P6\n%u %u\n255\n", render_width, render_height);
  for (unsigned i = 0; i < render_width * render_height; i++) {
    uint8_t rgb[3] = {screen_pixels[i] >> 16, screen_pixels[i] >> 8, screen_pixels[i]};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
  fprintf(stderr, "[screenshot] %s mode=%02x %ux%u\n", shot_path, video_mode,
          render_width, render_height);
}
static void poll_services(CPU *c) {
  g_recomp_tick_budget = 500;
  if (polling)
    return;
  polling = 1;
  if(render_stats_path && !headless){
    uint64_t sample=host_us();
    if(service_metrics.last_poll){uint64_t gap=sample-service_metrics.last_poll;
      if(gap>service_metrics.max_service_gap_us)service_metrics.max_service_gap_us=gap;}
    service_metrics.last_poll=sample;service_metrics.polls++;
  }
  uint64_t now = now_us();
  if (limit_seconds > 0 && now >= (uint64_t)(limit_seconds * 1000000)) {
    polling = 0;
    finish(0);
  }
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT)
      finish(0);
    if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
      if (e.key.keysym.scancode == SDL_SCANCODE_F12) {
        if (e.type == SDL_KEYDOWN && !e.key.repeat)
          show_fps = !show_fps;
      } else
        input_key(1, scan_for(e.key.keysym.scancode), e.type == SDL_KEYUP);
    }
    if (controller_enabled)
      lhx_gamepad_event(&e);
    if (e.type == SDL_WINDOWEVENT) {
      if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        release_physical_keys();
        lhx_gamepad_focus(0);
      } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
        lhx_gamepad_focus(1);
    }
  }
  if (controller_enabled)
    lhx_gamepad_poll(now);
  for (unsigned i = 0; i < script_count; i++)
    if (!script[i].done && now >= (uint64_t)(script[i].at * 1000000)) {
      input_key(4, script[i].key, script[i].release);
      script[i].done = 1;
    }
  uint64_t ticks = pit_ticks();
  if(render_stats_path && ticks>next_irq){
    uint64_t late=(ticks-next_irq)*1000000/1193182;
    if(late>service_metrics.max_irq_lateness_us)service_metrics.max_irq_lateness_us=late;
  }
  if (ticks > next_irq + 119318){
    if(render_stats_path)service_metrics.timer_resets++;
    next_irq = ticks;
  }
  if (!in_irq && (c->flags & FLAG_IF)) {
    while (ticks >= next_irq && !(pic_mask & 1)) {
      next_irq += pit[0].divisor;
      in_irq = 1;
      call_vector(c, 8);
      in_irq = 0;
    }
    while (key_head != key_tail && !(pic_mask & 2)) {
      key_port = keyq[key_head++ & 255];
      in_irq = 1;
      call_vector(c, 9);
      in_irq = 0;
    }
  }
  if (now >= next_frame) {
    uint64_t draw_start=render_stats_path?host_us():0;
    draw_frame(c);
    if(render_stats_path){
      uint64_t elapsed=host_us()-draw_start,serial=lhx_scene_frame()->serial;
      if(service_metrics.draws && serial==service_metrics.last_serial)service_metrics.repeat_draws++;
      service_metrics.last_serial=serial;service_metrics.draws++;
      if(elapsed>service_metrics.max_draw_us)service_metrics.max_draw_us=elapsed;
      if(elapsed>16667)service_metrics.slow_draws++;
    }
    next_frame = lhx_present_deadline(now,now_us());
  }
  polling = 0;
}

void recomp_tick(CPU *c) {
  if (headless && !polling)
    virtual_us += 300;
  poll_services(c);
}

void lhx_frame_pace(CPU *c) {
  uint64_t deadline = lhx_frame_clock_next(&flight_clock, now_us());
  uint64_t now;
  while ((now = now_us()) < deadline) {
    if (headless)
      virtual_us = deadline;
    else {
      uint64_t remaining = deadline - now;
      SDL_Delay(remaining > 2000 ? 2 : 1);
    }
    poll_services(c);
  }
  /* LHX samples its IRQ counter immediately after this observer. Deliver
   * elapsed timer interrupts before it computes the simulation timestep. */
  poll_services(c);
  last_flight_us = now_us();
  if (!flight_frames)
    first_flight_us = last_flight_us;
  flight_frames++;
}

static void init_machine(CPU *c) {
  cpu_init(c);
  c->mem = calloc(1, MEM_SIZE);
  if (!c->mem) {
    perror("calloc");
    finish(2);
  }
  memcpy(c->mem + g_load_seg * 16, g_image, g_image_len);
  for (unsigned i = 0; i < g_nrelocs; i++) {
    unsigned off = g_relocs[i][0], seg = g_load_seg + g_relocs[i][1];
    mem_write16(c, seg, off, mem_read16(c, seg, off) + g_load_seg);
  }
  for (unsigned i = 0; i < 256; i++) {
    mem_write16(c, 0, i * 4, i);
    mem_write16(c, 0, i * 4 + 2, 0xf000);
  }
  c->ds = c->es = g_psp_seg;
  c->cs = g_load_seg + g_entry_cs;
  c->ip = g_entry_ip;
  c->ss = g_load_seg + g_entry_ss;
  c->sp = g_entry_sp;
  c->flags |= FLAG_IF;
  mem_write16(c, g_psp_seg, 0, 0x20cd);
  mem_write16(c, g_psp_seg, 2, 0xa000);
  mem_write16(c, g_psp_seg, 0x16, g_psp_seg);
  mem_write16(c, g_psp_seg, 0x2c, 0x80);
  const char env[] =
      "COMSPEC=C:\\COMMAND.COM\0PATH=C:\\\0\0\x01\0C:\\LHX\\LHX.EXE\0";
  memcpy(c->mem + 0x800, env, sizeof env);
  /* Keep the original PC speaker player active even when host output is muted.
   * This also retains the player's timer scheduling and music-resource paths.
   */
  const char *tail = " VGA KEYBOARD HIGH PCSOUND";
  size_t tail_len = strlen(tail);
  mem_write8(c, g_psp_seg, 0x80, tail_len);
  memcpy(c->mem + g_psp_seg * 16 + 0x81, tail, tail_len);
  mem_write8(c, g_psp_seg, 0x81 + tail_len, 13);
  dta_seg = g_psp_seg;
  dta_off = 0x80;
  mcb(c, g_psp_seg - 1, 0x5a, g_psp_seg, 0xa000 - g_psp_seg);
  mem_write16(c, 0xf000, 0x3000, g_psp_seg - 1);
  mem_write16(c, 0x40, 0x10, 0x61);
  mem_write16(c, 0x40, 0x13, 640);
  mem_write16(c, 0x40, 0x1a, 0x1e);
  mem_write16(c, 0x40, 0x1c, 0x1e);
  mem_write16(c, 0x40, 0x80, 0x1e);
  mem_write16(c, 0x40, 0x82, 0x3e);
  mem_write8(c, 0x40, 0x49, 3);
  mem_write16(c, 0x40, 0x4a, 80);
  mem_write8(c, 0x40, 0x84, 24);
  for (unsigned i = 0; i < 2000; i++)
    mem_write16(c, 0xb800, i * 2, 0x0720);
  for (unsigned i = 0; i < 256; i++)
    palette[i * 3] = palette[i * 3 + 1] = palette[i * 3 + 2] = i / 4;
}
static void parse_keys(const char *text) {
  char *copy = strdup(text), *save = NULL;
  for (char *p = strtok_r(copy, ",", &save); p;
       p = strtok_r(NULL, ",", &save)) {
    char *colon = strchr(p, ':');
    if (!colon) {
      fprintf(stderr, "Bad --keys entry: %s\n", p);
      exit(2);
    }
    *colon++ = 0;
    char *end;
    double at = strtod(p, &end);
    if (*end || at < 0) {
      fprintf(stderr, "Bad key time\n");
      exit(2);
    }
    SDL_Scancode sc = SDL_GetScancodeFromName(colon);
    if (!strcasecmp(colon, "enter"))
      sc = SDL_SCANCODE_RETURN;
    else if (!strcasecmp(colon, "esc"))
      sc = SDL_SCANCODE_ESCAPE;
    unsigned key = scan_for(sc);
    if (!key || script_count + 2 > 256) {
      fprintf(stderr, "Unsupported scripted key %s\n", colon);
      exit(2);
    }
    script[script_count++] = (ScriptKey){at, key, 0, 0};
    script[script_count++] = (ScriptKey){at + 0.08, key, 1, 0};
  }
  free(copy);
}
int main(int argc, char **argv) {
  int visibility_selected = 0;
  int configure_controls = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--data-dir") && i + 1 < argc)
      data_dir = argv[++i];
    else if (!strcmp(argv[i], "--save-dir") && i + 1 < argc)
      save_dir = argv[++i];
    else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
      limit_seconds = strtod(argv[++i], NULL);
    else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
      char *end;
      unsigned long rate = strtoul(argv[++i], &end, 10);
      if (!*argv[i] || *end || rate < 10 || rate > 120) {
        fprintf(stderr, "--fps must be an integer from 10 to 120\n");
        return 2;
      }
      flight_clock.rate = (unsigned)rate;
    }
    else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc)
      shot_path = argv[++i];
    else if (!strcmp(argv[i], "--dump-memory") && i + 1 < argc)
      memory_path = argv[++i];
    else if (!strcmp(argv[i], "--headless"))
      headless = 1;
    else if (!strcmp(argv[i], "--show-fps"))
      show_fps = 1;
    else if (!strcmp(argv[i],"--backend") && i+1<argc) {
      const char *name=argv[++i];
      if (strcasecmp(name,"cpu") && strcasecmp(name,"gpu")) {
        fprintf(stderr,"--backend must be cpu or gpu\n");return 2;
      }
      gpu_requested=!strcasecmp(name,"gpu");backend_selected=1;
    }
    else if (!strcmp(argv[i], "--renderer") && i + 1 < argc) {
      const char *name = argv[++i];
      if (strcasecmp(name, "native") && strcasecmp(name, "original")) {
        fprintf(stderr, "--renderer must be native or original\n");
        return 2;
      }
      native_renderer = !strcasecmp(name, "native");
      renderer_selected = 1;
    }
    else if (!strcmp(argv[i], "--render-scale") && i + 1 < argc) {
      char *end;
      unsigned long scale = strtoul(argv[++i], &end, 10);
      if (!*argv[i] || *end || scale < 1 || scale > 8) {
        fprintf(stderr, "--render-scale must be an integer from 1 to 8\n");
        return 2;
      }
      render_scale = (unsigned)scale;
      scale_selected = 1;
    }
    else if (!strcmp(argv[i], "--render-stats") && i + 1 < argc)
      render_stats_path = argv[++i];
    else if (!strcmp(argv[i], "--visibility") && i + 1 < argc) {
      const char *mode = argv[++i];
      if (strcmp(mode, "original") && strcmp(mode, "extended")) {
        fprintf(stderr, "--visibility must be original or extended\n");
        return 2;
      }
      g_lhx_scene_world_enabled = !strcmp(mode, "extended");
      visibility_selected = 1;
    }
    else if (!strcmp(argv[i], "--sound"))
      sound_enabled = 1;
    else if (!strcmp(argv[i], "--mute"))
      sound_enabled = 0;
    else if (!strcmp(argv[i], "--no-controller"))
      controller_enabled = 0;
    else if (!strcmp(argv[i], "--list-controllers"))
      list_controllers = 1;
    else if (!strcmp(argv[i], "--configure-controls"))
      configure_controls = 1;
    else if (!strcmp(argv[i], "--controls") && i + 1 < argc) {
      controls_path = argv[++i];
      controls_explicit = 1;
    }
    else if (!strcmp(argv[i], "--keys") && i + 1 < argc)
      parse_keys(argv[++i]);
    else if (!strcmp(argv[i], "--help")) {
      puts("Usage: lhx-native [--data-dir bin/dos] [--save-dir saves] "
           "[--headless] [--mute] [--fps 60] [--show-fps] [--seconds N] [--screenshot image.ppm] "
           "[--dump-memory image.bin] "
           "[--no-controller] [--list-controllers] [--configure-controls] [--controls controls.ini] "
           "[--renderer native|original] [--render-scale 3] "
           "[--backend cpu|gpu] "
           "[--render-stats report.json] "
           "[--visibility extended|original] "
           "[--keys "
           "2:enter,4:enter]");
      return 0;
    } else {
      fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
      return 2;
    }
  }
  if (configure_controls && (headless || list_controllers)) {
    fprintf(stderr,"--configure-controls cannot be combined with --headless or --list-controllers\n");
    return 2;
  }
  if (list_controllers) {
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) < 0) {
      fprintf(stderr, "SDL controllers: %s\n", SDL_GetError());
      return 2;
    }
    lhx_gamepad_list();
    SDL_Quit();
    return 0;
  }
  LhxControls config;
  if (!lhx_controls_load(&config, controls_path, controls_explicit && !configure_controls))
    return 2;
  if (configure_controls || (!headless && config.show_controller_setup)) {
    int setup=lhx_controller_setup_run(&config,controls_path);
    if (setup<=0) return setup<0?2:0;
  }
  lhx_gamepad_configure(&config);
  improved_sounds = config.improved_sounds;
  audio_channels = improved_sounds ? 2 : 1;
  lhx_sound_init(&enhanced_audio, audio_rate);
  if (improved_sounds) lhx_audio_observe(lhx_sound_submit, &enhanced_audio);
  if (!renderer_selected && config.rendering.renderer != LHX_RENDER_DEFAULT) {
    native_renderer = config.rendering.renderer == LHX_RENDER_NATIVE;
    renderer_selected = 1;
  }
  if (!scale_selected && config.rendering.render_scale) {
    render_scale = config.rendering.render_scale;
    scale_selected = 1;
  }
  if (!visibility_selected)
    g_lhx_scene_world_enabled = config.rendering.extended_visibility;
  show_fps |= config.rendering.show_fps;
  render_options.lighting=config.rendering.lighting;
  render_options.runway_textures=config.rendering.runway_textures;
  render_options.antialiasing=config.rendering.antialiasing;
  render_options.sphere_depth=config.rendering.sphere_depth;
  render_options.hide_ground_dots=config.rendering.hide_ground_dots;
  render_options.smooth_distant_aircraft=config.rendering.smooth_distant_aircraft;
  render_options.atmosphere=config.rendering.atmosphere;
  render_options.terrain_relief=config.rendering.terrain_relief;
  render_options.shadows=config.rendering.shadows;
  render_options.military_paint=config.rendering.military_paint;
  render_options.surface_detail=config.rendering.surface_detail;
  render_options.rotor_blades=config.rendering.rotor_blades;
  render_options.runway_detail=config.rendering.runway_detail;
  if (!backend_selected) gpu_requested=config.rendering.gpu;
  /* Preserve the original-size deterministic smoke-test interface. Native
   * headless rendering is available explicitly for captures and validation. */
  if (headless && !renderer_selected && !scale_selected)
    native_renderer = 0;
  g_lhx_scene_enabled = native_renderer;
  gpu_requested=gpu_requested && native_renderer;
  terrain_textures=native_renderer && config.rendering.terrain_textures;
  lhx_terrain_configure(terrain_textures,
                        config.rendering.terrain_dir);
  lhx_terrain_relief_configure(config.rendering.terrain_relief);
  if(native_renderer && config.rendering.replacement_models)
    scenery=lhx_props_load(config.rendering.models_dir);
  render_options.props=scenery;
  if(native_renderer && config.rendering.photo_skies)sky=lhx_sky_load(config.rendering.sky_dir);
  render_options.sky=sky;
  render_width = native_renderer ? 320 * render_scale : 320;
  render_height = native_renderer ? 200 * render_scale : 200;
  if (render_stats_path && !native_renderer) {
    fprintf(stderr, "--render-stats requires --renderer native\n");
    return 2;
  }
  if (mkdir(save_dir, 0755) && errno != EEXIST) {
    perror(save_dir);
    return 2;
  }
  if (headless)
    SDL_SetHint(SDL_HINT_VIDEODRIVER, gpu_requested?"offscreen":"dummy");
  if (headless)
    controller_enabled = 0;
  if (headless && sound_enabled)
    SDL_SetHint(SDL_HINT_AUDIODRIVER, "dummy");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
    if (headless && gpu_requested) {
      fprintf(stderr,"[gpu] offscreen video unavailable: %s; using CPU\n",SDL_GetError());
      SDL_Quit();gpu_requested=0;SDL_SetHint(SDL_HINT_VIDEODRIVER,"dummy");
      if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER)<0) {fprintf(stderr,"SDL: %s\n",SDL_GetError());return 2;}
    } else {fprintf(stderr, "SDL: %s\n", SDL_GetError());return 2;}
  }
  if (controller_enabled) {
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0) {
      fprintf(stderr, "Controller unavailable: %s\n", SDL_GetError());
      controller_enabled = 0;
    } else
      lhx_gamepad_init(controller_key);
  }
  if (sound_enabled) {
    SDL_AudioSpec want = {0}, have;
    want.freq = audio_rate;
    want.format = AUDIO_S16SYS;
    want.channels = audio_channels;
    want.samples = 512;
    want.callback = speaker_audio;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
      audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!audio_device) {
      fprintf(stderr, "Audio unavailable, continuing muted: %s\n",
              SDL_GetError());
      sound_enabled = 0;
    } else {
      audio_rate = have.freq;
      audio_channels = have.channels;
      fprintf(stderr, "[audio] %s, %d Hz, %d channel%s\n",
              improved_sounds ? "improved procedural sounds" : "original PC speaker",
              audio_rate, audio_channels, audio_channels == 1 ? "" : "s");
      SDL_PauseAudioDevice(audio_device, 0);
    }
  }
  if (gpu_requested) {
    lhx_gpu_attributes();
    window=SDL_CreateWindow("LHX: Attack Chopper — GPU",SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED,960,600,SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE|
      (headless?SDL_WINDOW_HIDDEN:0));
    if (window) gpu=lhx_gpu_create(window,render_width,render_height,render_options.antialiasing,render_stats_path!=NULL);
    if (!gpu) {
      fprintf(stderr,"[gpu] unavailable: %s; using native CPU renderer\n",SDL_GetError());
      SDL_DestroyWindow(window);window=NULL;
    }
  }
  if (!headless && !gpu) {
    window = SDL_CreateWindow(native_renderer ? "LHX: Attack Chopper — high resolution" :
                                               "LHX: Attack Chopper — original rendering",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              960, 600, SDL_WINDOW_RESIZABLE);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
      fprintf(stderr, "SDL renderer: %s\n", SDL_GetError());
      SDL_Quit();
      return 2;
    }
    SDL_RenderSetLogicalSize(renderer, render_width, render_height);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, render_width, render_height);
    if (!texture) {
      fprintf(stderr, "SDL texture: %s\n", SDL_GetError());
      SDL_Quit();
      return 2;
    }
    SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest);
  }
  if (native_renderer) {
    native_pixels = calloc((size_t)render_width * render_height, sizeof(*native_pixels));
    if (!native_pixels) {
      perror("native framebuffer");
      SDL_Quit();
      return 2;
    }
    screen_pixels = native_pixels;
  }
  fprintf(stderr, "[renderer] %s %ux%u\n", native_renderer ? "native" : "original",
          render_width, render_height);
  fprintf(stderr,"[backend] %s\n",gpu?"gpu":"cpu");
  if (render_stats_path && !lhx_scene_metrics_init(0)) {
    fprintf(stderr, "Cannot allocate rendering measurements\n");
    free(native_pixels);
    SDL_Quit();
    return 2;
  }
  init_machine(&machine);
  start_us = host_us();
  exit_ready = 1;
  if (!setjmp(exit_jmp)) {
    fprintf(stderr, "LHX native: %u lifted functions, %u-byte image\n",
            g_func_count, g_image_len);
    recomp_dispatch(&machine, g_load_seg + g_entry_cs, g_entry_ip);
    fprintf(stderr, "[exit] game entry returned\n");
  }
  if (limit_seconds > 0) {
    fprintf(stderr, "[runtime] elapsed=%.6f requested=%.6f exit=%d\n",
            now_us() / 1000000.0, limit_seconds, result);
    fprintf(stderr, "[pacing] target=%u frames=%llu span=%.6f\n",
            flight_clock.rate, (unsigned long long)flight_frames,
            (last_flight_us - first_flight_us) / 1000000.0);
  }
  screenshot(&machine);
  if (render_stats_path) {
    FILE *report = fopen(render_stats_path, "w");
    if (!report) {
      perror(render_stats_path);
      result = 2;
    } else {
      fprintf(report, "{\"renderer\":\"native\",\"width\":%u,\"height\":%u,"
                      "\"visibility\":\"%s\",\"backend\":\"%s\",\"gpu\":", render_width, render_height,
              g_lhx_scene_world_enabled ? "extended" : "original",gpu?"gpu":"cpu");
      if (gpu) lhx_gpu_report(gpu,report); else fputs("null",report);
      fprintf(report,",\"services\":{\"polls\":%llu,\"max_gap_us\":%llu,\"draws\":%llu,\"slow_draws\":%llu,\"max_draw_us\":%llu,\"repeated_scene_draws\":%llu,\"timer_resets\":%llu,\"max_irq_lateness_us\":%llu,\"audio_callbacks\":%llu,\"max_audio_callback_gap_us\":%llu}",
          (unsigned long long)service_metrics.polls,(unsigned long long)service_metrics.max_service_gap_us,
          (unsigned long long)service_metrics.draws,(unsigned long long)service_metrics.slow_draws,
          (unsigned long long)service_metrics.max_draw_us,(unsigned long long)service_metrics.repeat_draws,
          (unsigned long long)service_metrics.timer_resets,(unsigned long long)service_metrics.max_irq_lateness_us,
          (unsigned long long)atomic_load_explicit(&audio_callbacks,memory_order_relaxed),
          (unsigned long long)atomic_load_explicit(&audio_max_callback_gap,memory_order_relaxed));
      fputs(",\"statistics\":",report);
      int good = lhx_scene_metrics_report_json(report);
      if (fprintf(report, "}\n") < 0 || ferror(report))
        good = -1;
      if (fclose(report))
        good = -1;
      if (good <= 0) {
        fprintf(stderr, "Cannot write rendering measurements: %s\n", render_stats_path);
        result = 2;
      } else
        fprintf(stderr, "[render-stats] %s\n", render_stats_path);
    }
  }
  if (memory_path) {
    FILE *f = fopen(memory_path, "wb");
    if (!f || fwrite(machine.mem, 1, MEM_SIZE, f) != MEM_SIZE) {
      perror(memory_path);
      result = 2;
    }
    if (f)
      fclose(f);
  }
  if (audio_device)
    SDL_CloseAudioDevice(audio_device);
  lhx_gamepad_close();
  for (unsigned i = 5; i < 64; i++)
    if (handles[i])
      fclose(handles[i]);
  lhx_model_reset();
  lhx_scene_reset();
  lhx_scene_renderer_reset();
  lhx_terrain_reset();
  lhx_scene_metrics_reset();
  lhx_gpu_destroy(gpu);
  lhx_props_free(scenery);
  lhx_sky_free(sky);
  free(native_pixels);
  free(machine.mem);
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return result;
}
