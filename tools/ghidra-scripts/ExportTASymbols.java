//Exports all USER_DEFINED symbols (and function status) to a text file for verification.
//Output lines: name address kind  (kind: f = function entry, l = label)
//Usage: analyzeHeadless <proj> <name> -process <prog> -noanalysis -postScript ExportTASymbols.java /abs/out.txt
//@category TA
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolType;
import ghidra.program.model.symbol.SourceType;

public class ExportTASymbols extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("usage: ExportTASymbols.java <out file>");
            return;
        }
        int n = 0;
        PrintWriter w = new PrintWriter(args[0]);
        try {
            SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(false);
            while (it.hasNext()) {
                Symbol s = it.next();
                if (s.getSource() != SourceType.USER_DEFINED) {
                    continue;
                }
                String kind = (s.getSymbolType() == SymbolType.FUNCTION) ? "f" : "l";
                w.println(s.getName() + " 0x" + s.getAddress() + " " + kind);
                n++;
            }
        }
        finally {
            w.close();
        }
        println("ExportTASymbols: wrote " + n + " user-defined symbols to " + args[0]);
    }
}
