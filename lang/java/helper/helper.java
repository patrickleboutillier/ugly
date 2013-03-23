package ugly ;

import ugly.* ;


class helper {

	static UglyValue dispatch(UglyContext ctx, UglyRuntime rt, String cmd, UglyValue args[]){
		switch (cmd){
			case "call_function": {
				int extra = 2 ;
				UglyValue new_args[] = new UglyValue[args.length - extra] ;
				for (int i = 0 ; i < args.length - extra ; i++){
					new_args[i] = args[i + extra] ;
				}
				return call_function(ctx, rt, UglyType.wrap((int)args[0].getLong()), args[1].getString(), new_args) ;
			}
			default:
		}
				
		return new UglyValue(0) ;
	}


	//static UglyValue load_library(UglyContext ctx, UglyRuntime rt, String lib){
	//	return new UglyValue(1) ;
	//}


	static UglyValue call_function(UglyContext ctx, UglyRuntime rt, UglyType rtype, String func, 
		Object args[]){
		return new UglyValue(6) ;
	}


	static UglyValue new_object(UglyContext ctx, UglyRuntime rt, String clazz, Object args[]){
		return new UglyValue(1) ;
	}


	//static UglyValue call_method(UglyContext ctx, UglyRuntime rt, UglyObject obj, UglyType rtype, 
//		String method, Object args[]){
	//}
} ;
