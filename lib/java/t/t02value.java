import ugly.UglyValue ;
import ugly.UglyError ;

class t02value {
	public static void main(String[] args) {
		JTap jtap = new JTap() ;

		jtap.plan_tests(4) ;

		{
		UglyValue l = new UglyValue(42) ;
		jtap.ok(l.getLong() == 42, "Long value is 42") ;
		l.delete() ;
		}

		{
		UglyValue d = new UglyValue(42.6) ;
		jtap.ok(d.getDouble() == 42.6, "Double value is 42.6") ;
		d.delete() ;
		}

		{
		UglyValue s = new UglyValue("some string") ;
		jtap.ok(s.getString().equals("some string"), "String value is \"some string\"") ;
		s.delete() ;
		}

		{
		UglyValue s = new UglyValue((String)null) ;
		jtap.ok(s.getString() == null, "String value is null") ;
		s.delete() ;
		}
	}
}

