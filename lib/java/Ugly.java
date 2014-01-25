package ugly ;


public class Ugly { 
	private static int debug = 0 ;
	static {
		try {
			String v = System.getenv("UGLY_DEBUG") ;
			debug = Integer.parseInt(v) ;
		}
		catch (NumberFormatException re){
			// debug stays 0...
		}
	}


	public synchronized static void setDebugLevel(int d){
		debug = d ;
	}


	public static int getDebugLevel(){
		return debug ;
	}

	public synchronized static void debug(int level, String s) {
		if ((debug > 0)&&(debug >= level)){
			String sname = new String("java" + "    ") ;
			System.err.print("# DEBUG(" + sname + "): ") ;
			StringBuffer sb = new StringBuffer() ;
			for (int i = 0 ; i < level ; i++){
				sb.append(" ") ;
			}
			System.err.println(sb.toString() + s) ;
			System.err.flush() ;
		}
	}
}
