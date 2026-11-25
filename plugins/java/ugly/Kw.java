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

	/** The Kw that ends args, or null. */
	static Kw last(Object[] args){
		return ((args != null)&&(args.length > 0)&&(args[args.length - 1] instanceof Kw)) ? (Kw)args[args.length - 1] : null;
	}

	static Object[] rest(Object[] args){
		return Arrays.copyOf(args, args.length - 1);
	}
}
