package ugly ;

import ugly.Ugly ;


class helper {

	static UglyValue dispatch(UglyContext ctx, UglyRuntime rt, String cmd, UglyValue args[]){
		Ugly.debug(3, "JAVA: " + rt + " " + cmd + " ") ;
		for (int i = 0 ; i < args.length ; i++){
			Ugly.debug(3, args[i].toString()) ;
		}
				
		return new UglyValue(1) ;
	}


	static UglyValue load_library(UglyContext ctx, UglyRuntime rt, String lib){
		return new UglyValue(1) ;
	}


	static UglyValue call_function(UglyContext ctx, UglyRuntime rt, UglyType rtype, String func, 
		Object args[]){
		return new UglyValue(1) ;
	}


	static UglyValue new_object(UglyContext ctx, UglyRuntime rt, String clazz, Object args[]){
		return new UglyValue(1) ;
	}


	//static UglyValue call_method(UglyContext ctx, UglyRuntime rt, UglyObject obj, UglyType rtype, 
//		String method, Object args[]){
	//}
} ;
