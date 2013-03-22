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
	private UglyType(String name, int no){
		this.name = name ;
		this.no = no ;
	}

	public static UglyType wrap(int no){
        switch(no){
            case 0: return BOOL ;
            case 1: return LONG ;
            case 2: return DOUBLE ;
            case 3: return CHAR ;
            case 4: return STRING ;
            case 5: return OBJECT ;
            default:
                throw new RuntimeException("Unknown UglyType " + no + "") ;
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

