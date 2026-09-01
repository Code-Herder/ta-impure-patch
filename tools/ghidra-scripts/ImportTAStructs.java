//Parses a C header (tools/tamem_ghidra.h) into the program's data type manager.
//Usage: analyzeHeadless <proj> <name> -process <prog> -noanalysis -postScript ImportTAStructs.java /abs/tamem_ghidra.h
//@category TA
import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParserUtils;
import ghidra.app.util.cparser.C.CParserUtils.CParseResults;
import ghidra.program.model.data.DataTypeManager;

public class ImportTAStructs extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("usage: ImportTAStructs.java <header file>");
            return;
        }
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        int before = dtm.getDataTypeCount(true);
        CParseResults results = null;
        try {
            results = CParserUtils.parseHeaderFiles(
                new DataTypeManager[0],
                new String[] { args[0] },
                new String[0],
                dtm,
                monitor);
        }
        catch (Exception e) {
            printerr("CParser failed: " + e.getMessage());
            if (results != null) {
                printerr(results.getFormattedParseMessage(null));
            }
            throw e;
        }
        int after = dtm.getDataTypeCount(true);
        println("ImportTAStructs: parse successful=" + results.successful() +
            " dataTypes before=" + before + " after=" + after +
            " added=" + (after - before));
        String msg = results.getFormattedParseMessage(null);
        if (msg != null && !msg.isEmpty()) {
            println(msg);
        }
    }
}
