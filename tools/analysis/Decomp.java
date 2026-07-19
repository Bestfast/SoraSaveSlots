import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class Decomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        long base = currentProgram.getImageBase().getOffset();
        for (String a : args) {
            long rva = Long.parseLong(a.replace("0x", ""), 16);
            Address addr = currentProgram.getImageBase().add(rva);
            Function f = getFunctionContaining(addr);
            if (f == null) { println("!! no function at " + a); continue; }
            println("\n=================================================================");
            println("== " + f.getName() + "  @0x" + Long.toHexString(f.getEntryPoint().getOffset() - base)
                    + "   (asked " + a + ")");
            println("=================================================================");
            DecompileResults r = di.decompileFunction(f, 120, monitor);
            if (r.decompileCompleted()) println(r.getDecompiledFunction().getC());
            else println("!! decompile failed: " + r.getErrorMessage());
        }
        di.dispose();
    }
}
