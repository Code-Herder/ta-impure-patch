// Dump disassembly windows: args: out, csv of addr:before:after (counts of instructions)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;
public class DisasmWindow extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0]);
        for (String spec : args[1].split(",")) {
            String[] p = spec.split(":");
            Address a = toAddr(Long.parseLong(p[0].trim().replace("0x",""), 16));
            int before = Integer.parseInt(p[1]), after = Integer.parseInt(p[2]);
            Instruction ins = getInstructionContaining(a);
            for (int i = 0; i < before && ins != null; i++) { Instruction pr = ins.getPrevious(); if (pr == null) break; ins = pr; }
            out.println("==== window around " + a);
            for (int i = 0; i < before + after && ins != null; i++) {
                out.println((ins.getAddress().equals(a) ? ">> " : "   ") + ins.getAddress() + "  " + ins);
                ins = ins.getNext();
            }
        }
        out.close(); println("done");
    }
}
