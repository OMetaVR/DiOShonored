/* Shared D3D9 test scene (used by the Linux SDL3 test and the iOS probe app).
 * Exercises what UE3's D3D9Drv uses: SM3.0 VS+PS (d3dbc), DXT1/DXT5 textures with mips,
 * alpha blending, render-to-texture, vertex declarations over 2 streams, index buffers,
 * D24S8 depth-stencil, bool/float shader constants. Pure C interface. */
#ifndef D3D9_SCENE_H
#define D3D9_SCENE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct d3d9_scene d3d9_scene;

typedef struct d3d9_scene_desc {
    void *(*create9)(unsigned sdk_version); /* Direct3DCreate9 from the DXVK d3d9 library */
    void *window;                           /* SDL_Window* on DXVK-native (it is the HWND) */
    int   width, height;                    /* drawable size in pixels */
    int   vsync;
} d3d9_scene_desc;

/* Returns 0 on success; err receives a message otherwise. Log lines go to log_cb (may be NULL). */
int  d3d9_scene_create(const d3d9_scene_desc *d, d3d9_scene **out, char *err, size_t errsz,
                       void (*log_cb)(const char *line, void *user), void *user);
/* Renders one frame (RTT pass + backbuffer pass) and presents. 0 = ok. */
int  d3d9_scene_frame(d3d9_scene *s, float t);
/* Reads back the RTT texture centre and the backbuffer centre-quad + corner; writes a report.
 * Returns 0 if values are consistent with the expected shader output. */
int  d3d9_scene_selftest(d3d9_scene *s, char *report, size_t n);
/* Reads the backbuffer as tightly packed BGRA8 into *out (malloc'ed; caller frees). 0 = ok. */
int  d3d9_scene_read_backbuffer(d3d9_scene *s, uint8_t **out, int *w, int *h);
/* Writes the current backbuffer to a binary PPM (P6). 0 = ok. */
int  d3d9_scene_screenshot_ppm(d3d9_scene *s, const char *path);
/* 1 if the scene used real DXT1/DXT5 textures, 0 if the D3D9 layer lacked DXT and it fell back to A8R8G8B8. */
int  d3d9_scene_used_dxt(d3d9_scene *s);
void d3d9_scene_destroy(d3d9_scene *s);

#ifdef __cplusplus
}
#endif
#endif
