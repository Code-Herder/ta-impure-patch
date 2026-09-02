// For each hex VA in args[0], print the containing function (name@entry) to args[1].
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class FuncAtTA extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[1]);
        for (String a : args[0].split(",")) {
            Address addr = toAddr(Long.parseLong(a.trim().replace("0x",""), 16));
            Function f = getFunctionContaining(addr);
            out.println(a + " in " + (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()));
        }
        out.close();
        println("done");
    }
}
