// List call/jump references TO each address in args[0] (comma-separated hex); write to args[1].
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class XrefsTA extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[1]);
        for (String a : args[0].split(",")) {
            Address addr = toAddr(Long.parseLong(a.trim().replace("0x",""), 16));
            out.println("// xrefs to " + a);
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(addr);
            while (it.hasNext()) {
                Reference r = it.next();
                Function f = getFunctionContaining(r.getFromAddress());
                out.println("  from " + r.getFromAddress() + " (" + r.getReferenceType() + ") in " + (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()));
            }
        }
        out.close();
        println("done");
    }
}
