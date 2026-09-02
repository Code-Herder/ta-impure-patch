// Disassembly listing of address ranges (args[0]: "start-end,start-end" hex VAs) to args[1].
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class DisasmTA extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[1]);
        for (String r : args[0].split(",")) {
            String[] se = r.split("-");
            Address s = toAddr(Long.parseLong(se[0].trim().replace("0x",""), 16));
            Address e = toAddr(Long.parseLong(se[1].trim().replace("0x",""), 16));
            out.println("// ---- " + r);
            if (getInstructionAt(s) == null) disassemble(s);
            Instruction ins = getInstructionAt(s);
            if (ins == null) ins = getInstructionContaining(s);
            while (ins != null && ins.getAddress().compareTo(e) <= 0) {
                StringBuilder b = new StringBuilder();
                byte[] by = ins.getBytes();
                for (byte x : by) b.append(String.format("%02x", x & 0xff));
                out.println(String.format("%s  %-20s %s", ins.getAddress(), b.toString(), ins.toString()));
                ins = ins.getNext();
            }
        }
        out.close();
        println("done");
    }
}
