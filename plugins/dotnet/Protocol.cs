// The handle protocol over reflection, as the Java plugin's Bridge.java does it.
//
// Modules are types: a static class is a "module", a type with a public
// constructor a "class" (INSTANTIABLE). Their static fields, properties and
// methods, and their public nested types, are members. An object's members
// are its public fields, properties and methods; a dictionary's are its
// string keys. Arrays and lists have elements.
//
// Overloads are chosen by scoring how well each argument converts (lower is
// better), as Inline::Java did. Keyword arguments bind to parameters by
// name, and parameters left out keep their defaults: C#'s named and
// optional arguments.
using System.Collections;
using System.Collections.Concurrent;
using System.Linq.Expressions;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace Ugly;

// A method (static, or bound to a target), as a callable value.
sealed class MethodRef(Type cls, object? target, string name) {
    public Type Cls { get; } = cls;
    public object? Target { get; } = target;
    public string Name { get; } = name;
    public override string ToString() => $"{Cls.Name}.{Name}";
}

static class Protocol {
    public const uint MEMBERS = 1, ELEMENTS = 2, CALLABLE = 4, INSTANTIABLE = 8, ITERABLE = 32, ENTRIES = 64;

    const BindingFlags Static = BindingFlags.Public | BindingFlags.Static;
    const BindingFlags Instance = BindingFlags.Public | BindingFlags.Instance;
    const BindingFlags NoWrap = BindingFlags.DoNotWrapExceptions;

    /*
     * The hint call option (contract §11): the parameter types of the
     * overload to use, by C# alias or full name ("int,string" or
     * "System.Int32,System.String"). Set for one call; its overload
     * resolution takes it.
     */
    [ThreadStatic] internal static string? Hint;

    /* ---------------------------------------------------------------- */
    /* Describe                                                         */
    /* ---------------------------------------------------------------- */

    static bool Instantiable(Type t) =>
        !t.IsAbstract && !t.IsInterface && !t.ContainsGenericParameters
        && (t.IsValueType || t.GetConstructors().Length > 0);

    public static uint Traits(object o) {
        if (o is Type t) return MEMBERS | (Instantiable(t) ? INSTANTIABLE : 0);
        if (o is MethodRef) return CALLABLE;
        uint traits = MEMBERS;
        if (o is Delegate) traits |= CALLABLE;
        if ((o is Array a && a.Rank == 1) || (o is IList)) traits |= ELEMENTS;
        if ((o is IEnumerable) || (o is IEnumerator)) traits |= ITERABLE;
        return traits;
    }

    /* ---------------------------------------------------------------- */
    /* Iteration (contract §2)                                          */
    /* ---------------------------------------------------------------- */

    // The enumerators Iter handed out: their own iterators, disposed of when released.
    static readonly ConditionalWeakTable<object, object> enumerators = new();
    static readonly object mark = new();

    internal static bool Iterating(object o) => enumerators.TryGetValue(o, out _);

    /*
     * What foreach takes: an enumerable gives GetEnumerator(); an enumerator
     * is its own. A 'yield return' method's object is both, and needs its
     * GetEnumerator() first (until then MoveNext() gives nothing): so an
     * object that is both is taken as an enumerator only once Iter has
     * handed it out.
     */
    public static object Iter(object o) {
        if ((o is IEnumerator) && ((o is not IEnumerable) || Iterating(o))) return o;
        if (o is IEnumerable en) {
            IEnumerator e = en.GetEnumerator();
            // Such an object gives itself the first time: handed out as it is, it would be
            // the enumerable's own handle, spent after one pass and never disposed of on
            // its own. An iterator of its own, then; the next pass gets a fresh one.
            if (ReferenceEquals(e, o)) e = new Pass(e);
            enumerators.AddOrUpdate(e, mark);
            return e;
        }
        throw Fail.NotCapable("value is not enumerable");
    }

    // One pass over an object that is its own enumerator. Enumerable too, as that object
    // is: it comes home as an argument where an IEnumerable is taken, and goes on from where it is.
    sealed class Pass(IEnumerator inner) : IEnumerator, IEnumerable, IDisposable {
        public IEnumerator GetEnumerator() => this;
        public object? Current => inner.Current;
        public bool MoveNext() => inner.MoveNext();
        public void Reset() => inner.Reset();
        public void Dispose() => (inner as IDisposable)?.Dispose();
    }

