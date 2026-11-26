package ugly;

import java.lang.reflect.Array;
import java.lang.reflect.Constructor;
import java.lang.reflect.Executable;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.Proxy;
import java.math.BigInteger;
import java.util.Arrays;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/**
 * The Java side of the plugin: the handle protocol over reflection. The C
 * plugin calls these static methods; NotFound, NotCapable and TypeMismatch
 * become the matching statuses, any other Throwable is a Java exception
 * (rule 7).
 *
 * Modules are classes: their static fields and methods, and their public
 * nested classes, are members; new() calls their constructors. Overloads
 * are chosen by scoring each candidate on how well the arguments convert,
 * the way Inline::Java did.
 */
public final class Bridge {
	private Bridge(){ }

	/* Traits, as in ugly.h. */
	static final int MEMBERS = 1, ELEMENTS = 2, CALLABLE = 4, INSTANTIABLE = 8;

	public static final class NotFound extends RuntimeException {
		private static final long serialVersionUID = 1L;
		NotFound(String what){ super(what); }
	}
	public static final class NotCapable extends RuntimeException {
		private static final long serialVersionUID = 1L;
		NotCapable(String what){ super(what); }
	}
	public static final class TypeMismatch extends RuntimeException {
		private static final long serialVersionUID = 1L;
		TypeMismatch(String what){ super(what); }
	}

	/** A method (static or bound to a target), as a callable value. */
	public static final class MethodRef {
		final Class<?> cls;
		final Object target;     // null: static
		final String name;
		MethodRef(Class<?> cls, Object target, String name){ this.cls = cls; this.target = target; this.name = name; }
		@Override public String toString(){ return cls.getSimpleName() + "." + name; }
	}

	/* ---------------------------------------------------------------- */
	/* Identity: the core dedups handles by an identity key per object. */
	/* ---------------------------------------------------------------- */

	private static final IdentityHashMap<Object, long[]> ids = new IdentityHashMap<>();
	private static long nextId = 1;

	/** A stable identity for o, held while some native reference to o is (counted). */
	static synchronized long acquireId(Object o){
		long[] e = ids.get(o);
		if (e == null){
			e = new long[] { nextId++, 0 };
			ids.put(o, e);
		}
		e[1]++;
		return e[0];
	}

	static synchronized void releaseId(Object o){
		long[] e = ids.get(o);
		if ((e != null)&&(--e[1] <= 0)){
			ids.remove(o);
		}
	}

	/* Static methods as callable values: one per (class, name), so they have a stable identity. */
	private static final Map<Class<?>, Map<String, MethodRef>> statics = new HashMap<>();

	private static synchronized MethodRef staticRef(Class<?> c, String name){
		return statics.computeIfAbsent(c, k -> new HashMap<>()).computeIfAbsent(name, n -> new MethodRef(c, null, n));
	}

	/* ---------------------------------------------------------------- */
	/* Describe                                                         */
	/* ---------------------------------------------------------------- */

	public static Class<?> importClass(String name) throws ClassNotFoundException {
		return Class.forName(name, true, ClassLoader.getSystemClassLoader());
	}

	static boolean instantiable(Class<?> c){
		return (! c.isInterface())&&(! Modifier.isAbstract(c.getModifiers()))&&(c.getConstructors().length > 0) ;
	}

	public static int traits(Object o){
		if (o instanceof Class) return MEMBERS | (instantiable((Class<?>)o) ? INSTANTIABLE : 0);
		if (o instanceof MethodRef) return CALLABLE;
		if (o instanceof Foreign) return ((Foreign)o).traits();
		int t = MEMBERS;
		if (o.getClass().isArray()||(o instanceof List)) t |= ELEMENTS;
		if (sam(o.getClass()) != null) t |= CALLABLE;
		return t;
	}

	/**
	 * What a value converts to, for the plugin's to_val (one call instead of
	 * an IsInstanceOf per type): 1 Boolean, 2 an integer that fits a long,
	 * 3 BigInteger, 4 Double or Float, 5 Character, 6 String, 7 byte[],
	 * 8 anything else. Keep in step with java.c's K_ enum.
	 */
	public static int kind(Object o){
		if ((o instanceof Long)||(o instanceof Integer)||(o instanceof Short)||(o instanceof Byte)) return 2;
		if (o instanceof String) return 6;
		if ((o instanceof Double)||(o instanceof Float)) return 4;
		if (o instanceof Boolean) return 1;
		if (o instanceof Character) return 5;
		if (o instanceof BigInteger) return 3;
		if (o instanceof byte[]) return 7;
		return 8;
	}

