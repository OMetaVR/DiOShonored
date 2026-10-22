#!/usr/bin/env python3
"""Generate runtime/platform/d3d9_generated.inc: guest-vtable thunks for every D3D9 (non-Ex) interface, from the DXVK-native d3d9.h.

  python3 -I runtime/tools/gen_d3d9.py [path/to/d3d9.h]

For each method a `GuestFn` thunk is emitted that reads the 32-bit stack arguments, converts them (guest COM objects -> host objects,
guest pointers -> host pointers for layout-identical structs, out-interfaces -> new guest wrappers), calls the host method and pops the right
number of bytes. Methods whose arguments contain pointer-sized members (D3DPRESENT_PARAMETERS, D3DLOCKED_RECT, HWND-carrying structs, void**)
are listed in MANUAL and implemented by hand in platform/d3d9_manual.cpp; anything else that cannot be marshalled mechanically gets a loud stub.
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
HDR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "third_party/dxvk/install-linux/include/dxvk/d3d9.h")
OUT = os.path.join(ROOT, "runtime/platform/d3d9_generated.inc")

IFACES = ["IDirect3D9", "IDirect3DDevice9", "IDirect3DSwapChain9", "IDirect3DSurface9", "IDirect3DVolume9", "IDirect3DTexture9",
          "IDirect3DCubeTexture9", "IDirect3DVolumeTexture9", "IDirect3DVertexBuffer9", "IDirect3DIndexBuffer9", "IDirect3DVertexDeclaration9",
          "IDirect3DVertexShader9", "IDirect3DPixelShader9", "IDirect3DStateBlock9", "IDirect3DQuery9"]
KIND = {"IDirect3D9": "K_D3D9", "IDirect3DDevice9": "K_Device", "IDirect3DSwapChain9": "K_SwapChain", "IDirect3DSurface9": "K_Surface",
        "IDirect3DVolume9": "K_Volume", "IDirect3DTexture9": "K_Texture", "IDirect3DCubeTexture9": "K_CubeTexture",
        "IDirect3DVolumeTexture9": "K_VolumeTexture", "IDirect3DVertexBuffer9": "K_VertexBuffer", "IDirect3DIndexBuffer9": "K_IndexBuffer",
        "IDirect3DVertexDeclaration9": "K_VertexDecl", "IDirect3DVertexShader9": "K_VertexShader", "IDirect3DPixelShader9": "K_PixelShader",
        "IDirect3DStateBlock9": "K_StateBlock", "IDirect3DQuery9": "K_Query", "IDirect3DResource9": "K_Unknown", "IDirect3DBaseTexture9": "K_Unknown"}
UNKNOWN_IFACES = {"IDirect3DResource9", "IDirect3DBaseTexture9"}
IUNKNOWN = {"QueryInterface", "AddRef", "Release"}

MANUAL = {
    "IDirect3D9": {"CreateDevice", "GetAdapterMonitor", "RegisterSoftwareDevice", "GetAdapterIdentifier"},
    "IDirect3DDevice9": {"GetCreationParameters", "Reset", "CreateAdditionalSwapChain", "Present"},
    "IDirect3DSwapChain9": {"GetPresentParameters", "Present"},
    "IDirect3DSurface9": {"LockRect", "UnlockRect", "GetContainer", "GetDC", "ReleaseDC"},
    "IDirect3DTexture9": {"LockRect", "UnlockRect"},
    "IDirect3DCubeTexture9": {"LockRect", "UnlockRect"},
    "IDirect3DVolumeTexture9": {"LockBox", "UnlockBox"},
    "IDirect3DVolume9": {"LockBox", "UnlockBox", "GetContainer"},
    "IDirect3DVertexBuffer9": {"Lock", "Unlock"},
    "IDirect3DIndexBuffer9": {"Lock", "Unlock"},
}
# layout-identical between the 32-bit guest and the 64-bit host (no pointer-sized members; headers use pack(4))
POD = {"D3DCAPS9", "D3DCLIPSTATUS9", "D3DDISPLAYMODE", "D3DINDEXBUFFER_DESC", "D3DLIGHT9", "D3DMATERIAL9", "D3DMATRIX",
       "D3DRASTER_STATUS", "D3DSURFACE_DESC", "D3DVERTEXBUFFER_DESC", "D3DVERTEXELEMENT9", "D3DVIEWPORT9", "D3DVOLUME_DESC", "PALETTEENTRY", "RECT",
       "POINT", "D3DRECT", "D3DBOX", "DWORD", "UINT", "INT", "float", "int", "WINBOOL", "void", "RGNDATA", "D3DRECTPATCH_INFO", "D3DTRIPATCH_INFO",
       "D3DGAMMARAMP"}
VALUE_TYPES = {"int", "UINT", "INT", "DWORD", "WINBOOL", "D3DCOLOR", "UINT32"}

def norm(t):
    t = " ".join(t.replace("struct ", "").split())
    t = t.replace(" *", "*").replace("* ", "*")
    return t

def parse_params(s):
    out = []
    if not s.strip():
        return out
    for p in re.split(r',(?![^()]*\))', s):
        p = " ".join(p.split())
        m = re.match(r'^(.*?)(\w+)(\[\d*\])?$', p)
        if not m or not m.group(1).strip() or m.group(1).strip() in ("const", "struct"):
            typ, name = p, "arg%d" % len(out)       # unnamed parameter
        else:
            typ, name = m.group(1).strip(), m.group(2)
        out.append((norm(typ), name))
    return out

def classify(typ):
    """-> (kind, detail). kind in value,float,hwnd,riid,iface_in,iface_out,ptr,handle_null,manual"""
    t = typ.replace("const ", "").strip()
    stars = t.count("*")
    base = t.replace("*", "").strip()
    if stars == 0:
        if base == "float": return ("float", None)
        if base == "HWND": return ("hwnd", None)
        if base in ("REFIID", "REFGUID"): return ("riid", None)
        if base == "HDC": return ("manual", "HDC")
        if base in VALUE_TYPES or base.startswith("D3D"): return ("value", None)
        return ("manual", base)
    if stars == 1:
        if base in IFACES or base in UNKNOWN_IFACES: return ("iface_in", base)
        if base == "HANDLE": return ("handle_null", None)
        if base in POD: return ("ptr", base)
        return ("manual", base + "*")
    if stars == 2:
        if base in IFACES or base in UNKNOWN_IFACES: return ("iface_out", base)
        return ("manual", base + "**")
    return ("manual", t)

def parse_interfaces(text):
    res = {}
    for m in re.finditer(r'DECLARE_INTERFACE_\((\w+),\s*(\w+)\)\s*\{(.*?)\n\};', text, re.S):
        name, body = m.group(1), m.group(3)
        if name not in IFACES:
            continue
        methods = []
        for mm in re.finditer(r'STDMETHOD(?:_\(\s*([^,)]+?)\s*,\s*(\w+)\s*\)|\(\s*(\w+)\s*\))\s*\(\s*THIS(_)?\s*(.*?)\)\s*PURE;', body, re.S):
            ret = mm.group(1) or "HRESULT"
            mname = mm.group(2) or mm.group(3)
            params = parse_params(mm.group(5) or "")
            methods.append((ret, mname, params))
        res[name] = methods
    return res

def main():
    text = open(HDR).read()
    ifs = parse_interfaces(text)
    missing = [i for i in IFACES if i not in ifs]
    if missing:
        sys.exit("interfaces not found in header: %s" % missing)
    o = []
    o.append("// Generated by runtime/tools/gen_d3d9.py from d3d9.h (DXVK-native). Do not edit.")
    stats = {"generated": 0, "manual": 0, "unsupported": 0}
    unsupported_list = []
    for iface in IFACES:
        methods = ifs[iface]
        for idx, (ret, mname, params) in enumerate(methods):
            fname = "t_%s_%s" % (iface, mname)
            nslots = 1 + len(params)
            pop = 4 * nslots
            if mname in IUNKNOWN:
                continue
            if mname in MANUAL.get(iface, ()):
                stats["manual"] += 1
                o.append("GuestRet m_%s_%s(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);" % (iface, mname))
                continue
            kinds = [classify(t) for t, _ in params]
            bad = [(t, k) for (t, _), k in zip(params, kinds) if k[0] == "manual"]
            retbase = ret.replace("const ", "").strip()
            if bad or retbase not in ("HRESULT", "ULONG", "UINT", "DWORD", "WINBOOL", "void", "float") and not retbase.startswith("D3D"):
                stats["unsupported"] += 1
                unsupported_list.append("%s::%s (%s)" % (iface, mname, ", ".join(b[0] for b in bad) or ret))
                o.append("static GuestRet %s(uint32_t esp, uint32_t, uint32_t, uint32_t) { return d3d9_unsupported(\"%s::%s\", %d); }" % (fname, iface, mname, pop))
                continue
            stats["generated"] += 1
            L = []
            L.append("static GuestRet %s(uint32_t esp, uint32_t, uint32_t, uint32_t) {" % fname)
            L.append("    auto *self = static_cast<%s *>(com_host_object_raw(A(0)));" % iface)
            L.append("    if (__builtin_expect(!self, 0)) return d3d9_bad_this(\"%s::%s\", %d);" % (iface, mname, pop))
            L.append("    D3D9_TRACE(\"%s::%s\", %d);" % (iface, mname, len(params)))
            args = []
            post = []
            for i, ((t, n), (k, d)) in enumerate(zip(params, kinds)):
                a = "A(%d)" % (i + 1)
                v = "a%d" % (i + 1)
                if k == "value":
                    L.append("    %s %s = (%s)%s;" % (t, v, t, a))
                elif k == "float":
                    L.append("    float %s = AF(%d);" % (v, i + 1))
                elif k == "hwnd":
                    L.append("    HWND %s = d3d9_host_hwnd(%s);" % (v, a))
                elif k == "riid":
                    L.append("    const GUID &%s = *(const GUID *)(guest_base + %s);" % (v, a))
                elif k == "iface_in":
                    L.append("    %s %s = (%s)(%s ? com_host_object_raw(%s) : nullptr);" % (t, v, t, a, a))
                elif k == "iface_out":
                    L.append("    void *%s_raw = nullptr;" % v)
                    L.append("    %s %s = (%s)&%s_raw;" % (t, v, t, v))
                    post.append((v, a, KIND[d]))
                elif k == "ptr":
                    L.append("    %s %s = (%s)(%s ? guest_base + %s : nullptr);" % (t, v, t, a, a))
                elif k == "handle_null":
                    L.append("    (void)%s; HANDLE *%s = nullptr;" % (a, v))
                    post.append((None, a, None))
                args.append(v)
            call = "self->%s(%s)" % (mname, ", ".join(args))
            L.append("    uint32_t r = 0;")
            L.append("    try { %s } catch (...) { return d3d9_exception(\"%s::%s\", %d); }" % (
                ("r = (uint32_t)%s;" % call) if retbase != "void" else ("%s; r = 0;" % call), iface, mname, pop))
            for v, a, kind in post:
                if v is None:
                    L.append("    if (%s) wr32(%s, 0);" % (a, a))
                else:
                    raw = v + "_raw"
                    L.append("    if (r == 0 && %s) { if (%s) wr32(%s, d3d9_wrap(%s, %s)); else static_cast<IUnknown *>(%s)->Release(); } else if (%s) wr32(%s, 0);" % (
                        raw, a, a, raw, kind, raw, a, a))
            L.append("    return guest_ret(r, 0, %d);" % pop)
            L.append("}")
            o.extend(L)
        # vtable
        o.append("static const ComMethod kMethods_%s[] = {" % iface)
        for ret, mname, params in methods:
            if mname in IUNKNOWN:
                o.append('    {"%s", d3d9_%s},' % (mname, mname))
            elif mname in MANUAL.get(iface, ()):
                o.append('    {"%s", m_%s_%s},' % (mname, iface, mname))
            else:
                o.append('    {"%s", t_%s_%s},' % (mname, iface, mname))
        o.append("};")
    o.append("struct D3d9IfaceDef { const char *name; const ComMethod *methods; size_t n; };")
    o.append("static const D3d9IfaceDef kD3d9Ifaces[] = {")
    for iface in IFACES:
        o.append('    {"%s", kMethods_%s, sizeof(kMethods_%s) / sizeof(kMethods_%s[0])},' % (iface, iface, iface, iface))
    o.append("};")
    o.append("static const D3d9Kind kD3d9IfaceKinds[] = {%s};" % ", ".join(KIND[i] for i in IFACES))
    open(OUT, "w").write("\n".join(o) + "\n")
    print("wrote %s: %d generated, %d manual, %d unsupported" % (OUT, stats["generated"], stats["manual"], stats["unsupported"]))
    for u in unsupported_list:
        print("  unsupported:", u)

main()