    // A foreign value as an IEnumerable<T>: its iteration, each element coerced as it comes.
    static IEnumerable<T> Typed<T>(IEnumerable source) {
        foreach (object? x in source) yield return (T)Coerce(x, typeof(T))!;
    }

    static readonly MethodInfo typed = typeof(Protocol).GetMethod(nameof(Typed), BindingFlags.NonPublic | BindingFlags.Static)!;

    // IEnumerable<T>'s T, if t is that interface.
    static Type? EnumerableOf(Type t) =>
        t.IsGenericType && (t.GetGenericTypeDefinition() == typeof(IEnumerable<>)) ? t.GetGenericArguments()[0] : null;

    public static string TypeName(object o) => o switch {
        Type t => Instantiable(t) ? "class" : "module",
        MethodRef => "method",
        _ => o.GetType().Name,
    };

    /* ---------------------------------------------------------------- */
    /* Members                                                          */
    /* ---------------------------------------------------------------- */

    static PropertyInfo? Property(Type t, string name, BindingFlags flags) {
        foreach (PropertyInfo p in t.GetProperties(flags)) {
            if ((p.Name == name) && (p.GetIndexParameters().Length == 0)) return p;
        }
        return null;
    }

    static bool HasMethods(Type t, string name, BindingFlags flags) {
        foreach (MethodInfo m in t.GetMethods(flags)) {
            if ((m.Name == name) && !m.IsSpecialName) return true;
        }
        return false;
    }

    // Static methods as callable values: one per (type, name), so they keep an identity.
    static readonly ConcurrentDictionary<(Type, string), MethodRef> statics = new();

    public static object? GetMember(object o, string name) {
        if (o is IDictionary d) {
            if (d.Contains(name)) return d[name];
            throw Fail.NotFound(name);
        }
        if (o is IDictionary<string, object?> gd) {
            if (gd.TryGetValue(name, out object? v)) return v;
            throw Fail.NotFound(name);
        }
        bool isStatic = o is Type;
        Type t = isStatic ? (Type)o : o.GetType();
        object? target = isStatic ? null : o;
        BindingFlags flags = isStatic ? Static : Instance;
        if (isStatic && (t.GetNestedType(name, BindingFlags.Public) is Type nested)) return nested;
        if (t.GetField(name, flags) is FieldInfo f) return f.GetValue(target);
        if (Property(t, name, flags) is PropertyInfo p && p.CanRead) return p.GetValue(target, NoWrap, null, null, null);
        if (HasMethods(t, name, flags)) return isStatic ? statics.GetOrAdd((t, name), k => new MethodRef(k.Item1, null, k.Item2)) : new MethodRef(t, o, name);
        throw Fail.NotFound(name);
    }

    public static void SetMember(object o, string name, object? v) {
        if (o is IDictionary d) {
            if (d.IsReadOnly) throw Fail.NotCapable("the dictionary is read-only");
            d[name] = Coerce(v, ValueType(o.GetType(), typeof(IDictionary<,>), 1));
            return;
        }
        if (o is IDictionary<string, object?> gd) {
            gd[name] = v;
            return;
        }
        bool isStatic = o is Type;
        Type t = isStatic ? (Type)o : o.GetType();
        object? target = isStatic ? null : o;
        BindingFlags flags = isStatic ? Static : Instance;
        if (t.GetField(name, flags) is FieldInfo f) {
            if (f.IsInitOnly || f.IsLiteral) throw Fail.NotCapable($"{name} is read-only");
            f.SetValue(target, Coerce(v, f.FieldType));
            return;
        }
        if (Property(t, name, flags) is PropertyInfo p) {
            if (!p.CanWrite) throw Fail.NotCapable($"{name} is read-only");
            p.SetValue(target, Coerce(v, p.PropertyType), NoWrap, null, null, null);
            return;
        }
        throw Fail.NotFound(name);
    }

    // A dictionary's entry; fields and properties can't be removed. A missing key is not an error.
    public static void DeleteMember(object o, string name) {
        if (o is IDictionary d) {
            if (d.IsReadOnly || d.IsFixedSize) throw Fail.NotCapable("the dictionary can't change size");
            d.Remove(name);
            return;
        }
        if (o is IDictionary<string, object?> gd) {
            gd.Remove(name);
            return;
        }
        throw Fail.NotCapable("only a dictionary's members can be deleted");
    }