	public static String typeName(Object o){
		if (o instanceof Class) return instantiable((Class<?>)o) ? "class" : "module";
		if (o instanceof MethodRef) return "method";
		return o.getClass().getSimpleName();
	}

	/* ---------------------------------------------------------------- */
	/* Members                                                          */
	/* ---------------------------------------------------------------- */

	public static Object getMember(Object o, String name) throws Throwable {
		if (o instanceof Map){
			Map<?, ?> m = (Map<?, ?>)o;
			if (m.containsKey(name)) return m.get(name);
			throw new NotFound(name);
		}
		if (o instanceof Class){
			Class<?> c = (Class<?>)o;
			for (Class<?> n : c.getClasses()){
				if (n.getSimpleName().equals(name)) return n;
			}
			Field f = field(c, name, true);
			if (f != null) return f.get(null);
			if (methods(c, name, true).size() > 0) return staticRef(c, name);
			throw new NotFound(name);
		}
		Class<?> c = o.getClass();
		Field f = field(c, name, false);
		if (f != null) return f.get(o);
		if (methods(c, name, false).size() > 0) return new MethodRef(c, o, name);
		// A bean property: getName() / isName().
		String cap = Character.toUpperCase(name.charAt(0)) + name.substring(1);
		for (String g : new String[] { "get" + cap, "is" + cap }){
			for (Method m : methods(c, g, false)){
				if (m.getParameterCount() == 0) return unwrap(() -> accessible(m).invoke(o));
			}
		}
		throw new NotFound(name);
	}

	public static void setMember(Object o, String name, Object v) throws Throwable {
		if (o instanceof Map){
			@SuppressWarnings("unchecked") Map<Object, Object> m = (Map<Object, Object>)o;
			m.put(name, v);
			return;
		}
		boolean isStatic = (o instanceof Class);
		Class<?> c = (isStatic ? (Class<?>)o : o.getClass());
		Field f = field(c, name, isStatic);
		if (f == null) throw new NotFound(name);
		f.set(isStatic ? null : o, coerce(v, f.getType()));
	}

	/** A Map's entry; fields can't be removed. A missing key is not an error. */
	public static void deleteMember(Object o, String name){
		if (! (o instanceof Map)) throw new NotCapable("only a Map's members can be deleted");
		try {
			((Map<?, ?>)o).remove(name);
		}
		catch (UnsupportedOperationException e){
			throw new NotCapable("the map is unmodifiable");
		}
	}

	public static String[] memberNames(Object o){
		Set<String> names = new LinkedHashSet<>();
		if (o instanceof Map){
			for (Object k : ((Map<?, ?>)o).keySet()) if (k instanceof String) names.add((String)k);
		}
		else {
			boolean isStatic = (o instanceof Class);
			Class<?> c = (isStatic ? (Class<?>)o : o.getClass());
			if (isStatic) for (Class<?> n : c.getClasses()) names.add(n.getSimpleName());
			for (Field f : c.getFields()) if (Modifier.isStatic(f.getModifiers()) == isStatic) names.add(f.getName());
			for (Method m : c.getMethods()){
				if ((Modifier.isStatic(m.getModifiers()) == isStatic)&&(m.getDeclaringClass() != Object.class)) names.add(m.getName());
			}
		}
		return names.toArray(new String[0]);
	}

	/* ---------------------------------------------------------------- */
	/* Elements                                                         */
	/* ---------------------------------------------------------------- */

	public static int length(Object o){
		if (o.getClass().isArray()) return Array.getLength(o);
		if (o instanceof List) return ((List<?>)o).size();
		throw new NotCapable("value has no elements");
	}

	public static Object getIndex(Object o, int i){
		int n = length(o);
		if ((i < 0)||(i >= n)) throw new NotFound("index out of range");
		return (o instanceof List ? ((List<?>)o).get(i) : Array.get(o, i));
	}

