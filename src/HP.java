import java.io.*;

public class HP 
{
    public static void main(String args[]) throws Exception 
	{
	   Runtime r = Runtime.getRuntime();
	   Process p = r.exec("Debug/hpctrl.exe -i");
	   
	   BufferedReader readEnd = new BufferedReader(new InputStreamReader(p.getInputStream()));
	   BufferedWriter writeEnd = new BufferedWriter(new OutputStreamWriter(p.getOutputStream()));
	      
	   writeEnd.write("help");
	   writeEnd.newLine();
	   writeEnd.flush();
	   Thread.sleep(1000);
	   while (readEnd.ready())
	   {
		   System.out.print((char)readEnd.read());
	   }		   
	   
	   writeEnd.write("connect");
	   writeEnd.newLine();
	   writeEnd.flush();
	   Thread.sleep(1000);
	   while (readEnd.ready())
	   {
		   System.out.print((char)readEnd.read());
	   }
		
	   writeEnd.write("getstate");
	   writeEnd.newLine();
	   writeEnd.flush();
	   Thread.sleep(1000);
	   while (readEnd.ready())
	   {
		   System.out.print((char)readEnd.read());
	   }
	   
	   writeEnd.write("exit");
	   writeEnd.newLine();
	   writeEnd.flush();
	   Thread.sleep(1000);
	   while (readEnd.ready())
	   {
		   System.out.print((char)readEnd.read());
	   }
	}
	
}