    public static List<string> MemberNames(object o) {
        var names = new List<string>();
        var seen = new HashSet<string>();
        void Add(string n) { if (seen.Add(n)) names.Add(n); }

        if (o is IDictionary d) {
            foreach (object k in d.Keys) if (k is string s) Add(s);
            return names;
        }
        if (o is IDictionary<string, object?> gd) {
            foreach (string k in gd.Keys) Add(k);
            return names;
        }
        bool isStatic = o is Type;
        Type t = isStatic ? (Type)o : o.GetType();
        BindingFlags flags = isStatic ? Static : Instance;
        if (isStatic) foreach (Type n in t.GetNestedTypes(BindingFlags.Public)) Add(n.Name);
        foreach (FieldInfo f in t.GetFields(flags)) Add(f.Name);
        foreach (PropertyInfo p in t.GetProperties(flags)) if (p.GetIndexParameters().Length == 0) Add(p.Name);
        foreach (MethodInfo m in t.GetMethods(flags)) {
            if (!m.IsSpecialName && (m.DeclaringType != typeof(object))) Add(m.Name);
        }
        return names;
    }

    /* ---------------------------------------------------------------- */
    /* Elements                                                         */
    /* ---------------------------------------------------------------- */

    public static long Length(object o) => o switch {
        Array a when a.Rank == 1 => a.Length,
        IList l => l.Count,
        _ => throw Fail.NotCapable("value has no elements"),
    };

    static int Index(object o, long i, bool append) {
        long n = Length(o);
        if ((i < 0) || (i > n) || ((i == n) && !append)) throw Fail.NotFound("index out of range");
        return (int)i;
    }

    public static object? GetIndex(object o, long i) {
        int k = Index(o, i, false);
        return o is Array a ? a.GetValue(k) : ((IList)o)[k];
    }

    // The element type of a collection: T of the generic interface 'generic' it implements (argument 'arg'), else object.
    static Type ValueType(Type t, Type generic, int arg) {
        foreach (Type i in t.GetInterfaces()) {
            if (i.IsGenericType && (i.GetGenericTypeDefinition() == generic)) return i.GetGenericArguments()[arg];
        }
        return typeof(object);
    }

    static Type ElementType(object o) =>
        o is Array a ? a.GetType().GetElementType()! : ValueType(o.GetType(), typeof(IList<>), 0);

    public static void SetIndex(object o, long i, object? v) {
        if (o is Array a) {
            a.SetValue(Coerce(v, ElementType(o)), Index(o, i, false));
            return;
        }
        var l = (IList)o;
        int k = Index(o, i, true);     // one past the end appends
        object? e = Coerce(v, ElementType(o));
        if (k < l.Count) {
            if (l.IsReadOnly) throw Fail.NotCapable("the list is read-only");
            l[k] = e;
        }
        else {
            if (l.IsReadOnly || l.IsFixedSize) throw Fail.NotCapable("the list can't change size");
            l.Add(e);
        }
    }

    // Replaces 'remove' elements at 'offset' (checked by the core) with 'insert'. Lists only: arrays have a fixed size.
    public static void Splice(object o, long offset, long remove, object?[] insert) {
        if (o is Array) throw Fail.NotCapable("a .NET array has a fixed size");
        if (o is not IList l) throw Fail.NotCapable("value has no elements");
        if (l.IsReadOnly || l.IsFixedSize) throw Fail.NotCapable("the list can't change size");
        Type et = ElementType(o);
        object?[] items = insert.Select(x => Coerce(x, et)).ToArray();
        int at = (int)offset;
        for (long k = 0; k < remove; k++) l.RemoveAt(at);
        for (int k = 0; k < items.Length; k++) l.Insert(at + k, items[k]);
    }

    /* ---------------------------------------------------------------- */
    /* Calls                                                            */
    /* ---------------------------------------------------------------- */

