// Pattern scan over instruction text. args: out, regex1(csv of regexes separated by ;;)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;
import java.util.*;
import java.util.regex.Pattern;
public class WeaponSurvey2 extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0]);
        String[] res = args[1].split(";;");
        List<Pattern> pats = new ArrayList<>();
        for (String r : res) pats.add(Pattern.compile(r));
        Map<String, List<String>> perFunc = new TreeMap<>();
        InstructionIterator ii = currentProgram.getListing().getInstructions(true);
        while (ii.hasNext()) {
            Instruction ins = ii.next();
            String t = ins.toString();
            for (int k = 0; k < pats.size(); k++) {
                if (pats.get(k).matcher(t).find()) {
                    Function f = getFunctionContaining(ins.getAddress());
                    String fn = f == null ? "?" : f.getName() + "@" + f.getEntryPoint();
                    perFunc.computeIfAbsent(fn, x -> new ArrayList<>()).add("p" + k + " " + ins.getAddress() + " " + t);
                }
            }
        }
        for (Map.Entry<String, List<String>> e : perFunc.entrySet()) {
            Set<String> kinds = new TreeSet<>();
            for (String s : e.getValue()) kinds.add(s.substring(0, 2));
            out.println("FUNC " + e.getKey() + "  kinds=" + kinds + " n=" + e.getValue().size());
            for (String s : e.getValue()) out.println("    " + s);
        }
        out.close(); println("done");
    }
}