	public static void setIndex(Object o, int i, Object v){
		int n = length(o);
		if (o instanceof List){
			@SuppressWarnings("unchecked") List<Object> l = (List<Object>)o;
			if ((i < 0)||(i > n)) throw new NotFound("index out of range");
			if (i == n) l.add(v); else l.set(i, v);     // one past the end appends
			return;
		}
		if ((i < 0)||(i >= n)) throw new NotFound("index out of range");
		Array.set(o, i, coerce(v, o.getClass().getComponentType()));
	}

	/** Replaces 'remove' elements at 'offset' (checked by the core) with 'insert'. Lists only: arrays have a fixed size. */
	public static void splice(Object o, int offset, int remove, Object[] insert){
		if (! (o instanceof List)){
			throw new NotCapable(o.getClass().isArray() ? "a Java array has a fixed size" : "value has no elements");
		}
		@SuppressWarnings("unchecked") List<Object> l = (List<Object>)o;
		try {
			l.subList(offset, offset + remove).clear();
			l.addAll(offset, Arrays.asList(insert));
		}
		catch (UnsupportedOperationException e){
			throw new NotCapable("the list can't change size");
		}
	}

	/* ---------------------------------------------------------------- */
	/* Calls                                                            */
	/* ---------------------------------------------------------------- */

	public static Object call(Object f, Object[] args) throws Throwable {
		if (f instanceof MethodRef){
			MethodRef r = (MethodRef)f;
			return invokeOn(r.cls, r.target, r.name, args);
		}
		if (f instanceof Foreign) return ((Foreign)f).call(args);
		Method m = sam(f.getClass());
		if (m == null) throw new NotCapable("value is not callable");
		Object[] a = convert(m.getParameterTypes(), args);
		if (a == null) throw new TypeMismatch("arguments don't fit " + m);
		return unwrap(() -> m.invoke(f, a));
	}

	public static Object invoke(Object o, String name, Object[] args) throws Throwable {
		if (o instanceof Class) return invokeOn((Class<?>)o, null, name, args);
		if (o instanceof Foreign) return ((Foreign)o).invoke(name, args);
		return invokeOn(o.getClass(), o, name, args);
	}

	public static Object newInstance(Object cls, Object[] args) throws Throwable {
		if (! (cls instanceof Class)) throw new NotCapable("not a class");
		Class<?> c = (Class<?>)cls;
		Overloads ctors = constructors.computeIfAbsent(c, k -> new Overloads(List.of(k.getConstructors())));
		if (ctors.methods.length == 0) throw new NotCapable(c.getName() + " has no public constructor");
		int best = choose(ctors, args, c.getSimpleName());
		Object[] a = convert(ctors.params[best], args);
		Constructor<?> ctor = (Constructor<?>)ctors.methods[best];
		return unwrap(() -> ctor.newInstance(a));
	}

	private static Object invokeOn(Class<?> c, Object target, String name, Object[] args) throws Throwable {
		Overloads cands = methodCache.computeIfAbsent(new Key(c, name, target == null), k -> {
			List<Executable> ms = new ArrayList<>();
			for (Method m : methods(k.cls(), k.name(), k.isStatic())) ms.add(accessible(m));
			return new Overloads(ms);
		});
		if (cands.methods.length == 0) throw new NotFound(name);
		int best = choose(cands, args, name);
		Object[] a = convert(cands.params[best], args);
		Method m = (Method)cands.methods[best];
		return unwrap(() -> m.invoke(target, a));
	}

	/*
	 * Resolution is cached: the candidates of each (class, name, static),
	 * with their parameter types, and, for calls of up to 4 arguments, the
	 * overload chosen for each signature: the arguments' classes, and for a
	 * Long which narrower types it fits, for a String whether it could be a
	 * char (what score() looks at besides classes).
	 */
	static final class Overloads {
		final Executable[] methods;
		final Class<?>[][] params;
		final ConcurrentHashMap<Sig, Integer> chosen = new ConcurrentHashMap<>();
		Overloads(List<? extends Executable> ms){
			methods = ms.toArray(new Executable[0]);
			params = new Class<?>[methods.length][];
			for (int i = 0 ; i < methods.length ; i++) params[i] = methods[i].getParameterTypes();
		}
	}

