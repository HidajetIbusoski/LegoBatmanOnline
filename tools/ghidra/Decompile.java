// Ghidra-Skript (headless): dekompiliert die Funktionen, die die angegebenen Adressen enthalten.
// Aufruf: analyzeHeadless <projektordner> <projekt> -process <programm> -noanalysis -readOnly
//         -scriptPath tools\ghidra -postScript Decompile.java <ausgabe.txt> 0x... 0x...
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.FileWriter;
import java.io.PrintWriter;

public class Decompile extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; i++) {
                Address a = toAddr(args[i]);
                Function f = getFunctionContaining(a);
                if (f == null) {
                    // keine Funktion bekannt: an der Adresse anlegen
                    disassemble(a);
                    f = createFunction(a, null);
                }
                out.println("==== " + args[i] + " -> " + (f == null ? "keine Funktion" : f.getName() + " @ " + f.getEntryPoint()));
                if (f == null) continue;
                DecompileResults r = dec.decompileFunction(f, 120, monitor);
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "Dekompilieren fehlgeschlagen: " + r.getErrorMessage());
            }
        }
    }
}
