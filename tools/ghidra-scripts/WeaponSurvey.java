// Survey script: (1) instructions whose scalar operands match a list of values,
// (2) defined strings matching a regex + their xrefs (and xrefs of pointer tables that
// point at them), (3) xrefs to a list of data addresses.
// args: out-file, scalars(hex,csv), string-regex, data-addrs(hex,csv)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import java.io.PrintWriter;
import java.util.*;
import java.util.regex.Pattern;

public class WeaponSurvey extends GhidraScript {
    PrintWriter out;
    String fn(Address a) {
        Function f = getFunctionContaining(a);
        return f == null ? "?" : f.getName() + "@" + f.getEntryPoint();
    }
    void xrefs(Address a, String label, int depth) {
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(a);
        while (it.hasNext()) {
            Reference r = it.next();
            Address from = r.getFromAddress();
            Instruction ins = getInstructionAt(from);
            if (ins != null) {
                out.println("  " + label + " <- code " + from + " in " + fn(from) + " : " + ins);
            } else {
                out.println("  " + label + " <- data " + from + (depth < 2 ? "  (table? following)" : ""));
                if (depth < 2) xrefs(from, label + "/tbl@" + from, depth + 1);
            }
        }
    }
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        out = new PrintWriter(args[0]);
        Set<Long> scalars = new HashSet<>();
        for (String s : args[1].split(",")) if (!s.trim().isEmpty()) scalars.add(Long.parseLong(s.trim().replace("0x",""), 16));
        Pattern re = Pattern.compile(args[2]);
        out.println("=== SCALAR OPERAND HITS " + args[1]);
        InstructionIterator ii = currentProgram.getListing().getInstructions(true);
        while (ii.hasNext()) {
            Instruction ins = ii.next();
            for (int i = 0; i < ins.getNumOperands(); i++) {
                for (Object o : ins.getOpObjects(i)) {
                    if (o instanceof Scalar) {
                        long v = ((Scalar)o).getUnsignedValue();
                        if (scalars.contains(v)) {
                            out.println(String.format("  0x%X  %s  in %s : %s", v, ins.getAddress(), fn(ins.getAddress()), ins));
                        }
                    }
                }
            }
        }
        out.println("=== STRINGS matching " + args[2]);
        DataIterator di = currentProgram.getListing().getDefinedData(true);
        while (di.hasNext()) {
            Data d = di.next();
            if (!(d.getDataType() instanceof StringDataType) && !(d.getDataType() instanceof TerminatedStringDataType)) continue;
            Object v = d.getValue();
            if (v == null) continue;
            String s = v.toString();
            if (re.matcher(s).matches()) {
                out.println("STR " + d.getAddress() + " \"" + s + "\"");
                xrefs(d.getAddress(), "\"" + s + "\"", 0);
            }
        }
        out.println("=== DATA ADDR XREFS " + args[3]);
        for (String s : args[3].split(",")) {
            if (s.trim().isEmpty()) continue;
            Address a = toAddr(Long.parseLong(s.trim().replace("0x",""), 16));
            out.println("DATA " + a);
            xrefs(a, s.trim(), 0);
        }
        out.close();
        println("done");
    }
}