    public static object? Call(object f, object?[] args, Kw? kw) {
        switch (f) {
            case MethodRef r:
                return InvokeOn(r.Cls, r.Target, r.Name, args, kw);
            case Delegate d: {
                Overloads o = invokers.GetOrAdd(d.GetType(), t => new Overloads([t.GetMethod("Invoke")!]));
                (MethodBase best, object?[] a) = Choose(o, args, kw, d.GetType().Name);
                return best.Invoke(d, NoWrap, null, a, null);
            }
            case Foreign x:
                // A hint names a .NET overload: it can't go on to another language (exact or refused).
                if (Hint != null) throw new Failure(Status.NotSupported, "NotSupportedException", "a hint applies to .NET overloads only");
                return x.CallWith(args, kw);
            default:
                throw Fail.NotCapable("value is not callable");
        }
    }

    public static object? Invoke(object o, string name, object?[] args, Kw? kw) =>
        o is Type t ? InvokeOn(t, null, name, args, kw) : InvokeOn(o.GetType(), o, name, args, kw);

    static object? InvokeOn(Type t, object? target, string name, object?[] args, Kw? kw) {
        bool isStatic = target == null;
        Overloads cands = methods.GetOrAdd((t, name, isStatic), k => new Overloads(
            k.Item1.GetMethods(k.Item3 ? Static : Instance).Where(m => (m.Name == k.Item2) && !m.IsSpecialName).ToArray<MethodBase>()));
        if (cands.Methods.Length == 0) {
            // A callable member that isn't a method (a delegate in a field or a dictionary): call it.
            object? member = GetMember((object?)target ?? t, name);
            if ((member is Delegate) || (member is MethodRef) || (member is Foreign)) return Call(member, args, kw);
            throw Fail.NotCapable($"{name} is not callable");
        }
        (MethodBase best, object?[] a) = Choose(cands, args, kw, name);
        return best.Invoke(target, NoWrap, null, a, null);
    }

    public static object New(object cls, object?[] args, Kw? kw) {
        if (cls is not Type t) throw Fail.NotCapable("not a class");
        if (!Instantiable(t)) throw Fail.NotCapable($"{t.Name} can't be instantiated");
        Overloads ctors = constructors.GetOrAdd(t, k => new Overloads(k.GetConstructors().ToArray<MethodBase>()));
        if ((ctors.Methods.Length == 0) || (t.IsValueType && (args.Length == 0) && (kw == null))) {
            return Activator.CreateInstance(t)!;     // a struct's default value
        }
        (MethodBase best, object?[] a) = Choose(ctors, args, kw, t.Name);
        return ((ConstructorInfo)best).Invoke(NoWrap, null, a, null);
    }

    /* ---------------------------------------------------------------- */
    /* Overload resolution: score how well each argument converts       */
    /* ---------------------------------------------------------------- */

    const int NO = int.MaxValue;

    /*
     * Resolution is cached: the candidates of each (type, name, static) with
     * their parameters, and, for positional calls of up to 4 arguments, the
     * overload chosen for each signature: the arguments' types, and for an
     * integer which types it fits, for a string whether it could be a char
     * (what Score looks at besides types).
     */
    sealed class Overloads(MethodBase[] methods) {
        public readonly MethodBase[] Methods = methods;
        public readonly ParameterInfo[][] Params = methods.Select(m => m.GetParameters()).ToArray();
        public readonly ConcurrentDictionary<Sig, (int Best, int[] Slots)> Chosen = new();
    }

    readonly record struct Sig(int N, Type? T0, int E0, Type? T1, int E1, Type? T2, int E2, Type? T3, int E3);

    static readonly ConcurrentDictionary<(Type, string, bool), Overloads> methods = new();
    static readonly ConcurrentDictionary<Type, Overloads> constructors = new();
    static readonly ConcurrentDictionary<Type, Overloads> invokers = new();

    static (Type?, int) ArgSig(object? a) => a switch {
        null => (null, 0),
        long v => (typeof(long), (Fits(v, typeof(int)) ? 1 : 0) | (Fits(v, typeof(short)) ? 2 : 0) | (Fits(v, typeof(sbyte)) ? 4 : 0)
            | (Fits(v, typeof(byte)) ? 8 : 0) | (Fits(v, typeof(ushort)) ? 16 : 0) | (Fits(v, typeof(uint)) ? 32 : 0) | (v >= 0 ? 64 : 0)),
        string s => (typeof(string), s.Length == 1 ? 1 : 0),
        _ => (a.GetType(), 0),
    };

