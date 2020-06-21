package org.winehq.wine;

import java.io.File;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

public class sharedwine
{
    private static native String wine_init( String[] cmdline, String[] env );

    private static final void runWine( String cmdline, HashMap<String,String> environ )
    {
        String[] env = new String[environ.size() * 2];
        int j = 0;
        for (Map.Entry<String,String> entry : environ.entrySet())
        {
            env[j++] = entry.getKey();
            env[j++] = entry.getValue();
        }

        String[] cmd = { environ.get( "WINELOADER" ),
                         "explorer.exe",
                         "/desktop=shell,,x11",
                         cmdline };

        String err = wine_init( cmd, env );
        System.err.println(err);
    }

    private static void loadWine( String root, String cmdline )
    {
        File bindir = new File(root, "/bin");
        File libdir = new File(root, "/lib64");
        File dlldir = new File(libdir, "/wine");
        File loader = new File(bindir, "wine64");
        String locale = Locale.getDefault().getLanguage() + "_" +
                        Locale.getDefault().getCountry() + ".UTF-8";
        File prefix = new File(System.getenv("WINEPREFIX"));

        HashMap<String, String> env = new HashMap<String, String>();
        env.put("WINELOADER", loader.toString());
        /* env.put("WINEPREFIX", */
        env.put("WINEDLLPATH", dlldir.toString());
        /* env.put("LD_LIBRARY_PATH", libdir.toString() + ":" + System.getenv("LD_LIBRARY_PATH")); */
        env.put("LC_ALL", locale);
        env.put("LANG", locale);
        /* env.put("PATH", bindir.toString() + ":" + System.getenv("PATH)); */

        if (cmdline == null)
        {
            if (new File(prefix, "drive_c/winestart.cmd").exists()) cmdline = "c:\\winestart.cmd";
            else cmdline = "wineconsole.exe";
        }

        String winedebug = "-all";
        env.put("WINEDEBUG", winedebug);
        /* env.put("WINEDEBUGLOG", ); */

        try
        {
            // TODO System.loadLibrary("wine");
            System.load(libdir.toString() + "/libwine.so");
        }
        catch (java.lang.UnsatisfiedLinkError e)
        {
            System.load(libdir.toString() + "/libwine.so");
        }
        /* prefix.mkdirs(); */

        runWine(cmdline, env);
    }

    public static void initializeWine(String root, String cmdline)
    {
        try
        {
             new Thread( new Runnable() { public void run() { loadWine( root, cmdline ); }} ).start();
        }
        catch (Exception e)
        {
            e.printStackTrace();
        }
    }
}
