// Decompile a list of functions (hex addresses in args[0], comma-separated)
// and write the C to args[1].
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class DecompileTAFuncs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String[] addrs = args[0].split(",");
        PrintWriter out = new PrintWriter(args[1]);
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (String a : addrs) {
            Address addr = toAddr(Long.parseLong(a.trim().replace("0x",""), 16));
            Function f = getFunctionAt(addr);
            if (f == null) f = getFunctionContaining(addr);
            out.println("// ================= " + a + " =================");
            if (f == null) { out.println("// NO FUNCTION AT " + a); continue; }
            out.println("// name: " + f.getName() + "  entry: " + f.getEntryPoint());
            DecompileResults res = di.decompileFunction(f, 120, monitor);
            if (res != null && res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("// DECOMPILE FAILED");
            }
            out.flush();
        }
        di.dispose();
        out.close();
        println("done");
    }
}