    static Sig? SigOf(object?[] args) {
        if (args.Length > 4) return null;
        (Type? t0, int e0) = args.Length > 0 ? ArgSig(args[0]) : (null, 0);
        (Type? t1, int e1) = args.Length > 1 ? ArgSig(args[1]) : (null, 0);
        (Type? t2, int e2) = args.Length > 2 ? ArgSig(args[2]) : (null, 0);
        (Type? t3, int e3) = args.Length > 3 ? ArgSig(args[3]) : (null, 0);
        return new Sig(args.Length, t0, e0, t1, e1, t2, e2, t3, e3);
    }

    static readonly Dictionary<Type, string> aliases = new() {
        [typeof(bool)] = "bool", [typeof(byte)] = "byte", [typeof(sbyte)] = "sbyte", [typeof(short)] = "short",
        [typeof(ushort)] = "ushort", [typeof(int)] = "int", [typeof(uint)] = "uint", [typeof(long)] = "long",
        [typeof(ulong)] = "ulong", [typeof(float)] = "float", [typeof(double)] = "double", [typeof(decimal)] = "decimal",
        [typeof(char)] = "char", [typeof(string)] = "string", [typeof(object)] = "object", [typeof(nint)] = "nint",
        [typeof(nuint)] = "nuint",
    };

    static bool Named(Type t, string name) =>
        (t.FullName == name) || (aliases.TryGetValue(t, out string? a) && (a == name));

    static bool Matches(ParameterInfo[] ps, string[] hint) {
        if (ps.Length != hint.Length) return false;
        for (int i = 0; i < ps.Length; i++) if (!Named(ps[i].ParameterType, hint[i])) return false;
        return true;
    }

    /*
     * Where each argument goes for candidate m: slots[i] is the index in
     * args (positional, then keyword values after them) or -1 for a
     * default. The score, or NO if the arguments don't fit.
     */
    static int Bind(MethodBase m, ParameterInfo[] ps, object?[] args, Kw? kw, out int[] slots) {
        slots = new int[ps.Length];
        Array.Fill(slots, -1);
        if (m.ContainsGenericParameters || (args.Length > ps.Length)) return NO;
        if (ps.Any(p => p.ParameterType.IsByRef || p.ParameterType.IsPointer)) return NO;
        int score = 0;
        for (int i = 0; i < args.Length; i++) {
            int s = Score(args[i], ps[i].ParameterType);
            if (s == NO) return NO;
            score += s;
            slots[i] = i;
        }
        if (kw != null) {
            for (int k = 0; k < kw.Names.Length; k++) {
                int j = Array.FindIndex(ps, p => p.Name == kw.Names[k]);
                if ((j < 0) || (slots[j] >= 0)) return NO;     // no such parameter, or given twice
                int s = Score(kw.Values[k], ps[j].ParameterType);
                if (s == NO) return NO;
                score += s;
                slots[j] = args.Length + k;
            }
        }
        for (int j = 0; j < ps.Length; j++) {
            if ((slots[j] < 0) && !ps[j].IsOptional) return NO;
        }
        return score;
    }

    static object?[] Convert(ParameterInfo[] ps, object?[] args, Kw? kw, int[] slots) {
        var a = new object?[ps.Length];
        for (int j = 0; j < ps.Length; j++) {
            int k = slots[j];
            if (k < 0) a[j] = ps[j].HasDefaultValue ? ps[j].DefaultValue : Type.Missing;
            else a[j] = Coerce(k < args.Length ? args[k] : kw!.Values[k - args.Length], ps[j].ParameterType);
        }
        return a;
    }