	record Key(Class<?> cls, String name, boolean isStatic) { }
	record Sig(int n, Class<?> c0, int e0, Class<?> c1, int e1, Class<?> c2, int e2, Class<?> c3, int e3) { }

	private static final ConcurrentHashMap<Key, Overloads> methodCache = new ConcurrentHashMap<>();
	private static final ConcurrentHashMap<Class<?>, Overloads> constructors = new ConcurrentHashMap<>();

	private static Class<?> sigClass(Object a){ return (a == null ? null : a.getClass()); }

	private static int sigExtra(Object a){
		if (a instanceof Long){
			long v = (Long)a;
			return (fits(v, Integer.class) ? 1 : 0) | (fits(v, Short.class) ? 2 : 0) | (fits(v, Byte.class) ? 4 : 0);
		}
		if (a instanceof String) return (((String)a).length() == 1 ? 1 : 0);
		return 0;
	}

	private static Sig sigOf(Object[] args){
		int n = args.length;
		if (n > 4) return null;
		return new Sig(n,
			n > 0 ? sigClass(args[0]) : null, n > 0 ? sigExtra(args[0]) : 0,
			n > 1 ? sigClass(args[1]) : null, n > 1 ? sigExtra(args[1]) : 0,
			n > 2 ? sigClass(args[2]) : null, n > 2 ? sigExtra(args[2]) : 0,
			n > 3 ? sigClass(args[3]) : null, n > 3 ? sigExtra(args[3]) : 0);
	}

	/* ---------------------------------------------------------------- */
	/* Overload resolution: score how well each argument converts       */
	/* ---------------------------------------------------------------- */

	private static final int NO = Integer.MAX_VALUE;

	/*
	 * The hint call option (contract §11): the parameter types of the
	 * overload to use, as Class.getName() gives them, joined by commas
	 * ("int,java.lang.String"). Set by the plugin for one call; the call's
	 * own overload resolution takes it.
	 */
	private static final ThreadLocal<String> HINT = new ThreadLocal<>();

	static void hint(String h){
		if (h == null) HINT.remove(); else HINT.set(h.replace(" ", ""));
	}

	private static String signature(Class<?>[] ps){
		StringBuilder b = new StringBuilder();
		for (Class<?> p : ps){
			if (b.length() > 0) b.append(',');
			b.append(p.getName());
		}
		return b.toString();
	}

	/** The index of the overload to call. */
	private static int choose(Overloads o, Object[] args, String what){
		String hint = HINT.get();
		HINT.remove();
		if (hint != null){
			for (int j = 0 ; j < o.methods.length ; j++){
				Class<?>[] ps = o.params[j];
				if (signature(ps).equals(hint)){
					if (ps.length != args.length) throw new TypeMismatch(what + "(" + hint + ") takes " + ps.length + " argument(s)");
					for (int i = 0 ; i < ps.length ; i++){
						if (score(args[i], ps[i]) == NO) throw new TypeMismatch("argument " + (i + 1) + " doesn't fit " + what + "(" + hint + ")");
					}
					return j;
				}
			}
			throw new NotFound("no overload " + what + "(" + hint + ")");
		}
		Sig sig = sigOf(args);
		if (sig != null){
			Integer known = o.chosen.get(sig);
			if (known != null) return known;
		}
		int best = -1, bestScore = NO, ties = 0;
		for (int j = 0 ; j < o.methods.length ; j++){
			Class<?>[] ps = o.params[j];
			if (ps.length != args.length) continue;
			int s = 0;
			for (int i = 0 ; (i < ps.length)&&(s != NO) ; i++){
				int k = score(args[i], ps[i]);
				s = (k == NO ? NO : s + k);
			}
			if (s == NO) continue;
			if (s < bestScore){ best = j ; bestScore = s ; ties = 0 ; }
			else if (s == bestScore){ ties++ ; }
		}
		if (best < 0) throw new TypeMismatch("no overload of " + what + " takes these " + args.length + " argument(s)");
		if (ties > 0) throw new TypeMismatch("ambiguous call to " + what + ": " + (ties + 1) + " overloads fit equally well");
		if (sig != null) o.chosen.putIfAbsent(sig, best);
		return best;
	}

