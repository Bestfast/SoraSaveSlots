import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import java.io.PrintWriter;

public class DecompRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long lo = Long.parseLong(a[0].replace("0x", ""), 16);
        long hi = Long.parseLong(a[1].replace("0x", ""), 16);
        String out = a[2];
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter pw = new PrintWriter(out);
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        int n = 0;
        while (it.hasNext()) {
            Function f = it.next();
            long rva = f.getEntryPoint().getOffset() - base;
            if (rva < lo || rva >= hi) continue;
            pw.println("\n//========== " + f.getName() + " @0x" + Long.toHexString(rva) + " ==========");
            DecompileResults r = di.decompileFunction(f, 90, monitor);
            if (r.decompileCompleted()) pw.println(r.getDecompiledFunction().getC());
            else pw.println("// decompile failed: " + r.getErrorMessage());
            n++;
        }
        pw.close();
        di.dispose();
        println("decompiled " + n + " functions -> " + out);
    }
}
