package ugly ;


public enum UglyType {
   	BOOL("BOOL", 0), 
   	LONG("LONG", 1), 
	DOUBLE("DOUBLE", 2), 
   	CHAR("CHAR", 3), 
	STRING("STRING", 4),
	OBJECT("OBJECT", 5) ;

	private String name ;
	private int no ;
	UglyType(String name, int no){
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

