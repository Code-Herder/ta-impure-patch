//Imports symbols from a text file of "name address [type]" lines
//(the classic ImportSymbolsScript.py format: type f = function, l = label).
//Usage: analyzeHeadless <proj> <name> -process <prog> -postScript ImportTASymbols.java /abs/path/symbols.txt
//@category TA
import java.io.BufferedReader;
import java.io.FileReader;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ImportTASymbols extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("usage: ImportTASymbols.java <symbols file>");
            return;
        }
        int nFuncsRenamed = 0, nFuncsCreated = 0, nLabels = 0, nErr = 0;
        BufferedReader r = new BufferedReader(new FileReader(args[0]));
        try {
            String line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) {
                    continue;
                }
                String[] parts = line.split("\\s+");
                if (parts.length < 2) {
                    continue;
                }
                String name = parts[0];
                String type = parts.length > 2 ? parts[2] : "l";
                try {
                    Address addr = toAddr(parts[1]);
                    if (type.equals("f")) {
                        Function fn = getFunctionAt(addr);
                        if (fn == null) {
                            fn = createFunction(addr, name);
                            if (fn != null) {
                                nFuncsCreated++;
                            }
                        }
                        if (fn != null) {
                            fn.setName(name, SourceType.USER_DEFINED);
                            nFuncsRenamed++;
                        }
                        else {
                            // couldn't make a function there; at least drop a label
                            createLabel(addr, name, true, SourceType.USER_DEFINED);
                            nLabels++;
                            println("WARN not-a-function, labeled instead: " + line);
                        }
                    }
                    else {
                        createLabel(addr, name, true, SourceType.USER_DEFINED);
                        nLabels++;
                    }
                }
                catch (Exception e) {
                    nErr++;
                    println("ERR  " + line + " : " + e.getMessage());
                }
            }
        }
        finally {
            r.close();
        }
        println("ImportTASymbols: functions named=" + nFuncsRenamed +
            " (newly created=" + nFuncsCreated + ") labels=" + nLabels +
            " errors=" + nErr);
    }
}