	private static Class<?> box(Class<?> p){
		if (p == int.class) return Integer.class;
		if (p == long.class) return Long.class;
		if (p == double.class) return Double.class;
		if (p == float.class) return Float.class;
		if (p == boolean.class) return Boolean.class;
		if (p == short.class) return Short.class;
		if (p == byte.class) return Byte.class;
		if (p == char.class) return Character.class;
		return p;
	}

	private static boolean fits(long v, Class<?> t){
		if ((t == Long.class)||(t == BigInteger.class)) return true;
		if (t == Integer.class) return (v >= Integer.MIN_VALUE)&&(v <= Integer.MAX_VALUE);
		if (t == Short.class) return (v >= Short.MIN_VALUE)&&(v <= Short.MAX_VALUE);
		if (t == Byte.class) return (v >= Byte.MIN_VALUE)&&(v <= Byte.MAX_VALUE);
		return false;
	}

	/** Lower is better; NO if 'a' can't be passed as 'p'. */
	static int score(Object a, Class<?> p){
		if (a == null) return (p.isPrimitive() ? NO : 1);
		Class<?> t = box(p);
		if (a.getClass() == t) return 0;
		if (a instanceof Long){
			long v = (Long)a;
			if (fits(v, t)) return 1;
			if ((t == Double.class)||(t == Float.class)) return 3;
		}
		if ((a instanceof Double)&&(t == Float.class)) return 2;
		if ((a instanceof String)&&(t == Character.class)&&(((String)a).length() == 1)) return 3;
		if (t.isInstance(a)) return (t == Object.class ? 6 : 2);
		if ((a instanceof Foreign)&&(t.isInterface())) return 5;
		if ((a instanceof MethodRef)&&(samOf(t) != null)) return 5;
		return NO;
	}

	/** Converts args to the chosen parameter types (null if one doesn't fit). */
	static Object[] convert(Class<?>[] ps, Object[] args){
		Object[] out = new Object[args.length];
		for (int i = 0 ; i < args.length ; i++){
			if (score(args[i], ps[i]) == NO) return null;
			out[i] = coerce(args[i], ps[i]);
		}
		return out;
	}

	static Object coerce(Object a, Class<?> p){
		if (a == null) return null;
		Class<?> t = box(p);
		if (t.isInstance(a)) return a;
		if (a instanceof Long){
			long v = (Long)a;
			if (t == Integer.class) return (int)v;
			if (t == Short.class) return (short)v;
			if (t == Byte.class) return (byte)v;
			if (t == Double.class) return (double)v;
			if (t == Float.class) return (float)v;
			if (t == BigInteger.class) return BigInteger.valueOf(v);
		}
		if ((a instanceof Double)&&(t == Float.class)) return ((Double)a).floatValue();
		if ((a instanceof String)&&(t == Character.class)) return ((String)a).charAt(0);
		if ((a instanceof Foreign)&&(t.isInterface())) return proxy((Foreign)a, t);
		if ((a instanceof MethodRef)&&(samOf(t) != null)){
			// A Java method passed where a functional interface is expected: implement it by calling the method.
			final MethodRef ref = (MethodRef)a;
			final Method sam = samOf(t);
			return Proxy.newProxyInstance(t.getClassLoader() != null ? t.getClassLoader() : Bridge.class.getClassLoader(),
				new Class<?>[] { t }, (self, m, args) -> {
					if (m.equals(sam)){
						Object r = call(ref, (args != null ? args : new Object[0]));
						return ((m.getReturnType() == void.class)||(r == null) ? null : coerce(r, m.getReturnType()));
					}
					return (m.getName().equals("hashCode") ? System.identityHashCode(self) : m.getName().equals("equals") ? (self == args[0]) : ref.toString());
				});
		}
		throw new TypeMismatch("can't convert " + a.getClass().getSimpleName() + " to " + p.getSimpleName());
	}

	/* ---------------------------------------------------------------- */
	/* Interfaces implemented by foreign values (dynamic proxies)      */
	/* ---------------------------------------------------------------- */

	static final class ForeignHandler implements InvocationHandler {
		final Foreign foreign;
		final Method sam;
		ForeignHandler(Foreign f, Method sam){ this.foreign = f ; this.sam = sam ; }

