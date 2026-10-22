// Export Ghidra function list: entry, name, body ranges, thunk flag, calling convention.
// Output: private/ghidra/functions.tsv  (path given as script arg 0)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import java.io.*;

public class ExportFuncs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "/tmp/functions.tsv";
        try (PrintWriter pw = new PrintWriter(new FileWriter(out))) {
            pw.println("entry\tname\tnranges\tbytes\tthunk\tcc\tnoreturn\tranges");
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext()) {
                Function f = it.next();
                AddressSetView b = f.getBody();
                StringBuilder sb = new StringBuilder();
                long bytes = 0; int n = 0;
                for (AddressRange r : b) {
                    if (n++ > 0) sb.append(',');
                    sb.append(Long.toHexString(r.getMinAddress().getOffset())).append('-')
                      .append(Long.toHexString(r.getMaxAddress().getOffset()));
                    bytes += r.getLength();
                }
                pw.println(Long.toHexString(f.getEntryPoint().getOffset()) + "\t" + f.getName() + "\t" + n + "\t" + bytes + "\t"
                    + f.isThunk() + "\t" + f.getCallingConventionName() + "\t" + f.hasNoReturn() + "\t" + sb);
            }
        }
        println("exported");
    }
}
