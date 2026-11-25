package ugly;

import java.util.Arrays;
import java.util.Map;

/**
 * Keyword arguments for a call into another language (contract §11), passed
 * as the last argument: {@code json.invoke("dumps", data, Ugly.kw("indent", 2))}.
 * Made by {@link Ugly#kw(Object...)} or {@link Ugly#kw(Map)}; the order is kept.
 */
public final class Kw {
	final String[] names;
	final Object[] values;

	Kw(String[] names, Object[] values){
		this.names = names;
		this.values = values;
	}

	static Kw of(Object... pairs){
		if (pairs.length % 2 != 0) throw new IllegalArgumentException("Ugly.kw(name, value, ...): pairs expected");
		String[] n = new String[pairs.length / 2];
		Object[] v = new Object[pairs.length / 2];
		for (int i = 0; i < n.length; i++){
			if (! (pairs[2 * i] instanceof String)) throw new IllegalArgumentException("Ugly.kw: names are strings");
			n[i] = (String)pairs[2 * i];
			v[i] = pairs[2 * i + 1];
		}
		return new Kw(n, v);
	}

	static Kw of(Map<String, ?> map){
		String[] n = map.keySet().toArray(new String[0]);
		Object[] v = new Object[n.length];
		for (int i = 0; i < n.length; i++) v[i] = map.get(n[i]);
		return new Kw(n, v);
	}

	/**
	 * A call's trailing markers: a Kw and an Opts, at most one of each, in
	 * either order. 'args' is what's left.
	 */
	static final class Split {
		final Object[] args;
		final Kw kw;
		final Opts opts;

		Split(Object[] a){
			Kw k = null;
			Opts o = null;
			int n = (a == null ? 0 : a.length);
			while (n > 0){
				Object last = a[n - 1];
				if ((o == null)&&(last instanceof Opts)) o = (Opts)last;
				else if ((k == null)&&(last instanceof Kw)) k = (Kw)last;
				else break;
				n--;
			}
			args = (a == null || n == a.length) ? a : Arrays.copyOf(a, n);
			kw = k;
			opts = o;
		}

		boolean plain(){ return (kw == null)&&(opts == null); }
		String[] names(){ return (kw == null) ? new String[0] : kw.names; }
		Object[] values(){ return (kw == null) ? new Object[0] : kw.values; }
		String hint(){ return (opts == null) ? null : opts.hint; }
		String context(){ return (opts == null) ? null : opts.context; }
	}
}
