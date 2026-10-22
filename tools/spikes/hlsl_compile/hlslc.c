#define COBJMACROS
/* Minimal fxc substitute: HLSL -> D3D9 SM1-3 bytecode (d3dbc) via D3DCompile.
 * Built with mingw, run under wine (wine's d3dcompiler_47 wraps vkd3d-shader's HLSL compiler).
 * usage: hlslc.exe <in.hlsl> <entry> <profile e.g. vs_3_0> <out.bin> [disasm.txt]
 */
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>

static void *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc(*n + 1); fread(b, 1, *n, f); fclose(f); return b;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: hlslc in.hlsl entry profile out.bin [disasm.txt]\n"); return 1; }
    size_t n; void *src = slurp(argv[1], &n);
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, n, argv[1], NULL, NULL, argv[2], argv[3], 0, 0, &code, &err);
    if (err) fprintf(stderr, "%.*s\n", (int)ID3D10Blob_GetBufferSize(err), (char *)ID3D10Blob_GetBufferPointer(err));
    if (FAILED(hr)) { fprintf(stderr, "D3DCompile failed 0x%08lx\n", (unsigned long)hr); return 3; }
    FILE *o = fopen(argv[4], "wb");
    fwrite(ID3D10Blob_GetBufferPointer(code), 1, ID3D10Blob_GetBufferSize(code), o); fclose(o);
    printf("%s %s -> %s (%u bytes)\n", argv[1], argv[3], argv[4], (unsigned)ID3D10Blob_GetBufferSize(code));
    if (argc > 5) {
        ID3DBlob *d = NULL;
        if (SUCCEEDED(D3DDisassemble(ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), 0, NULL, &d))) {
            FILE *t = fopen(argv[5], "wb"); fwrite(ID3D10Blob_GetBufferPointer(d), 1, ID3D10Blob_GetBufferSize(d), t); fclose(t);
        }
    }
    return 0;
}
