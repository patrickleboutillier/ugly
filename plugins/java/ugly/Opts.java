package ugly;

/**
 * Call options for a call into another language (contract §11), passed as
 * the last argument (beside a {@link Kw} if there are keyword arguments too):
 * {@code perl.invoke("localtime", 0, Ugly.opts().context("scalar"))},
 * {@code java.invoke("over", 1, Ugly.opts().hint("int"))}.
 */
public final class Opts {
	String hint, context;

	Opts(){ }

	/** The overload to call, by parameter types ("int,java.lang.String"): honoured by Java. */
	public Opts hint(String h){ hint = h; return this; }

	/** "list", "scalar" or "void": honoured by Perl. */
	public Opts context(String c){ context = c; return this; }
}
