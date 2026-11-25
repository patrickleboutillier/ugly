package ugly;

import java.lang.ref.Cleaner;

/**
 * An error from another language, thrown into Java: its type, message and
 * language, and the original exception object (payload, often a Foreign).
 * If it escapes Java unhandled, the original error goes on unchanged.
 */
public final class ForeignException extends RuntimeException {
	private static final long serialVersionUID = 1L;
	private static final Cleaner cleaner = Cleaner.create();

	/**
	 * The core's error while this exception owns it ([0] = 0 once it has
	 * been taken back, or for a rethrown payload). An array, so the cleaner
	 * can see it without keeping the exception alive.
	 */
	private final long[] err;
	private final String type, language;
	private final transient Object payload;

	ForeignException(long err, String type, String message, String language, Object payload){
		super(message);
		this.err = new long[] { err };
		this.type = type;
		this.language = language;
		this.payload = payload;
		if (err != 0){
			final long[] holder = this.err;
			cleaner.register(this, () -> { if (holder[0] != 0) Foreign.freeError(holder[0]); });
		}
	}

	/** Rethrows a foreign exception object (a Foreign): the payload is that value itself. */
	public ForeignException(Object payload){
		this(0, "ForeignError", "rethrown foreign exception", "java", payload);
	}

	/** Called by the plugin when the error leaves Java: it takes the core's error back. */
	long take(){
		long e = err[0];
		err[0] = 0;
		return e;
	}

	public String type(){ return type; }
	public String language(){ return language; }
	public Object payload(){ return payload; }
}