		@Override
		public Object invoke(Object proxy, Method m, Object[] args){
			Object[] a = (args != null ? args : new Object[0]);
			if (m.getDeclaringClass() == Object.class){
				switch (m.getName()){
					case "equals": return (a[0] != null)&&Proxy.isProxyClass(a[0].getClass())
						&&(Proxy.getInvocationHandler(a[0]) instanceof ForeignHandler)
						&&foreign.equals(((ForeignHandler)Proxy.getInvocationHandler(a[0])).foreign);
					case "hashCode": return foreign.hashCode();
					default: return foreign.toString();
				}
			}
			// The interface's single abstract method calls the foreign value; any other method invokes it by name.
			Object r = (m.equals(sam) ? foreign.call(a) : foreign.invoke(m.getName(), a));
			Class<?> rt = m.getReturnType();
			return ((rt == void.class)||(r == null) ? null : coerce(r, rt));
		}
	}

	static Object proxy(Foreign f, Class<?> iface){
		return Proxy.newProxyInstance(iface.getClassLoader() != null ? iface.getClassLoader() : Bridge.class.getClassLoader(),
			new Class<?>[] { iface }, new ForeignHandler(f, samOf(iface)));
	}

	/** The foreign value behind a dynamic proxy (rule 5: unwrap when it goes out), or null. */
	public static Foreign foreignOf(Object o){
		if ((o != null)&&Proxy.isProxyClass(o.getClass())){
			InvocationHandler h = Proxy.getInvocationHandler(o);
			if (h instanceof ForeignHandler) return ((ForeignHandler)h).foreign;
		}
		return null;
	}

	/* ---------------------------------------------------------------- */
	/* Reflection helpers                                               */
	/* ---------------------------------------------------------------- */

	private static Field field(Class<?> c, String name, boolean isStatic){
		try {
			Field f = c.getField(name);
			return (Modifier.isStatic(f.getModifiers()) == isStatic ? f : null);
		}
		catch (NoSuchFieldException e){
			return null;
		}
	}

	private static List<Method> methods(Class<?> c, String name, boolean isStatic){
		List<Method> out = new ArrayList<>();
		for (Method m : c.getMethods()){
			if (m.getName().equals(name)&&(Modifier.isStatic(m.getModifiers()) == isStatic)) out.add(m);
		}
		return out;
	}

	/** The single abstract method of a functional interface, or null. */
	private static Method samOf(Class<?> iface){
		if (! iface.isInterface()) return null;
		Method found = null;
		for (Method m : iface.getMethods()){
			if (! Modifier.isAbstract(m.getModifiers())) continue;
			try {
				Object.class.getMethod(m.getName(), m.getParameterTypes());
				continue;     // equals, hashCode... redeclared: not the SAM
			}
			catch (NoSuchMethodException e){
				// a real abstract method
			}
			if (found != null) return null;
			found = m;
		}
		return found;
	}

	/** The SAM of a functional interface that c implements (a lambda's class), or null. */
	private static Method sam(Class<?> c){
		for (Class<?> i : c.getInterfaces()){
			Method m = samOf(i);
			if (m != null) return m;
		}
		return null;
	}

	/** A method that can be invoked: the same one on a public supertype if its class isn't public. */
	static Method accessible(Method m){
		if (Modifier.isPublic(m.getDeclaringClass().getModifiers())) return m;
		for (Class<?> c = m.getDeclaringClass() ; c != null ; c = c.getSuperclass()){
			for (Class<?> i : c.getInterfaces()){
				try { return accessible(i.getMethod(m.getName(), m.getParameterTypes())); }
				catch (NoSuchMethodException e){ /* not there */ }
			}
			if ((c != m.getDeclaringClass())&&Modifier.isPublic(c.getModifiers())){
				try { return c.getMethod(m.getName(), m.getParameterTypes()); }
				catch (NoSuchMethodException e){ /* not there */ }
			}
		}
		return m;
	}

	interface Reflective { Object run() throws Exception; }

	/** Runs a reflective call, rethrowing what the target threw as itself. */
	static Object unwrap(Reflective r) throws Throwable {
		try {
			return r.run();
		}
		catch (InvocationTargetException e){
			throw e.getCause();
		}
		catch (IllegalArgumentException e){
			throw new TypeMismatch(e.getMessage());
		}
	}
}
