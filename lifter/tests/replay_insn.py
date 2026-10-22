import sys, pickle, struct
sys.path.insert(0, '.')
import insn_tests as it, difftest as d
from unicorn.x86_const import *
work = d.OUT + '/insn'
fails = pickle.load(open(work + '/fails.pkl', 'rb'))
tests = []
for g in (it.alu_tests, it.unary_tests, it.shift_tests, it.muldiv_tests, it.conv_tests, it.bit_tests, it.cc_tests, it.string_tests, it.stack_tests, it.x87_tests, it.sse_tests):
    tests += g()
want = sys.argv[1]
blob = open(work + '/t.bin', 'rb').read(); size = (len(blob) + 0xfff) & ~0xfff
env = d.Env(None, mods=[(0x50000000, size, blob + b'\0' * (size - len(blob)), 'blob')], out=work, driver=work + '/diffdrv'); env.setup_uc()
n = 0
for ti, lst in fails.items():
    t = tests[ti]
    if want not in t.name: continue
    res, c = lst[0]
    u = env.run_case(c)
    d.write_cases(work + '/one.bin', [c])
    import subprocess
    subprocess.run([work + '/diffdrv', work + '/images.txt', work + '/one.bin', work + '/one_res.bin'])
    dd = d.read_results(work + '/one_res.bin', 1)[0]
    print('TEST', t.name, 'pre=', t.pre, 'mask=', hex(t.mask), res)
    print(' regs in :', [hex(x) for x in c['r']])
    uc, cc = u['cap'], dd['cap']
    for off in range(0, 512, 4):
        if uc[off:off+4] != cc[off:off+4]:
            print('  cap+%x unicorn=%s c=%s' % (off, uc[off:off+4].hex(), cc[off:off+4].hex()))
    n += 1
    if n >= int(sys.argv[2] if len(sys.argv) > 2 else 2): break
