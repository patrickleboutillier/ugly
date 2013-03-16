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

