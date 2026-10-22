# Local changes to the vendored MojoShader

Upstream: https://github.com/icculus/mojoshader at ad5dff84830c2863c841f4b1f4e3df78c705b383 (zlib license, see LICENSE.txt). All files are the
upstream files except `profiles/mojoshader_profile_metal.c`, whose changes are marked `LOCAL CHANGE (dish)` and collected in `local-changes.patch`
(unified diff against upstream). Upstream asks that AI-generated code is not contributed to it; these changes are for this project only and are not
meant to be upstreamed as they are.

1. `TEXLDL` (texture load with explicit LOD, required for vertex texture fetch) used to be emitted as a plain `sample()`, dropping the LOD; it now emits
   `sample(s, uv, level(w))`.
2. A relative-addressed constant array (`c[a0.x + n]`) emitted `#define cN uniforms.uniforms_float4[K];` with a stray `;`, which breaks every plain use
   of a register inside such an array.
3. Vertex outputs / pixel inputs with a semantic other than position, psize, colour, fog, texcoord and normal (tangent, binormal, ...) were silently
   dropped from the interpolant structs while the body still referenced them; they are now generic `[[user(usageU_I)]]` interpolants. (The `NORMAL`
   output case also fell through into the default case.)

Handled outside the vendored code (runtime/platform/d3d9_metal/shader_xlate.cpp): partial-mask `texkill`, shaders with relative constant addressing
and a stripped constant table (a synthetic CTAB declaring c0..cN is injected), the MSL entry-point cosmetics and the alpha-test hook.
