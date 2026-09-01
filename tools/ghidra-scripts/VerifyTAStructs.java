//Verifies key TADR structs exist in the program DTM with the expected packed sizes.
//@category TA
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeManager;
import java.util.Iterator;

public class VerifyTAStructs extends GhidraScript {

    private void check(DataTypeManager dtm, String name, int expected) {
        java.util.ArrayList<DataType> hits = new java.util.ArrayList<>();
        dtm.findDataTypes(name, hits);
        if (hits.isEmpty()) {
            println("MISSING " + name);
            return;
        }
        DataType dt = hits.get(0);
        int len = dt.getLength();
        println((expected < 0 || len == expected ? "OK      " : "BADSIZE ") + name +
            " len=0x" + Integer.toHexString(len) +
            (expected >= 0 ? " expected=0x" + Integer.toHexString(expected) : ""));
    }

    @Override
    public void run() throws Exception {
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        int nStruct = 0, nEnum = 0;
        Iterator<ghidra.program.model.data.Structure> sit = dtm.getAllStructures();
        while (sit.hasNext()) {
            sit.next();
            nStruct++;
        }
        Iterator<DataType> it = dtm.getAllDataTypes();
        while (it.hasNext()) {
            if (it.next() instanceof ghidra.program.model.data.Enum) {
                nEnum++;
            }
        }
        println("total structures=" + nStruct + " enums=" + nEnum +
            " all types=" + dtm.getDataTypeCount(true));
        check(dtm, "UnitStruct", 0x118);
        check(dtm, "UnitDefStruct", 0x249);
        check(dtm, "WeaponStruct", 0x115);
        check(dtm, "ProjectileStruct", 0x6B);
        check(dtm, "ExplosionStruct", 0x54);
        check(dtm, "DebrisStruct", 0x34);
        check(dtm, "TAdynmemStruct", -1);
        check(dtm, "PlayerStruct", -1);
        check(dtm, "Object3doStruct", -1);
        check(dtm, "PrimitiveStruct", -1);
        check(dtm, "Model3DONode", -1);
        check(dtm, "Model3DOFace", -1);
        check(dtm, "GafAnimStruct", -1);
        check(dtm, "FeatureStruct", -1);
        check(dtm, "FeatureDefStruct", -1);
        check(dtm, "UnitOrdersStruct", -1);
        check(dtm, "TAProgramStruct", -1);
    }
}
