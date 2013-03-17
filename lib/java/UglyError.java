package ugly ;


public enum UglyError {
    NOT_SUPPORTED("NOT_SUPPORTED", -1000),
    NOT_IMPLEMENTED("NOT_IMPLEMENTED", -1001),
    OK("OK", 0),
    INTERNAL_ERROR("INTERNAL_ERROR", 1), 
    RUNTIME_ERROR("RUNTIMEL_ERROR", 2), 
    LIBRARY_ERROR("LIBRARY_ERROR", 3) ;

	private String name ;
	private int no ;
	UglyError(String name, int no){
		this.name = name ;
		this.no = no ;
	}

	public static UglyError wrap(int no){
		switch(no){
			case -1000: return NOT_SUPPORTED ; 
			case -1001: return NOT_IMPLEMENTED ; 
			case 0: return OK ;
			case 1: return INTERNAL_ERROR ; 
			case 2: return RUNTIME_ERROR ; 
			case 3: return LIBRARY_ERROR ; 
			default:
				throw new RuntimeException("Unknown UglyError " + no + "") ;
		}
	}

	public String getName(){
		return name ;
	}

	public String toString(){
		return getName() ;
	}

	public int getNo(){
		return no ;
	}
} ;