    static (MethodBase, object?[]) Choose(Overloads o, object?[] args, Kw? kw, string what) {
        string? hint = Hint;
        Hint = null;
        int n = args.Length + (kw?.Names.Length ?? 0);
        int[]? slots;
        if (hint != null) {
            string[] want = hint.Replace(" ", "").Split(',', StringSplitOptions.RemoveEmptyEntries);
            for (int i = 0; i < o.Methods.Length; i++) {
                if (!Matches(o.Params[i], want)) continue;
                if (Bind(o.Methods[i], o.Params[i], args, kw, out slots) == NO) throw Fail.Type($"the arguments don't fit {what}({hint})");
                return (o.Methods[i], Convert(o.Params[i], args, kw, slots));
            }
            throw Fail.NotFound($"no overload {what}({hint})");
        }
        Sig? sig = kw == null ? SigOf(args) : null;
        if ((sig != null) && o.Chosen.TryGetValue(sig.Value, out var chosen)) {
            return (o.Methods[chosen.Best], Convert(o.Params[chosen.Best], args, null, chosen.Slots));
        }
        int best = -1;
        int[]? bestSlots = null;
        int bestScore = NO, ties = 0;
        for (int i = 0; i < o.Methods.Length; i++) {
            int s = Bind(o.Methods[i], o.Params[i], args, kw, out slots);
            if (s == NO) continue;
            if (s < bestScore) { best = i; bestSlots = slots; bestScore = s; ties = 0; }
            else if (s == bestScore) ties++;
        }
        if (best < 0) throw Fail.Type($"no overload of {what} takes these {n} argument(s)");
        if (ties > 0) throw Fail.Type($"ambiguous call to {what}: {ties + 1} overloads fit equally well");
        if (sig != null) o.Chosen.TryAdd(sig.Value, (best, bestSlots!));
        return (o.Methods[best], Convert(o.Params[best], args, kw, bestSlots!));
    }

    static bool Fits(long v, Type t) =>
        t == typeof(long) ? true
        : t == typeof(int) ? (v >= int.MinValue) && (v <= int.MaxValue)
        : t == typeof(short) ? (v >= short.MinValue) && (v <= short.MaxValue)
        : t == typeof(sbyte) ? (v >= sbyte.MinValue) && (v <= sbyte.MaxValue)
        : t == typeof(byte) ? (v >= byte.MinValue) && (v <= byte.MaxValue)
        : t == typeof(ushort) ? (v >= ushort.MinValue) && (v <= ushort.MaxValue)
        : t == typeof(uint) ? (v >= uint.MinValue) && (v <= uint.MaxValue)
        : t == typeof(ulong) || t == typeof(nuint) ? v >= 0
        : t == typeof(nint) || t == typeof(System.Numerics.BigInteger) || t == typeof(Int128);

    static bool IsDelegate(Type t) =>
        typeof(Delegate).IsAssignableFrom(t) && (t != typeof(Delegate)) && (t != typeof(MulticastDelegate));

    /** Lower is better; NO if 'a' can't be passed as 'p'. */
    static int Score(object? a, Type p) {
        Type? under = Nullable.GetUnderlyingType(p);
        if (a == null) return (!p.IsValueType || (under != null)) ? 1 : NO;
        Type t = under ?? p;
        if (a.GetType() == t) return 0;
        if (a is long v) {
            if (Fits(v, t)) return 1;
            // double first: Math.Round(5) has double and decimal, Math.Sign and others float too.
            if (t == typeof(double)) return 3;
            if (t == typeof(decimal)) return 4;
            if (t == typeof(float)) return 5;
        }
        if ((a is double) && (t == typeof(float))) return 2;
        if ((a is string s) && (t == typeof(char)) && (s.Length == 1)) return 3;
        if (t.IsInstanceOfType(a)) return t == typeof(object) ? 6 : 2;
        if (IsDelegate(t) && ((a is Foreign) || (a is MethodRef) || (a is Delegate))) return 5;
        if ((a is Foreign) && (EnumerableOf(t) != null)) return 4;     // its iteration, typed
        if ((a is Foreign) && t.IsInterface) return 5;
        return NO;
    }

    public static object? Coerce(object? a, Type p) {
        if (Score(a, p) == NO) {
            throw Fail.Type(a == null ? $"null isn't a {p.Name}" : $"can't convert {a.GetType().Name} to {p.Name}");
        }
        if (a == null) return null;
        Type t = Nullable.GetUnderlyingType(p) ?? p;
        if (t.IsInstanceOfType(a)) return a;
        if (a is long v) {
            if (t == typeof(int)) return (int)v;
            if (t == typeof(short)) return (short)v;
            if (t == typeof(sbyte)) return (sbyte)v;
            if (t == typeof(byte)) return (byte)v;
            if (t == typeof(ushort)) return (ushort)v;
            if (t == typeof(uint)) return (uint)v;
            if (t == typeof(ulong)) return (ulong)v;
            if (t == typeof(nint)) return (nint)v;
            if (t == typeof(nuint)) return (nuint)v;
            if (t == typeof(double)) return (double)v;
            if (t == typeof(float)) return (float)v;
            if (t == typeof(decimal)) return (decimal)v;
            if (t == typeof(System.Numerics.BigInteger)) return new System.Numerics.BigInteger(v);
            if (t == typeof(Int128)) return (Int128)v;
        }
        if ((a is double d) && (t == typeof(float))) return (float)d;
        if ((a is string s) && (t == typeof(char))) return s[0];
        if (IsDelegate(t)) return Delegates.Make(a, t);
        if ((a is Foreign g) && (EnumerableOf(t) is Type et)) return typed.MakeGenericMethod(et).Invoke(null, [g]);
        if ((a is Foreign f) && t.IsInterface) return ForeignProxy.Create(f, t);
        throw Fail.Type($"can't convert {a.GetType().Name} to {p.Name}");
    }
}


