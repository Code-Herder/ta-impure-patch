// Print code xrefs to each function entry in args[1] (hex csv) into args[0].
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
public class CallersOf extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0]);
        for (String s : args[1].split(",")) {
            Address a = toAddr(Long.parseLong(s.trim().replace("0x",""), 16));
            Function f = getFunctionAt(a);
            out.println("FUNC " + a + " " + (f == null ? "?" : f.getName()));
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(a);
            while (it.hasNext()) {
                Reference r = it.next();
                Function c = getFunctionContaining(r.getFromAddress());
                out.println("   <- " + r.getFromAddress() + " " + r.getReferenceType() + " in " + (c == null ? "?" : c.getName() + "@" + c.getEntryPoint()));
            }
        }
        out.close(); println("done");
    }
}