/* -------------------------------------------------------------------- */
/* Callables as delegates                                               */
/* -------------------------------------------------------------------- */

// A callable (a foreign value, a method, another delegate) as a delegate of a given type.
static class Delegates {
    static readonly ConcurrentDictionary<Type, Func<object, Delegate>> factories = new();
    static readonly MethodInfo callAny = typeof(Delegates).GetMethod(nameof(CallAny), BindingFlags.NonPublic | BindingFlags.Static)!;
    static readonly MethodInfo coerce = typeof(Protocol).GetMethod(nameof(Protocol.Coerce))!;

    static object? CallAny(object target, object?[] args) => Protocol.Call(target, args, null);

    // (target) => (p1, p2...) => (R)Coerce(CallAny(target, [p1, p2...]), typeof(R)), compiled once per delegate type.
    static Func<object, Delegate> Factory(Type dt) {
        MethodInfo inv = dt.GetMethod("Invoke")!;
        if (inv.GetParameters().Any(p => p.ParameterType.IsByRef)) throw Fail.Type($"{dt.Name} has ref parameters");
        ParameterExpression target = Expression.Parameter(typeof(object), "target");
        ParameterExpression[] ps = inv.GetParameters().Select(p => Expression.Parameter(p.ParameterType, p.Name)).ToArray();
        Expression args = Expression.NewArrayInit(typeof(object), ps.Select(p => (Expression)Expression.Convert(p, typeof(object))));
        Expression body = Expression.Call(callAny, target, args);
        if (inv.ReturnType != typeof(void)) {
            body = Expression.Convert(Expression.Call(coerce, body, Expression.Constant(inv.ReturnType, typeof(Type))), inv.ReturnType);
        }
        LambdaExpression inner = Expression.Lambda(dt, body, ps);
        return Expression.Lambda<Func<object, Delegate>>(inner, target).Compile();
    }

    public static Delegate Make(object callable, Type dt) {
        Delegate d = factories.GetOrAdd(dt, Factory)(callable);
        if (Conv.ForeignOf(callable) is Foreign f) Conv.Wrapped(d, f);
        return d;
    }
}


// A foreign value passed where an interface is expected: its methods invoke the value's by name.
public class ForeignProxy : DispatchProxy {
    Foreign? foreign;
    MethodInfo? sam;

    internal static object Create(Foreign f, Type iface) {
        object p = Create(iface, typeof(ForeignProxy));
        var fp = (ForeignProxy)p;
        fp.foreign = f;
        MethodInfo[] abstracts = iface.GetMethods().Where(m => m.IsAbstract && !m.IsSpecialName).ToArray();
        fp.sam = abstracts.Length == 1 ? abstracts[0] : null;     // a single abstract method calls the value itself
        Conv.Wrapped(p, f);
        return p;
    }

    protected override object? Invoke(MethodInfo? m, object?[]? args) {
        Foreign f = foreign!;
        args ??= [];
        object? r;
        if (m == sam) r = f.Call(args);
        else if (m!.IsSpecialName && m.Name.StartsWith("get_") && (args.Length == 0)) r = f.Get(m.Name[4..]);
        else if (m.IsSpecialName && m.Name.StartsWith("set_") && (args.Length == 1)) { f.Set(m.Name[4..], args[0]); r = null; }
        else r = f.Invoke(m.Name, args);
        return m!.ReturnType == typeof(void) ? null : Protocol.Coerce(r, m.ReturnType);
    }
}
